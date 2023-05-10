#define _GNU_SOURCE
#include <stdio.h>
#include <systemd/sd-event.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <protocol.h>
#include <log.h>
#include <cgroup_util.h>
#include <s_task.h>
#include <se_libs.h>
#include <tcbpf_util.h>
#include "daemon.h"


static const size_t STACK_SIZE = 256*1024;
static const int NO_JOB_SLEEP_DELAY = 20 * 1000 * 1000;

static const int MAX_IO_USEC = 300 * 1000;
static const int MAX_NR_TASKS = 1000;
/*
static const int MAX_IO_USEC = 10 * 300 * 1000;
static const int MAX_NR_TASKS = 2;
*/

static enum{
    NOEXIT = 0,
    EXIT_REQ_SENT = 1,
    WAIT_TASKS = 2,
} g_exit_req = NOEXIT;

static char *g_this_unit_name = NULL;
static struct daemon g_daemon = {0};
static int g_nr_tasks = 0;

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

static int write_rate_limit_msg(__async__, struct msg_stream *stream, int kind, int code){
    int length = sizeof(struct rate_limit_msg);
    switch(kind){
        case RATE_LIMIT_FAIL:
            length += sizeof(struct rate_limit_fail_attr);
            break;
        case RATE_LIMIT_PROCEED:
            length += 0;
            break;
        default:
            return -EINVAL;
    }
    char buf[length];
    memset(buf, 0, length);
    struct rate_limit_msg *msg = (struct rate_limit_msg *)buf;
    msg->length = length;
    msg->type = kind;
    if(kind == RATE_LIMIT_FAIL){
        struct rate_limit_fail_attr *attr = (struct rate_limit_fail_attr *)msg->attr;
        attr->reason = code;
    }
    int rc = msg_stream_write(__await__, stream, buf, length, MAX_IO_USEC);
    return rc;
}

static int write_rate_limit_log(__async__, struct msg_stream *stream, const char *fmt, ...){
    char *msg = NULL;
    int str_length = 0;
    va_list ap;
    va_start(ap, fmt);
    str_length = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if(str_length < 0){
        return -errno;
    }
    int length = str_length + sizeof(struct rate_limit_msg);
    char buf[length];
    memset(buf, 0, sizeof(struct rate_limit_msg));
    struct rate_limit_msg *msg_header = (struct rate_limit_msg *)buf;
    msg_header->length = length;
    msg_header->type = RATE_LIMIT_LOG;
    strncpy(msg_header->attr, msg, str_length);
    free(msg);
    int rc = msg_stream_write(__await__, stream, buf, length, MAX_IO_USEC);
    return rc;
}

static void decrease_nr_tasks(void *dummy){
    (void) dummy;
    if(g_nr_tasks > 0){
        g_nr_tasks--;
    }else{
        log_error("try to decrease g_nr_tasks below 0");
        abort();
    }
}

