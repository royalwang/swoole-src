/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "swoole.h"
#include "server.h"

#include <sys/wait.h>

typedef struct
{
    uint8_t  reloading;
    uint8_t  reload_all_worker;
    uint8_t  reload_task_worker;
    uint8_t  read_message;
    uint32_t reload_init;
    uint32_t reload_worker_i;
    uint32_t reload_worker_num;
    swWorker *reload_workers;
} swManagerProcess;

typedef struct
{
    uint32_t reload_worker_num;
    swWorker *workers;
} swReloadWorker;

static int swManager_loop(swFactory *factory);
static void swManager_signal_handler(int sig);
static pid_t swManager_spawn_worker(swFactory *factory, int worker_id);
static void swManager_check_exit_status(swServer *serv, int worker_id, pid_t pid, int status);

static swManagerProcess ManagerProcess;

static void swManager_onTimer(swTimer *timer, swTimer_node *tnode)
{
    swServer *serv = (swServer *) tnode->data;
    if (serv->hooks[SW_SERVER_HOOK_MANAGER_TIMER])
    {
        swServer_call_hook(serv, SW_SERVER_HOOK_MANAGER_TIMER, serv);
    }
}

static void swManager_kill_timeout_process(swTimer *timer, swTimer_node *tnode)
{
    uint32_t i;
    swReloadWorker *reload_info = (swReloadWorker *) tnode->data;
    swWorker *workers = reload_info->workers;

    for (i = 0; i < reload_info->reload_worker_num; i++)
    {
        pid_t pid = workers[i].pid;
        if (swKill(pid, 0) == -1)
        {
            continue;
        }
        if (swKill(pid, SIGKILL) < 0)
        {
            swSysError("swKill(%d, SIGKILL) [%d] failed.", pid, i);
        }
        else
        {
            swoole_error_log(SW_LOG_WARNING, SW_ERROR_SERVER_WORKER_EXIT_TIMEOUT,
                    "[Manager] Worker#%d[pid=%d] exit timeout, forced kill.", workers[i].id, pid);
        }
    }
    sw_free(workers);
    sw_free(reload_info);
}

static void swManager_add_timeout_killer(swServer *serv, swWorker *workers, int n)
{
    if (!serv->max_wait_time)
    {
        return;
    }
    /**
     * separate old workers, free memory in the timer
     */
    swWorker *reload_workers = (swWorker *) sw_malloc(sizeof(swWorker) * n);
    swReloadWorker *reload_info = (swReloadWorker *) sw_malloc(sizeof(swReloadWorker));
    memcpy(reload_workers, workers, sizeof(swWorker) * n);
    reload_info->reload_worker_num = n;
    reload_info->workers = reload_workers;

    swTimer_add(&SwooleG.timer, (long) (serv->max_wait_time * 1000), 0, reload_info, swManager_kill_timeout_process);
}

