#ifndef INC_S_TASK_H_
#define INC_S_TASK_H_

/* Copyright xhawk, MIT license */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "s_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct{
    int dummy;
} s_awaiter_t;

#   define __await__      __awaiter_dummy__
#   define __async__      s_awaiter_t *__awaiter_dummy__

/* Function type for task entrance */
typedef void(*s_task_fn_t)(__async__, void *arg);


/* #define USE_SWAP_CONTEXT                                            */
/* #define USE_JUMP_FCONTEXT                                           */
/* #define USE_LIST_TIMER_CONTAINER //for very small memory footprint  */
/* #define USE_IN_EMBEDDED                                             */
/* #define USE_STACK_DEBUG                                             */
/* #define USE_DEAD_TASK_CHECKING                                      */


#if defined __unix__ || defined __linux__ || defined __APPLE__
#   define USE_JUMP_FCONTEXT
#   define USE_DEAD_TASK_CHECKING
#else
#   error "no arch detected"
#endif

typedef struct {
    s_list_t wait_list;
#ifdef USE_DEAD_TASK_CHECKING
    s_list_t self;
#endif
} s_event_t;



/* Initialize the task system. */

void s_task_init_system_(void);
#define s_task_init_system() __async__ = 0; (void)__awaiter_dummy__; s_task_init_system_()

/* Create a new task */
void s_task_create(void *stack, size_t stack_size, s_task_fn_t entry, void *arg);

/* Wait a task to exit */
int s_task_join(__async__, void *stack);

/* Kill a task */
void s_task_kill(void *stack);

/* Yield current task */
void s_task_yield(__async__);

/* Cancel task waiting and make it running */
void s_task_cancel_wait(void* stack);

/* Get free stack size (for debug) */
size_t s_task_get_stack_free_size(void);

/* Dump task information */
/* void dump_tasks(__async__); */

/* Initialize a wait event */
void s_event_init(s_event_t *event);

/* Set event */
void s_event_set(s_event_t *event);

/* Wait event
 *  return 0 on event set
 *  return -1 on event waiting cancelled
 */
int s_event_wait(__async__, s_event_t *event);

void s_task_main_loop_once(void);

void *s_task_get_current_stack(__async__);

#ifdef __cplusplus
}
#endif

#endif