static void clear_rate_limit(void *data){
    uint64_t cgroup_id = (uint64_t)(uintptr_t)data;
    int rc = 0;
    rc = cgroup_rate_limit_unset(cgroup_id);
    if(rc < 0){
        log_error("cgroup_rate_limit_unset(%d) failed: %s (ignored)", cgroup_id, strerror(-rc));
    }else{
        log_trace("cgroup_rate_limit_unset(%d) succeed", cgroup_id);
    }
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
    int client_error = 0;
    rc = init_msg_stream(&stream, g_daemon.event_loop, fd);
    if(rc < 0){
        alog_error("init_msg_stream failed: %s", strerror(-rc));
        goto err_close_fd;
    }
    se_task_register_memory_to_free(__await__, stream, (void (*)(void *))destroy_msg_stream);
    msg_stream_reg_interrupt(__await__, stream, (void *)INT_IO_ERR);

    if(g_nr_tasks >= MAX_NR_TASKS){
        alog_warn("Too many tasks, reject new connection");
        write_rate_limit_msg(__await__, stream, RATE_LIMIT_FAIL, RATE_LIMIT_FAIL_NORESOURCE);
        shutdown_msg_stream(__await__, stream);
        return;
    }
    g_nr_tasks++;
    se_task_register_memory_to_free(__await__, NULL, decrease_nr_tasks);

    const struct ucred *cred = msg_stream_get_peer_cred(stream);
    alog_info("Our peer pid=%d, uid=%d", cred->pid, cred->uid);

    char *scope_obj = NULL;
    char *scope_name = NULL;
    if(g_this_unit_name == NULL){
        char *this_unit_name = NULL;
        rc = get_self_unit_name(__await__, g_daemon.sd_bus, &this_unit_name);
        if(rc < 0){
            if(rc == -EINTR){
                goto interrupt;
            }
            alog_error("get_self_unit_name failed: %s", strerror(-rc));
            goto err_close_stream;
        }
        /*
            We delay the assign to g_this_unit_name after the async call
            to ensure that two co-routines won't assign to g_this_unit_name
            at the same time.
        */
        if(g_this_unit_name == NULL){
            g_this_unit_name = this_unit_name;
        }else{
            free(this_unit_name);
        }
        alog_trace("got our unit name %s", g_this_unit_name);
    }

    rc = start_transient_scope(__await__, g_daemon.sd_bus, cred->pid, &scope_name, &scope_obj,
        "(sv)(sv)(sv)",
        "After", "as", 1, g_this_unit_name,
        "BindsTo", "as", 1, g_this_unit_name,
        "SendSIGHUP", "b", 1
    );
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

    uint64_t cgroup_id;
    rc = cg_path_get_cgroupid(cgroup_path, &cgroup_id);
    if(rc < 0){
        alog_error("cg_path_get_cgroupid failed: %s", strerror(-rc));
        goto err_close_stream;
    }
    alog_trace("cgroup_id=%llu", cgroup_id);

    rc = cgroup_rate_limit_set(cgroup_id, &(struct rate_limit){.byte_rate = 0, .packet_rate = 0});
    if(rc < 0){
        alog_error("cgroup_rate_limit_set failed: %s", strerror(-rc));
        goto err_close_stream;
    }

    se_task_register_memory_to_free(__await__, (void *)(uintptr_t) cgroup_id, clear_rate_limit);

    #define excepted_msg_len (sizeof(struct rate_limit_msg) + sizeof(struct rate_limit_req_attr))
    char _buf[excepted_msg_len];
    rc = msg_stream_read(__await__, stream, _buf, excepted_msg_len, MAX_IO_USEC);
    if(rc < 0){
        if(rc == -EINTR){
            goto interrupt;
        }
        if(rc == -ETIMEDOUT){
            client_error = 1;
        }
        alog_error("msg_stream_read failed: %s", strerror(-rc));
        goto err_close_stream;
    }else if(rc == 0){
        client_error = 1;
        alog_trace("read eof");
        rc = -ECONNRESET;
        goto err_close_stream;
    }else if((unsigned int)rc < excepted_msg_len){
        client_error = 1;
        alog_error("invalid message size: %d", rc);
        rc = -EINVAL;
        goto err_close_stream;
    }
    struct rate_limit_msg *msg = (struct rate_limit_msg *)_buf;
    struct rate_limit_req_attr *attr = (struct rate_limit_req_attr *)msg->attr;
    if(msg->length < (unsigned int)excepted_msg_len){
        client_error = 1;
        alog_error("invalid message size in header: %d", msg->length);
        rc = -EINVAL;
        goto err_close_stream;
    }
    #undef excepted_msg_len
    //disable interrupt from stream
    msg_stream_reg_interrupt(__await__, stream, 0);

    rc = cgroup_rate_limit_set(cgroup_id, &attr->limit);
    if(rc < 0){
        alog_error("cgroup_rate_limit_set failed: %s", strerror(-rc));
        goto err_close_stream;
    }

    alog_info("will start task with ratelimit bps=%ld, pps=%ld", attr->limit.byte_rate, attr->limit.packet_rate);
    write_rate_limit_log(__await__, stream, "Start task with ratelimit bps=%ld, pps=%ld", attr->limit.byte_rate, attr->limit.packet_rate);
    write_rate_limit_msg(__await__, stream, RATE_LIMIT_PROCEED, 0);
    shutdown_msg_stream(__await__, stream);
    stream = NULL;
    rc = pidfd_event_wait_for_exit(__await__, pidfd_event);
    if(rc < 0){
        if(rc == -EINTR){
            goto interrupt;
        }
        alog_error("pidfd_event_wait_for_exit failed: %s", strerror(-rc));
    }else{
        alog_info("task exited");
    }
    alog_trace("will kill scope: %s", scope_name);
    rc = sb_bus_call_unit_method(__await__, g_daemon.sd_bus, scope_obj, "Kill", NULL, "si", "all", SIGKILL);
    return;
err_close_stream:
    write_rate_limit_log(__await__, stream, "%s Error: %s", client_error ? "Client" : "Internal", strerror(-rc));
    write_rate_limit_msg(__await__, stream, RATE_LIMIT_FAIL, client_error ? RATE_LIMIT_FAIL_YOUR_ERROR : RATE_LIMIT_FAIL_INTERNAL);
    shutdown_msg_stream(__await__, stream);
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
            write_rate_limit_msg(__await__, stream, RATE_LIMIT_FAIL, RATE_LIMIT_FAIL_INTERNAL);
            shutdown_msg_stream(__await__, stream);
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

    const char *ifnames = getenv("IFACES");
    if(!ifnames){
        log_error("environment variable IFACES should be set");
        return -1;
    }

    s_task_init_system();

    log_trace("init_sys");

    int rc = 0;

    rc = install_signals();
    if(rc < 0){
        log_error("install_signals failed: %s", strerror(-rc));
        return -1;
    }

    rc = cg_find_unified();
    if(rc < 0){
        log_error("cg_find_unified failed: %s", strerror(-rc));
        return -1;
    }

    rc = open_and_load_bpf_obj(MAX_NR_TASKS);
    if(rc < 0){
        log_error("open_and_load_bpf_obj failed: %s", strerror(-rc));
        return -1;
    }

    rc = tc_setup_inferface(ifnames);
    if(rc < 0){
        log_error("tc_setup_inferface failed: %s", strerror(-rc));
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
    if(g_this_unit_name){
        free(g_this_unit_name);
    }
    close_bpf_obj();
    return 0;
}