//create worker child proccess
int swManager_start(swFactory *factory)
{
    swFactoryProcess *object = (swFactoryProcess *) factory->object;
    swServer *serv = (swServer *) factory->ptr;

    int i;
    pid_t pid;

    object->pipes = (swPipe *) sw_calloc(serv->worker_num, sizeof(swPipe));
    if (object->pipes == NULL)
    {
        swError("malloc[worker_pipes] failed. Error: %s [%d]", strerror(errno), errno);
        return SW_ERR;
    }

    for (i = 0; i < serv->worker_num; i++)
    {
        if (swPipeUnsock_create(&object->pipes[i], 1, SOCK_DGRAM) < 0)
        {
            return SW_ERR;
        }
        serv->workers[i].pipe_master = object->pipes[i].getFd(&object->pipes[i], SW_PIPE_MASTER);
        serv->workers[i].pipe_worker = object->pipes[i].getFd(&object->pipes[i], SW_PIPE_WORKER);
        serv->workers[i].pipe_object = &object->pipes[i];
        swServer_store_pipe_fd(serv, serv->workers[i].pipe_object);
    }

    if (serv->task_worker_num > 0)
    {
        if (swServer_create_task_worker(serv) < 0)
        {
            return SW_ERR;
        }
        swTaskWorker_init(serv);

        swWorker *worker;
        for (i = 0; i < serv->task_worker_num; i++)
        {
            worker = &serv->gs->task_workers.workers[i];
            if (swServer_worker_create(serv, worker) < 0)
            {
                return SW_ERR;
            }
            if (serv->task_ipc_mode == SW_TASK_IPC_UNIXSOCK)
            {
                swServer_store_pipe_fd(serv, worker->pipe_object);
            }
        }
    }

    //User Worker Process
    if (serv->user_worker_num > 0)
    {
        serv->user_workers = (swWorker *) SwooleG.memory_pool->alloc(SwooleG.memory_pool, serv->user_worker_num * sizeof(swWorker));
        if (serv->user_workers == NULL)
        {
            swoole_error_log(SW_LOG_ERROR, SW_ERROR_SYSTEM_CALL_FAIL, "gmalloc[server->user_workers] failed.");
            return SW_ERR;
        }
        swUserWorker_node *user_worker;
        i = 0;
        LL_FOREACH(serv->user_worker_list, user_worker)
        {
            memcpy(&serv->user_workers[i], user_worker->worker, sizeof(swWorker));
            if (swServer_worker_create(serv, &serv->user_workers[i]) < 0)
            {
                return SW_ERR;
            }
            i++;
        }
    }

    serv->message_box = swChannel_new(65536, sizeof(swWorkerStopMessage), SW_CHAN_LOCK | SW_CHAN_SHM);
    if (serv->message_box == NULL)
    {
        return SW_ERR;
    }

    pid = swoole_fork();
    switch (pid)
    {
    //fork manager process
    case 0:
        //wait master process
        SW_START_SLEEP;
        if (!serv->gs->start)
        {
            return SW_OK;
        }
        swServer_close_port(serv, SW_TRUE);
        /**
         * create task worker process
         */
        if (serv->task_worker_num > 0)
        {
            swProcessPool_start(&serv->gs->task_workers);
        }
        /**
         * create worker process
         */
        for (i = 0; i < serv->worker_num; i++)
        {
            pid = swManager_spawn_worker(factory, i);
            if (pid < 0)
            {
                swError("fork() failed.");
                return SW_ERR;
            }
            else
            {
                serv->workers[i].pid = pid;
            }
        }
        /**
         * create user worker process
         */
        if (serv->user_worker_list)
        {
            swUserWorker_node *user_worker;
            LL_FOREACH(serv->user_worker_list, user_worker)
            {
                /**
                 * store the pipe object
                 */
                if (user_worker->worker->pipe_object)
                {
                    swServer_store_pipe_fd(serv, user_worker->worker->pipe_object);
                }
                swManager_spawn_user_worker(serv, user_worker->worker);
            }
        }

        SwooleG.process_type = SW_PROCESS_MANAGER;
        SwooleG.pid = getpid();
        exit(swManager_loop(factory));
        break;

        //master process
    default:
        serv->gs->manager_pid = pid;
        break;
    case -1:
        swError("fork() failed.");
        return SW_ERR;
    }
    return SW_OK;
}

static void swManager_check_exit_status(swServer *serv, int worker_id, pid_t pid, int status)
{
    if (status != 0)
    {
        swWarn(
            "worker#%d[pid=%d] abnormal exit, status=%d, signal=%d" "%s",
            worker_id, pid, WEXITSTATUS(status), WTERMSIG(status),
            WTERMSIG(status) == SIGSEGV ? "\n" SWOOLE_BUG_REPORT : ""
        );
        if (serv->onWorkerError != NULL)
        {
            serv->onWorkerError(serv, worker_id, pid, WEXITSTATUS(status), WTERMSIG(status));
        }
    }
}

