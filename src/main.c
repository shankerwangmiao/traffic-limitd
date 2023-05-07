#define _GNU_SOURCE
#include <stdio.h>
#include <systemd/sd-event.h>
#include <s_task.h>
#include <se_libs.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <log.h>
#include <stdlib.h>
#include <unistd.h>
#include "daemon.h"
#include <time.h>
#include <sys/socket.h>


static const size_t STACK_SIZE = 256*1024;

static struct daemon g_daemon = {0};

static const int NO_JOB_SLEEP_DELAY = 20 * 1000 * 1000;

static enum{
    NOEXIT = 0,
    EXIT_REQ_SENT = 1,
    WAIT_TASKS = 2,
} g_exit_req = NOEXIT;

static int exit_req_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata){
    sd_event_source_disable_unref(s);
    if(g_exit_req == NOEXIT){
        g_exit_req = EXIT_REQ_SENT;
        interrupt_all_tasks((void *)&global_interrupt_reasons.SYS_WILL_EXIT);
    }
    return 0;
}

static int sleep_timer_handler(sd_event_source *s, uint64_t usec, void *userdata){
    if(is_task_empty()){
        log_trace("No job, exit daemon");
        int rc = sd_event_exit(g_daemon.event_loop, 0);
        if(rc < 0){
            log_error("sd_event_exit failed: %s", strerror(-rc));
            /* Ignored */
        }
    }
    return 0;
}

static int install_signals(void){
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    int rc = 0;
    rc = sigprocmask(SIG_BLOCK, &mask, NULL);
    if(rc < 0){
        return -errno;
    }
    return rc;
}

static void client_handler_async(__async__, void *arg){
    enum {
        INT_IO_ERR = 1,
        INT_PROC_END = 2,
    };

    int fd = (int)(uintptr_t)arg;
    alog_trace("handle incoming connection: %d", fd);
    int rc = 0;
    struct msg_stream *stream = NULL;
    rc = init_msg_stream(&stream, g_daemon.event_loop, fd);
    if(rc < 0){
        alog_error("init_msg_stream failed: %s", strerror(-rc));
        goto err_close_fd;
    }
    se_task_register_memory_to_free(__await__, stream, (void (*)(void *))destroy_msg_stream);
    msg_stream_reg_interrupt(__await__, stream, (void *)INT_IO_ERR);

    const struct ucred *cred = msg_stream_get_peer_cred(stream);
    alog_info("Our peer pid=%d, uid=%d", cred->pid, cred->uid);

    char *scope_obj = NULL;
    char *scope_name = NULL;
    rc = start_transient_scope(__await__, g_daemon.sd_bus, cred->pid, &scope_name, &scope_obj);
    if(rc < 0){
        if(rc == -EINTR){
            goto interrupt;
        }
        alog_error("start_transient_scope failed: %s", strerror(-rc));
        goto err_close_stream;
    }
    se_task_register_memory_to_free(__await__, scope_name, free);
    se_task_register_memory_to_free(__await__, scope_obj, free);

    alog_trace("scope_name=%s, scope_obj=%s", scope_name, scope_obj);

    struct pidfd_event *pidfd_event = NULL;
    rc = init_pidfd_event(&pidfd_event, g_daemon.event_loop, cred->pid);
    if(rc < 0){
        alog_error("init_pidfd_event failed: %s", strerror(-rc));
        goto err_close_stream;
    }
    se_task_register_memory_to_free(__await__, pidfd_event, (void (*)(void *))destroy_pidfd_event);
    pidfd_event_reg_interrupt(__await__, pidfd_event, (void *)INT_PROC_END);

    char *cgroup_path = NULL;
    rc = sb_Unit_Get_subprop_string(__await__, g_daemon.sd_bus, scope_obj, "ControlGroup", &cgroup_path);
    if(rc < 0){
        if(rc == -EINTR){
            goto interrupt;
        }
        alog_error("sb_Unit_Get_subprop_string(scope, ControlGroup) failed: %s", strerror(-rc));
        goto err_close_stream;
    }
    se_task_register_memory_to_free(__await__, cgroup_path, free);

    alog_trace("cgroup_path=%s", cgroup_path);

    char buf[256];
    while(1){
        rc = msg_stream_read(__await__, stream, buf, sizeof(buf) - 1, 3*1000*1000);
        if(rc < 0){
            if(rc == -EINTR){
                goto interrupt;
            }
            alog_error("msg_stream_read failed: %s", strerror(-rc));
            goto err_close_stream;
        }else if(rc == 0){
            alog_trace("read eof");
            break;
        }
        int len = rc;
        buf[len++] = '\0';
        alog_trace("read %d bytes: %s", len, buf);
        rc = se_task_usleep(__await__, g_daemon.event_loop, 5*1000*1000);
        if(rc < 0){
            if(rc == -EINTR){
                goto interrupt;
            }
            alog_error("se_task_usleep failed: %s", strerror(-rc));
            goto err_close_stream;
        }
        alog_trace("after 5 sec delay");
        rc = msg_stream_write(__await__, stream, buf, len, 3*1000*1000);
        if(rc < 0){
            if(rc == -EINTR){
                goto interrupt;
            }
            alog_error("msg_stream_write failed: %s", strerror(-rc));
            goto err_close_stream;
        }
        rc = se_task_usleep(__await__, g_daemon.event_loop, 5*1000*1000);
        if(rc < 0){
            if(rc == -EINTR){
                goto interrupt;
            }
            alog_error("se_task_usleep failed: %s", strerror(-rc));
            goto err_close_stream;
        }
        alog_trace("after 5 sec delay");
    }
err_close_stream:
    return;
err_close_fd:
    close(fd);
    return;
interrupt:
    alog_trace("interrupted, because:");
    set_interrupt_disabled(__await__, 1);
    void *reason = get_interrupt_reason(__await__);
    if((uintptr_t) reason == INT_IO_ERR){
        alog_trace("    io closed");
    }else if(reason == &global_interrupt_reasons.SYS_WILL_EXIT){
        alog_trace("    system will exit");
        if(stream){
            msg_stream_write(__await__, stream, "Killed\n", 8, 3*1000*1000);
        }
    }else if((uintptr_t)reason == INT_PROC_END){
        alog_trace("    process ended");
    }
    if(scope_obj){
        rc = sb_bus_call_unit_method(__await__, g_daemon.sd_bus, scope_obj, "Kill", NULL, "si", "all", SIGKILL);
        if(rc < 0){
            alog_error("kill scope failed: %s", strerror(-rc));
        }else{
            alog_trace("kill scope success");
        }
    }
    return;
}

