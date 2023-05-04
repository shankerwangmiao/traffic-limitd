#ifndef INC_S_TASK_INTERNAL_H_
#define INC_S_TASK_INTERNAL_H_

/* Copyright xhawk, MIT license */

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************/
/* s_task type definitions                                         */
/*******************************************************************/

#ifdef USE_JUMP_FCONTEXT
typedef void* fcontext_t;
typedef struct {
    fcontext_t  fctx;
    void* data;
} transfer_t;
#endif

typedef struct tag_s_task_t {
    s_list_t     node;
    s_event_t    join_event;
    s_task_fn_t  task_entry;
    void        *task_arg;
#ifdef USE_DEAD_TASK_CHECKING
    s_event_t   *waiting_event;
#endif
#if defined   USE_SWAP_CONTEXT
    ucontext_t   uc;
#   ifdef __APPLE__
    char dummy[512]; /* it seems darwin ucontext has no enough memory ? */
#   endif
#elif defined USE_JUMP_FCONTEXT
    fcontext_t   fc;
#endif
    size_t       stack_size;
    bool         waiting_cancelled;
    bool         closed;
} s_task_t;

#if defined USE_JUMP_FCONTEXT
typedef struct {
    fcontext_t *from;
    fcontext_t *to;
} s_jump_t;
#endif

typedef struct {
    s_task_t    main_task;
    s_list_t    active_tasks;
    s_task_t   *current_task;

#ifdef USE_DEAD_TASK_CHECKING
    s_list_t    waiting_events;
#endif

} s_task_globals_t;

#if defined __clang__
#   if __clang_major__ >= 2
#       define THREAD_LOCAL __thread
#   else
#       define THREAD_LOCAL
#   endif
#elif defined __GNUC__
#   define GNUC_VERSION_ (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#   if GNUC_VERSION_ >= 30301
#       define THREAD_LOCAL __thread
#   else
#       define THREAD_LOCAL
#   endif
#elif defined __STDC_VERSION__ && __STDC_VERSION__ >= 201112L
#   define THREAD_LOCAL _Thread_local
#else
#   define THREAD_LOCAL
#endif
extern THREAD_LOCAL s_task_globals_t g_globals;


struct tag_s_task_t;
/* */
void s_task_context_entry(void);
#ifdef USE_JUMP_FCONTEXT
void s_task_fcontext_entry(transfer_t arg);
#endif

/* Run next task, but not set myself for ready to run */
void s_task_next(__async__);

/* Return: number of cancelled tasks */
unsigned int s_task_cancel_dead(void);
void s_event_remove_from_waiting_list(s_event_t *event);
#ifdef USE_DEAD_TASK_CHECKING
unsigned int s_event_cancel_dead_waiting_tasks_(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