static int swManager_loop(swFactory *factory)
{
    int pid, new_pid;
    int i;
    pid_t reload_worker_pid = 0;

    int status;

    SwooleG.use_signalfd = 0;
    SwooleG.main_reactor = NULL;
    SwooleG.enable_coroutine = 0;

    memset(&ManagerProcess, 0, sizeof(ManagerProcess));

    swServer *serv = (swServer *) factory->ptr;

    if (serv->hooks[SW_SERVER_HOOK_MANAGER_START])
    {
        swServer_call_hook(serv, SW_SERVER_HOOK_MANAGER_START, serv);
    }

    if (serv->onManagerStart)
    {
        serv->onManagerStart(serv);
    }

    ManagerProcess.reload_workers = (swWorker *) sw_calloc(serv->worker_num + serv->task_worker_num, sizeof(swWorker));
    if (ManagerProcess.reload_workers == NULL)
    {
        swError("malloc[reload_workers] failed");
        return SW_ERR;
    }

    //for reload
    swSignal_add(SIGHUP, NULL);
    swSignal_add(SIGTERM, swManager_signal_handler);
    swSignal_add(SIGUSR1, swManager_signal_handler);
    swSignal_add(SIGUSR2, swManager_signal_handler);
    swSignal_add(SIGIO, swManager_signal_handler);
#ifdef SIGRTMIN
    swSignal_add(SIGRTMIN, swManager_signal_handler);
#endif
    //swSignal_add(SIGINT, swManager_signal_handler);

    if (serv->manager_alarm > 0)
    {
        swTimer_add(&SwooleG.timer, (long) (serv->manager_alarm * 1000), 1, serv, swManager_onTimer);
    }

    while (SwooleG.running > 0)
    {
        _wait: pid = wait(&status);

        if (ManagerProcess.read_message)
        {
            swWorkerStopMessage msg;
            while (swChannel_pop(serv->message_box, &msg, sizeof(msg)) > 0)
            {
                if (SwooleG.running == 0)
                {
                    continue;
                }
                if (msg.worker_id >= serv->worker_num)
                {
                    swManager_spawn_task_worker(serv, swServer_get_worker(serv, msg.worker_id));
                }
                else
                {
                    pid_t new_pid = swManager_spawn_worker(factory, msg.worker_id);
                    if (new_pid > 0)
                    {
                        serv->workers[msg.worker_id].pid = new_pid;
                    }
                }
            }
            ManagerProcess.read_message = 0;
        }

        if (SwooleG.signal_alarm == 1)
        {
            SwooleG.signal_alarm = 0;
            swTimer_select(&SwooleG.timer);
        }

        if (pid < 0)
        {
            if (ManagerProcess.reloading == 0)
            {
                error: if (errno > 0 && errno != EINTR)
                {
                    swSysError("wait() failed.");
                }
                continue;
            }
            //reload task & event workers
            else if (ManagerProcess.reload_all_worker == 1)
            {
                swNotice("Server is reloading all workers now.");
                if (ManagerProcess.reload_init == 0)
                {
                    ManagerProcess.reload_init = 1;
                    memcpy(ManagerProcess.reload_workers, serv->workers, sizeof(swWorker) * serv->worker_num);

                    swManager_add_timeout_killer(serv, serv->workers, serv->worker_num);

                    ManagerProcess.reload_worker_num = serv->worker_num;
                    if (serv->task_worker_num > 0)
                    {
                        memcpy(ManagerProcess.reload_workers + serv->worker_num, serv->gs->task_workers.workers,
                                sizeof(swWorker) * serv->task_worker_num);
                        ManagerProcess.reload_worker_num += serv->task_worker_num;

                        swManager_add_timeout_killer(serv, serv->gs->task_workers.workers, serv->task_worker_num);
                    }

                    ManagerProcess.reload_all_worker = 0;
                    if (serv->reload_async)
                    {
                        for (i = 0; i < serv->worker_num; i++)
                        {
                            if (swKill(ManagerProcess.reload_workers[i].pid, SIGTERM) < 0)
                            {
                                swSysError("swKill(%d, SIGTERM) [%d] failed.", ManagerProcess.reload_workers[i].pid, i);
                            }
                        }
                        ManagerProcess.reload_worker_i = serv->worker_num;
                    }
                    else
                    {
                        ManagerProcess.reload_worker_i = 0;
                    }
                }
                goto kill_worker;
            }
            //only reload task workers
            else if (ManagerProcess.reload_task_worker == 1)
            {
                if (serv->task_worker_num == 0)
                {
                    swWarn("cannot reload task workers, task workers is not started.");
                    continue;
                }
                swNotice("Server is reloading task workers now.");
                if (ManagerProcess.reload_init == 0)
                {
                    memcpy(ManagerProcess.reload_workers, serv->gs->task_workers.workers, sizeof(swWorker) * serv->task_worker_num);
                    swManager_add_timeout_killer(serv, serv->gs->task_workers.workers, serv->task_worker_num);
                    ManagerProcess.reload_worker_num = serv->task_worker_num;
                    ManagerProcess.reload_worker_i = 0;
                    ManagerProcess.reload_init = 1;
                    ManagerProcess.reload_task_worker = 0;
                }
                goto kill_worker;
            }
            else
            {
                goto error;
            }
        }
        if (SwooleG.running == 1)
        {
            //event workers
            for (i = 0; i < serv->worker_num; i++)
            {
                //compare PID
                if (pid != serv->workers[i].pid)
                {
                    continue;
                }

                if (WIFSTOPPED(status) && serv->workers[i].tracer)
                {
                    serv->workers[i].tracer(&serv->workers[i]);
                    serv->workers[i].tracer = NULL;
                    goto _wait;
                }

                //Check the process return code and signal
                swManager_check_exit_status(serv, i, pid, status);

                while (1)
                {
                    new_pid = swManager_spawn_worker(factory, i);
                    if (new_pid < 0)
                    {
                        SW_START_SLEEP;
                        continue;
                    }
                    else
                    {
                        serv->workers[i].pid = new_pid;
                        break;
                    }
                }
            }

            swWorker *exit_worker;
            //task worker
            if (serv->gs->task_workers.map)
            {
                exit_worker = (swWorker *) swHashMap_find_int(serv->gs->task_workers.map, pid);
                if (exit_worker != NULL)
                {
                    if (WIFSTOPPED(status) && exit_worker->tracer)
                    {
                        exit_worker->tracer(exit_worker);
                        exit_worker->tracer = NULL;
                        goto _wait;
                    }
                    swManager_check_exit_status(serv, exit_worker->id, pid, status);
                    swManager_spawn_task_worker(serv, exit_worker);
                }
            }
            //user process
            if (serv->user_worker_map != NULL)
            {
                swManager_wait_other_worker(&serv->gs->event_workers, pid, status);
            }
            if (pid == reload_worker_pid && ManagerProcess.reloading == 1)
            {
                ManagerProcess.reload_worker_i++;
            }
        }
        //reload worker
        kill_worker: if (ManagerProcess.reloading == 1)
        {
            //reload finish
            if (ManagerProcess.reload_worker_i >= ManagerProcess.reload_worker_num)
            {
                reload_worker_pid = ManagerProcess.reload_worker_i = ManagerProcess.reload_init = ManagerProcess.reloading = 0;
                continue;
            }
            reload_worker_pid = ManagerProcess.reload_workers[ManagerProcess.reload_worker_i].pid;
            if (swKill(reload_worker_pid, SIGTERM) < 0)
            {
                if (errno == ECHILD || errno == ESRCH)
                {
                    ManagerProcess.reload_worker_i++;
                    goto kill_worker;
                }
                swSysError("swKill(%d, SIGTERM) [%d] failed.", ManagerProcess.reload_workers[ManagerProcess.reload_worker_i].pid, ManagerProcess.reload_worker_i);
            }
        }
    }

    sw_free(ManagerProcess.reload_workers);
    swSignal_none();
    //kill all child process
    for (i = 0; i < serv->worker_num; i++)
    {
        swTrace("[Manager]kill worker processor");
        swKill(serv->workers[i].pid, SIGTERM);
    }
    //kill and wait task process
    if (serv->task_worker_num > 0)
    {
        swProcessPool_shutdown(&serv->gs->task_workers);
    }
    //wait child process
    for (i = 0; i < serv->worker_num; i++)
    {
        if (swWaitpid(serv->workers[i].pid, &status, 0) < 0)
        {
            swSysError("waitpid(%d) failed.", serv->workers[i].pid);
        }
    }
    //kill all user process
    if (serv->user_worker_map)
    {
        swManager_kill_user_worker(serv);
    }

    if (serv->onManagerStop)
    {
        serv->onManagerStop(serv);
    }

    return SW_OK;
}

