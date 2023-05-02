/* Copyright xhawk, MIT license */

#include <stdio.h>
#include <systemd/sd-event.h>
#include <s_task.h>
#include <se_libs.h>
#include <errno.h>
#include <string.h>


s_event_t  g_event;
bool       g_closed = false;
void      *g_stack_main[256*1024];
void      *g_stack0[256*1024];
void      *g_stack1[256*1024];

sd_event *g_sd_event;


void sub_task(__async__, void *arg) {
    size_t n = (size_t)arg;

    printf("begin_sub_%ld\n", n);

    while (!g_closed) {
        s_event_wait(__await__, &g_event);
        printf("task %d wait event OK\n", (int)n);
    }
}


void main_task(__async__, void *arg) {
    int i;
    s_event_init(&g_event);

    printf("begin_main\n");

    s_task_create(g_stack0, sizeof(g_stack0), sub_task, (void *)1);
    s_task_create(g_stack1, sizeof(g_stack1), sub_task, (void *)2);

    for(i = 0; i < 10; ++i) {
        printf("task_main arg = %p, i = %d\n", arg, i);

        s_task_yield(__await__);
        /* s_task_msleep(__await__, 500); */
        se_task_usleep(__await__, g_sd_event, 500*1000);

        if (i % 3 == 0) {
            printf("task main set event\n");
            s_event_set(&g_event);
        }
    }

    g_closed = true;
    int rc = sd_event_exit(g_sd_event, 0);
    if(rc < 0){
        printf("sd_event_exit failed: %s\n", strerror(-rc));
    }
}

int main(int argc, char *argv[]) {
    (void)argv;

    s_task_init_system();

    printf("init_sys\n");

    int rc = 0;

    rc = sd_event_default(&g_sd_event);

    if(rc < 0){
        printf("get sd event failed: %s\n", strerror(-rc));
        return -1;
    }

    s_task_create(g_stack_main, sizeof(g_stack_main), main_task, (void *)(size_t)argc);

    printf("main_create\n");

    while(1){
        s_task_main_loop_once();
        rc = sd_event_run(g_sd_event, (uint64_t) -1);
        if(rc == -ESTALE){
            break;
        }else{
            if(rc < 0){
                printf("sd_event_run failed: %s\n", strerror(-rc));
                return -1;
            }
        }
    }
    printf("all task is over\n");
    sd_event_unrefp(&g_sd_event);
    return 0;
}
