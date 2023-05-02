#include <systemd/sd-event.h>
#include <s_task.h>
#include <time.h>
#include <se_libs.h>

struct se_timer_arg{
    s_event_t event;
    sd_event_source *source;
    int error;
};

static int timer_handler(sd_event_source *s, uint64_t usec, void *userdata){
    struct se_timer_arg *arg = (struct se_timer_arg *)userdata;
    int rc = 0;
    rc = sd_event_source_set_enabled(s, SD_EVENT_OFF);
    if(rc < 0){
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

    if(rc < 0){
        return rc;
    }

    s_event_wait(__await__, &this_arg.event);

    if(this_arg.error != 0){
        rc = this_arg.error;
    }

    if(rc < 0){
        goto err_unref_src;
    }

err_unref_src:
    sd_event_source_unref(this_arg.source);
    this_arg.source = NULL;
    return rc;
}