static pid_t swManager_spawn_worker(swFactory *factory, int worker_id)
{
    pid_t pid;
    int ret;

    pid = swoole_fork();

    //fork() failed
    if (pid < 0)
    {
        swWarn("Fork Worker failed. Error: %s [%d]", strerror(errno), errno);
        return SW_ERR;
    }
    //worker child processor
    else if (pid == 0)
    {
        ret = swWorker_loop(factory, worker_id);
        exit(ret);
    }
    //parent,add to writer
    else
    {
        return pid;
    }
}

static void swManager_signal_handler(int sig)
{
    switch (sig)
    {
    case SIGTERM:
        SwooleG.running = 0;
        break;
        /**
         * reload all workers
         */
    case SIGUSR1:
        if (ManagerProcess.reloading == 0)
        {
            ManagerProcess.reloading = 1;
            ManagerProcess.reload_all_worker = 1;
        }
        break;
        /**
         * only reload task workers
         */
    case SIGUSR2:
        if (ManagerProcess.reloading == 0)
        {
            ManagerProcess.reloading = 1;
            ManagerProcess.reload_task_worker = 1;
        }
        break;
    case SIGIO:
        ManagerProcess.read_message = 1;
        break;
    case SIGALRM:
        SwooleG.signal_alarm = 1;
        break;
    default:
#ifdef SIGRTMIN
        if (sig == SIGRTMIN)
        {
            swServer_reopen_log_file(SwooleG.serv);
        }
#endif
        break;
    }
}

