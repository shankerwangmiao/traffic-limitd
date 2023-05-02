/* Copyright xhawk, MIT license */

#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "s_task.h"
#include "s_list.h"

#define S_TASK_STACK_MAGIC ((int)0x5AA55AA5)
THREAD_LOCAL s_task_globals_t g_globals;

#if defined __unix__ || defined __linux__ || defined __APPLE__
#   include "s_port_posix.inc.h"
#else
#   error "no arch detected"
#endif


/*******************************************************************/
/* tasks                                                           */
/*******************************************************************/

/* 
    *timeout, wait timeout
    return, true on task run
 */
static void s_task_call_next(__async__) {
    /* get next task and run it */
    s_list_t* next;
    s_task_t* old_task;

    (void)__awaiter_dummy__;

    /* Check active tasks */
    if (s_list_is_empty(&g_globals.active_tasks)) {
#ifndef NDEBUG
        fprintf(stderr, "error: must has one task to run\n");
#endif
        return;
    }

    old_task = g_globals.current_task;
    next = s_list_get_next(&g_globals.active_tasks);
    
    /* printf("next = %p %p\n", g_globals.current_task, next); */

    g_globals.current_task = GET_PARENT_ADDR(next, s_task_t, node);
    s_list_detach(next);

    if (old_task != g_globals.current_task) {
#if defined   USE_SWAP_CONTEXT
        swapcontext(&old_task->uc, &g_globals.current_task->uc);
#elif defined USE_JUMP_FCONTEXT
        s_jump_t jump;
        jump.from = &old_task->fc;
        jump.to = &g_globals.current_task->fc;
        transfer_t t = jump_fcontext(g_globals.current_task->fc, (void*)&jump);
        s_jump_t* ret = (s_jump_t*)t.data;
        *ret->from = t.fctx;
#endif
    }

#ifdef USE_STACK_DEBUG
    if(g_globals.current_task->stack_size > 0) {
        s_task_t* task = g_globals.current_task;
        void *real_stack = (void *)&task[1];
        size_t real_stack_size = task->stack_size - sizeof(task[0]);
        size_t int_stack_size = real_stack_size / sizeof(int);
        if(((int *)real_stack)[0] != S_TASK_STACK_MAGIC) {
#ifndef NDEBUG
            fprintf(stderr, "stack overflow in lower bits");
#endif
            while(1);   /* dead */
        }
        if(((int *)real_stack)[int_stack_size - 1] != S_TASK_STACK_MAGIC) {
#ifndef NDEBUG
            fprintf(stderr, "stack overflow in higher bits");
#endif
            while(1);   /* dead */
        }
    }
#endif    
}

void s_task_next(__async__) {
    g_globals.current_task->waiting_cancelled = false;
    s_task_call_next(__await__);
}  

void s_task_main_loop_once() {
    __async__ = 0;
    while (!s_list_is_empty(&g_globals.active_tasks)) {
        /* Put current task to the waiting list */
        s_list_attach(&g_globals.active_tasks, &g_globals.current_task->node);
        s_task_call_next(__await__);
    }
}

void s_task_yield(__async__) {
    /* Put current task to the waiting list */
    s_list_attach(&g_globals.active_tasks, &g_globals.current_task->node);
    s_task_next(__await__);
    g_globals.current_task->waiting_cancelled = false;
}

void s_task_init_system_() {
    s_list_init(&g_globals.active_tasks);
#ifdef USE_DEAD_TASK_CHECKING
    s_list_init(&g_globals.waiting_events);
#endif

    s_list_init(&g_globals.main_task.node);
    s_event_init(&g_globals.main_task.join_event);
    g_globals.main_task.stack_size = 0;
    g_globals.main_task.closed = false;
    g_globals.main_task.waiting_cancelled = false;
    g_globals.current_task = &g_globals.main_task;
}

void s_task_create(void *stack, size_t stack_size, s_task_fn_t task_entry, void *task_arg) {
    void *real_stack;
    size_t real_stack_size;

    s_task_t *task = (s_task_t *)stack;
    s_list_init(&task->node);
    s_event_init(&task->join_event);
    task->task_entry = task_entry;
    task->task_arg   = task_arg;
    task->stack_size = stack_size;
    task->closed = false;
    task->waiting_cancelled = false;
    s_list_attach(&g_globals.active_tasks, &task->node);

    real_stack = (void *)&task[1];
    real_stack_size = stack_size - sizeof(task[0]);
#ifdef USE_STACK_DEBUG
    {
        /* Fill magic number so as to check stack size */
        size_t int_stack_size = real_stack_size / sizeof(int);
        if(int_stack_size <= 2) {
#ifndef NDEBUG
            fprintf(stderr, "stack size too small");
            return;
#endif
        }
        ((int *)real_stack)[0] = S_TASK_STACK_MAGIC;
        ((int *)real_stack)[int_stack_size - 1] = S_TASK_STACK_MAGIC;
        real_stack = (char *)real_stack + sizeof(int);
        real_stack_size = (int_stack_size - 2) * sizeof(int);
    }
#endif

#if defined   USE_SWAP_CONTEXT
    create_context(&task->uc, real_stack, real_stack_size);
#elif defined USE_JUMP_FCONTEXT
    create_fcontext(&task->fc, real_stack, real_stack_size, s_task_fcontext_entry);
#endif
}

int s_task_join(__async__, void *stack) {
    s_task_t *task = (s_task_t *)stack;
    while (!task->closed) {
        int ret = s_event_wait(__await__, &task->join_event);
        if (ret != 0)
            return ret;
    }
    return 0;
}

/* timer conflict with this function!!!
   Do NOT call s_task_kill, and let the task exit by itself! */
void s_task_kill__remove(void *stack) {
    s_task_t *task = (s_task_t *)stack;
    s_list_detach(&task->node);
}

void s_task_cancel_wait(void* stack) {
    s_task_t* task = (s_task_t*)stack;

    task->waiting_cancelled = true;
    s_list_detach(&task->node);
    s_list_attach(&g_globals.active_tasks, &task->node);
}

unsigned int s_task_cancel_dead() {
#ifdef USE_DEAD_TASK_CHECKING
    return s_event_cancel_dead_waiting_tasks_();
#else
    return 0;
#endif
}

static size_t s_task_get_stack_free_size_ex_by_stack(void *stack) {
    uint32_t *check;
    for(check = (uint32_t *)stack; ; ++check){
        if(*check != 0xFFFFFFFF)
            return (char *)check - (char *)stack;
    }
}

static size_t s_task_get_stack_free_size_by_task(s_task_t *task) {
    return s_task_get_stack_free_size_ex_by_stack(&task[1]);
}

size_t s_task_get_stack_free_size() {
    return s_task_get_stack_free_size_by_task(g_globals.current_task);
}

void s_task_context_entry() {
    struct tag_s_task_t *task = g_globals.current_task;
    s_task_fn_t task_entry = task->task_entry;
    void *task_arg         = task->task_arg;

    __async__ = 0;
    (*task_entry)(__await__, task_arg);

    task->closed = true;
    s_event_set(&task->join_event);
    s_task_next(__await__);
}


#ifdef USE_JUMP_FCONTEXT
void s_task_fcontext_entry(transfer_t arg) {
    /* printf("=== s_task_helper_entry = %p\n", arg.fctx); */

    s_jump_t* jump = (s_jump_t*)arg.data;
    *jump->from = arg.fctx;
    /* printf("%p %p %p\n", jump, jump->to, g_globals.current_task); */

    s_task_context_entry();
}
#endif


