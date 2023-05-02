#include <systemd/sd-event.h>
#include <s_task.h>
#include <time.h>
#include <se_libs.h>
#include <errno.h>
#include <stdlib.h>
#include <log.h>
#include <string.h>

struct se_timer_arg{
    s_event_t event;
    sd_event_source *source;
    int error;
};

static size_t g_task_seq = 0;

static int timer_handler(sd_event_source *s, uint64_t usec, void *userdata){
    struct se_timer_arg *arg = (struct se_timer_arg *)userdata;
    int rc = 0;
    rc = sd_event_source_set_enabled(s, SD_EVENT_OFF);
    if(rc < 0){
        log_error("sd_event_source_set_enabled: %s", strerror(-rc));
        arg->error = rc;
    }
    s_event_set(&arg->event);
    return 0;
}

int se_task_usleep(__async__, sd_event *event, uint64_t usec) {
    struct se_timer_arg this_arg;
    this_arg.error = 0;
    int rc = 0;

    s_event_init(&this_arg.event);

    rc = sd_event_add_time_relative(
        event, &this_arg.source, CLOCK_MONOTONIC, usec, 0, timer_handler, &this_arg
    );
    log_trace("timer added after %ld usec", usec);
    if(rc < 0){
        log_error("sd_event_add_time_relative: %s", strerror(-rc));
        return rc;
    }

    s_event_wait(__await__, &this_arg.event);

    if(this_arg.error != 0){
        rc = this_arg.error;
    }
    if(rc < 0){
        log_error("error returned from timer handler: %s", strerror(-rc));
        goto err_unref_src;
    }

err_unref_src:
    sd_event_source_unref(this_arg.source);
    this_arg.source = NULL;
    return rc;
}

struct se_task_arg{
    s_task_fn_t entry;
    sd_event_source *source;
    sd_event *event;
    size_t task_id;
    void *arg;
};

// Called on main stack
static int se_task_end_handler(sd_event_source *s, void *userdata){
    struct se_task_arg *arg = (struct se_task_arg *)userdata;
    sd_event_source_disable_unrefp(&arg->source);
    sd_event_unrefp(&arg->event);
    int this_task_id = arg->task_id;
    free(arg);
    log_trace("task %d freeed", this_task_id);
    return 0;
}

// Called on coroutine stack
static void se_task_entry(__async__, void *arg){
    struct se_task_arg *this_arg = (struct se_task_arg *)arg;
    this_arg->entry(__await__, this_arg->arg);
    int rc = sd_event_add_defer(this_arg->event, &this_arg->source, se_task_end_handler, this_arg);
    log_trace("task %d ended", this_arg->task_id);
    if(rc < 0){
        log_error("task %d: sd_event_add_defer failed: %s", this_arg->task_id, strerror(-rc));
        abort();
    }
}

int se_task_create(sd_event *event, size_t stack_size, s_task_fn_t entry, void *arg){
    struct se_task_arg *this_arg = (struct se_task_arg *)malloc(sizeof(struct se_task_arg) + stack_size);
    if(this_arg == NULL){
        return -ENOMEM;
    }
    this_arg->entry = entry;
    this_arg->arg = arg;
    this_arg->event = sd_event_ref(event);
    this_arg->source = NULL;
    this_arg->task_id = g_task_seq++;
    s_task_create(this_arg + 1 , stack_size, se_task_entry, this_arg);
    log_trace("task %d alloced and created", this_arg->task_id);
    return 0;
}