int swManager_wait_other_worker(swProcessPool *pool, pid_t pid, int status)
{
    swServer *serv = (swServer *) pool->ptr;
    swWorker *exit_worker;

    if (serv->gs->task_workers.map)
    {
        exit_worker = (swWorker *) swHashMap_find_int(serv->gs->task_workers.map, pid);
        if (exit_worker)
        {
            swManager_check_exit_status(serv, exit_worker->id, pid, status);
            return swManager_spawn_task_worker(serv, exit_worker);
        }
    }

    if (serv->user_worker_map)
    {
        exit_worker = (swWorker *) swHashMap_find_int(serv->user_worker_map, pid);
        if (exit_worker != NULL)
        {
            swManager_check_exit_status(serv, exit_worker->id, pid, status);
            return swManager_spawn_user_worker(serv, exit_worker);
        }
    }

    return SW_ERR;
}

void swManager_kill_user_worker(swServer *serv)
{
    if (!serv->user_worker_map)
    {
        return;
    }
    swWorker* user_worker;
    uint64_t key;
    int __stat_loc;

    //kill user process
    while (1)
    {
        user_worker = (swWorker *) swHashMap_each_int(serv->user_worker_map, &key);
        //hashmap empty
        if (user_worker == NULL)
        {
            break;
        }
        swKill(user_worker->pid, SIGTERM);
    }

    //wait user process
    while (1)
    {
        user_worker = (swWorker *) swHashMap_each_int(serv->user_worker_map, &key);
        //hashmap empty
        if (user_worker == NULL)
        {
            break;
        }
        if (swWaitpid(user_worker->pid, &__stat_loc, 0) < 0)
        {
            swSysError("waitpid(%d) failed.", user_worker->pid);
        }
    }
}

pid_t swManager_spawn_task_worker(swServer *serv, swWorker* worker)
{
    return swProcessPool_spawn(&serv->gs->task_workers, worker);
}

pid_t swManager_spawn_user_worker(swServer *serv, swWorker* worker)
{
    pid_t pid = swoole_fork();

    if (pid < 0)
    {
        swWarn("Fork Worker failed. Error: %s [%d]", strerror(errno), errno);
        return SW_ERR;
    }
    //child
    else if (pid == 0)
    {
        SwooleG.process_type = SW_PROCESS_USERWORKER;
        SwooleWG.worker = worker;
        SwooleWG.id = worker->id;
        worker->pid = getpid();
        //close tcp listen socket
        if (serv->factory_mode == SW_MODE_BASE)
        {
            swServer_close_port(serv, SW_TRUE);
        }
        serv->onUserWorkerStart(serv, worker);
        exit(0);
    }
    //parent
    else
    {
        if (worker->pid)
        {
            swHashMap_del_int(serv->user_worker_map, worker->pid);
        }
        /**
         * worker: local memory
         * serv->user_workers: shared memory
         */
        swServer_get_worker(serv, worker->id)->pid = worker->pid = pid;
        swHashMap_add_int(serv->user_worker_map, pid, worker);
        return pid;
    }
}
