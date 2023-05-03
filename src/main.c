/* Copyright xhawk, MIT license */

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


static const size_t STACK_SIZE = 256*1024;

static struct daemon g_daemon = {0};

static int exit_req_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata){
    sd_event_source_disable_unref(s);
    int rc = sd_event_exit(g_daemon.event_loop, 0);
    if(rc < 0){
         log_error("sd_event_exit failed: %s", strerror(-rc));
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
    };

    int fd = (int)(size_t)arg;
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
    alog_trace("interrupted");
    int reason = (size_t)get_interrupt_reason(__await__);
    switch (reason) {
    case INT_IO_ERR:
        alog_trace("io closed");
        break;
    }
    return;
}

static void client_handler(int fd){
    se_task_create(g_daemon.event_loop, STACK_SIZE, client_handler_async, (void *)(size_t)fd);
}

int main(int argc, char *argv[]) {
    (void)argv;

    if(getenv("SYSTEMD")){
        log_set_systemd(true);
    }

    g_daemon.server_path = "/run/net_limiter_server.sock";

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

    //s_task_create(g_stack_main, sizeof(g_stack_main), main_task, (void *)(size_t)argc);
    //se_task_create(g_sd_event, STACK_SIZE, main_task, (void *)(size_t)argc);
    rc = setup_unix_listening_socket(&g_daemon, client_handler);
    if(rc < 0){
        log_error("setup_unix_listening_socket failed: %s", strerror(-rc));
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
    }
    log_info("all task is over");
    if(g_daemon.server_unix_sock_event_source){
        sd_event_source_disable_unrefp(&g_daemon.server_unix_sock_event_source);
    }
    sd_event_unrefp(&g_daemon.event_loop);
    return 0;
}
