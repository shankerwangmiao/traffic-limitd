#include "s_task.h"

/*******************************************************************/
/* event                                                           */
/*******************************************************************/

/* Initialize a wait event */
void s_event_init(s_event_t *event) {
    s_list_init(&event->wait_list);
#ifdef USE_DEAD_TASK_CHECKING
    s_list_init(&event->self);
#endif
}

/* Add the event to global waiting event list */
static void s_event_add_to_waiting_list(s_event_t *event) {
#ifdef USE_DEAD_TASK_CHECKING
    if(s_list_is_empty(&event->wait_list)) {
        s_list_detach(&event->self);
        s_list_attach(&g_globals.waiting_events, &event->self);
    }
#endif
}

/* Remove the event from global waiting event list */
static void s_event_remove_from_waiting_list(s_event_t *event) {
#ifdef USE_DEAD_TASK_CHECKING
    if(s_list_is_empty(&event->wait_list)) {
        s_list_detach(&event->self);
    }
#endif
}

#ifdef USE_DEAD_TASK_CHECKING
/* Cancel dead waiting tasks */
unsigned int s_event_cancel_dead_waiting_tasks_() {
    s_list_t *next_event;
    s_list_t *this_event;
    unsigned int ret = 0;
    /* Check all events */
    for(this_event = s_list_get_next(&g_globals.waiting_events);
        this_event != &g_globals.waiting_events;
        this_event = next_event) {
        s_list_t *next_task;
        s_list_t *this_task;
        s_event_t *event;
        
        next_event = s_list_get_next(this_event);
        s_list_detach(this_event);
        event = GET_PARENT_ADDR(this_event, s_event_t, self);
        
        /* Check all tasks blocked on this event */
        for(this_task = s_list_get_next(&event->wait_list);
            this_task != &event->wait_list;
            this_task = next_task) {
            next_task = s_list_get_next(this_task);
                    
            s_task_t *task = GET_PARENT_ADDR(this_task, s_task_t, node);
            s_task_cancel_wait(task);
            ++ret;
        }
    }

#ifndef NDEBUG
    if (ret > 0) {
        fprintf(stderr, "error: cancel dead tasks waiting on event!\n");
    }
#endif

    return ret;
}
#endif

/* Wait event
 *  return 0 on event set
 *  return -1 on event waiting cancelled
 */
int s_event_wait(__async__, s_event_t *event) {
    int ret;
    /* Put current task to the event's waiting list */
    s_event_add_to_waiting_list(event);
    s_list_detach(&g_globals.current_task->node);   /* no need, for safe */
    s_list_attach(&event->wait_list, &g_globals.current_task->node);
    s_task_next(__await__);
    ret = (g_globals.current_task->waiting_cancelled ? -1 : 0);
    g_globals.current_task->waiting_cancelled = false;
    return ret;
}

/* Set event */
void s_event_set(s_event_t *event) {
    s_list_attach(&g_globals.active_tasks, &event->wait_list);
    s_list_detach(&event->wait_list);
    s_event_remove_from_waiting_list(event);
}