static void client_handler(int fd){
    se_task_create(g_daemon.event_loop, STACK_SIZE, client_handler_async, (void *)(uintptr_t)fd);
}

int main(int argc, char *argv[]) {
    (void)argv;

    if(getenv("SYSTEMD")){
        log_set_systemd(true);
    }

    s_task_init_system();

    log_trace("init_sys");

    int rc = 0;

    rc = install_signals();
    if(rc < 0){
        log_error("install_signals failed: %s", strerror(-rc));
        return -1;
    }

    rc = sd_event_default(&g_daemon.event_loop);

    if(rc < 0){
        log_error("get sd event failed: %s", strerror(-rc));
        return -1;
    }

    rc = sd_event_add_signal(g_daemon.event_loop, NULL, SIGINT, exit_req_handler, NULL);
    if(rc < 0){
        log_error("add signal failed: %s", strerror(-rc));
        return -1;
    }
    rc = sd_event_add_signal(g_daemon.event_loop, NULL, SIGTERM, exit_req_handler, NULL);
    if(rc < 0){
        log_error("add signal failed: %s", strerror(-rc));
        return -1;
    }

    rc = initialize_sd_bus(&g_daemon);
    if(rc < 0){
        log_error("initialize_sd_bus failed: %s", strerror(-rc));
        return -1;
    }

    //s_task_create(g_stack_main, sizeof(g_stack_main), main_task, (void *)(size_t)argc);
    //se_task_create(g_sd_event, STACK_SIZE, main_task, (void *)(size_t)argc);
    rc = setup_unix_listening_socket(&g_daemon, client_handler);
    if(rc < 0){
        log_error("setup_unix_listening_socket failed: %s", strerror(-rc));
        return -1;
    }

    sd_event_source *sleep_timer = NULL;
    rc = sd_event_add_time_relative(g_daemon.event_loop, &sleep_timer, CLOCK_MONOTONIC, NO_JOB_SLEEP_DELAY, 0, sleep_timer_handler, NULL);
    if(rc < 0){
        log_error("add time event failed: %s", strerror(-rc));
        return -1;
    }
    rc = sd_event_source_set_enabled(sleep_timer, SD_EVENT_OFF);
    if(rc < 0){
        log_error("set time event disabled failed: %s", strerror(-rc));
        return -1;
    }

    log_trace("main_create");

    while(1){
        s_task_main_loop_once();
        rc = sd_event_run(g_daemon.event_loop, (uint64_t) -1);
        if(rc == -ESTALE){
            break;
        }else{
            if(rc < 0){
                log_error("sd_event_run failed: %s", strerror(-rc));
                return -1;
            }
        }
        if(g_exit_req == EXIT_REQ_SENT && is_task_empty()){
            rc = sd_event_exit(g_daemon.event_loop, 0);
            if(rc < 0){
                log_error("sd_event_exit failed: %s", strerror(-rc));
                return -1;
            }
            g_exit_req = WAIT_TASKS;
        }else if(is_task_empty()){
            int timer_enabled;
            rc = sd_event_source_get_enabled(sleep_timer, &timer_enabled);
            if(rc < 0){
                log_error("get time event enabled failed: %s", strerror(-rc));
                return -1;
            }
            if (timer_enabled == SD_EVENT_OFF){
                log_trace("No tasks, enable timer");
                rc = sd_event_source_set_time_relative(sleep_timer, NO_JOB_SLEEP_DELAY);
                if(rc < 0){
                    log_error("set time event failed: %s", strerror(-rc));
                    return -1;
                }
                rc = sd_event_source_set_enabled(sleep_timer, SD_EVENT_ONESHOT);
                if(rc < 0){
                    log_error("set time event enabled failed: %s", strerror(-rc));
                    return -1;
                }
            }
        }else{
            int timer_enabled;
            rc = sd_event_source_get_enabled(sleep_timer, &timer_enabled);
            if(rc < 0){
                log_error("get time event enabled failed: %s", strerror(-rc));
                return -1;
            }
            if(timer_enabled != SD_EVENT_OFF){
                log_trace("tasks coming, disable timer");
                rc = sd_event_source_set_enabled(sleep_timer, SD_EVENT_OFF);
                if(rc < 0){
                    log_error("set time event disabled failed: %s", strerror(-rc));
                    return -1;
                }
            }
        }
    }
    log_info("all task is over");
    if(g_daemon.sd_bus){
        sd_bus_unref(g_daemon.sd_bus);
    }
    if(g_daemon.server_unix_sock_event_source){
        sd_event_source_disable_unref(g_daemon.server_unix_sock_event_source);
    }
    if(sleep_timer){
        sd_event_source_disable_unref(sleep_timer);
    }
    sd_event_unrefp(&g_daemon.event_loop);
    return 0;
}
