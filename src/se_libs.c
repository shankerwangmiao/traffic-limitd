#define _GNU_SOURCE
#include <systemd/sd-event.h>
#include <s_task.h>
#include <time.h>
#include <se_libs.h>
#include <errno.h>
#include <stdlib.h>
#include <log.h>
#include <string.h>
#include <unistd.h>
#include <s_list.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/syscall.h>

struct se_timer_arg{
    s_event_t event;
    sd_event_source *source;
    int error;
};

static size_t g_task_seq = 1;

const struct global_interrupt_reasons global_interrupt_reasons;

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

static s_list_t all_tasks = {
    .next = &all_tasks,
    .prev = &all_tasks,
};

int se_task_usleep(__async__, sd_event *event, uint64_t usec) {
    struct se_timer_arg this_arg;
    this_arg.error = 0;
    int rc = 0;

    s_event_init(&this_arg.event);

    rc = sd_event_add_time_relative(
        event, &this_arg.source, CLOCK_MONOTONIC, usec, 0, timer_handler, &this_arg
    );
    alog_trace("timer added after %ld usec", usec);
    if(rc < 0){
        alog_error("sd_event_add_time_relative: %s", strerror(-rc));
        return rc;
    }

    rc = s_event_wait(__await__, &this_arg.event);

    if(rc < 0){
        alog_info("timer interrupted");
        rc = -EINTR;
        goto err_unref_src;
    }

    if(this_arg.error != 0){
        rc = this_arg.error;
    }
    if(rc < 0){
        alog_error("error returned from timer handler: %s", strerror(-rc));
        goto err_unref_src;
    }

err_unref_src:
    sd_event_source_disable_unref(this_arg.source);
    this_arg.source = NULL;
    return rc;
}

struct memory_to_free{
    void *mem;
    void (*free_fn)(void *);
    struct memory_to_free *next;
};

struct se_task_arg{
    s_list_t list_node;
    s_task_fn_t entry;
    sd_event_source *source;
    sd_event *event;
    size_t task_id;
    void *arg;
    struct memory_to_free *memory_to_free;
    int interrupt_disabled;
    void *interrupt_reason;
    uint8_t stack[];
};

// Called on main stack
static int se_task_end_handler(sd_event_source *s, void *userdata){
    struct se_task_arg *arg = (struct se_task_arg *)userdata;
    s_list_detach(&arg->list_node);
    sd_event_source_disable_unref(arg->source);
    sd_event_unref(arg->event);
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
    alog_trace("task %d ended", this_arg->task_id);

    struct memory_to_free *mem = this_arg->memory_to_free;
    while(mem){
        struct memory_to_free *next = mem->next;
        mem->free_fn(mem->mem);
        free(mem);
        mem = next;
    }

    if(rc < 0){
        alog_error("task %d: sd_event_add_defer failed: %s", this_arg->task_id, strerror(-rc));
        abort();
    }
}

static struct se_task_arg* get_current_task_arg(__async__){
    void *current_stack = s_task_get_current_stack(__await__);
    return (struct se_task_arg *)(current_stack - offsetof(struct se_task_arg, stack));
}

void se_task_register_memory_to_free(__async__, void *mem, void (*free_fn)(void *)){
    struct se_task_arg *this_arg = get_current_task_arg(__await__);
    struct memory_to_free *new_mem = (struct memory_to_free *)malloc(sizeof(struct memory_to_free));
    if(!new_mem){
        log_error("malloc failed");
        abort();
    }
    new_mem->mem = mem;
    new_mem->free_fn = free_fn;
    new_mem->next = this_arg->memory_to_free;
    this_arg->memory_to_free = new_mem;
}

void *get_interrupt_reason(__async__){
    struct se_task_arg *this_arg = get_current_task_arg(__await__);
    return this_arg->interrupt_reason;
}

size_t get_current_task_id(__async__){
    struct se_task_arg *this_arg = get_current_task_arg(__await__);
    return this_arg->task_id;
}

void set_interrupt_disabled(__async__, int disabled){
    struct se_task_arg *this_arg = get_current_task_arg(__await__);
    this_arg->interrupt_disabled = !!disabled;
}

static void interrupt_task(struct se_task_arg *arg, void *reason){
    if(arg->interrupt_disabled){
        return;
    }
    arg->interrupt_reason = reason;
    s_task_cancel_wait(&arg->stack);
}

void interrupt_all_tasks(void *reason){
    s_list_t *node = all_tasks.next;
    while(node != &all_tasks){
        struct se_task_arg *arg = GET_PARENT_ADDR(node, struct se_task_arg, list_node);
        node = node->next;
        interrupt_task(arg, reason);
    }
}

bool is_task_empty(void){
    return s_list_is_empty(&all_tasks);
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
    this_arg->memory_to_free = NULL;
    this_arg->interrupt_reason = NULL;
    this_arg->interrupt_disabled = 0;
    s_list_init(&this_arg->list_node);
    s_list_attach(&all_tasks, &this_arg->list_node);
    s_task_create(&this_arg->stack , stack_size, se_task_entry, this_arg);
    log_trace("task %d alloced and created", this_arg->task_id);
    return 0;
}

struct msg_stream {
    s_event_t event;
    enum {NOOP, WRITE, READ, ERR, END} state;
    sd_event_source *source;
    sd_event_source *timer;
    struct ucred cred;
    struct {
        struct se_task_arg *target_task;
        void *reason;
    } interrupt;
    struct {
        void * buf;
        size_t len;
    } io_buf;
    int error;
    sd_event *event_loop;
};

static int msg_stream_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata){
    struct msg_stream *this_stream = (struct msg_stream *)userdata;
    int rc = 0;
    //log_trace("msg_stream_handler called for fd %d, revents: 0x%x", fd, revents);
    if(this_stream->timer){
        sd_event_source_disable_unref(this_stream->timer);
        this_stream->timer = NULL;
    }
    if(revents & EPOLLIN){
        if(this_stream->state == READ){
            if(this_stream->io_buf.len){
                rc = read(fd, this_stream->io_buf.buf, this_stream->io_buf.len);
            }else{
                rc = 0;
            }
            if(rc < 0){
                if(errno == EAGAIN){
                    return 0;
                }else{
                    log_error("read: %s", strerror(errno));
                    this_stream->error = -errno;
                }
            }else{
                this_stream->error = 0;
            }
            this_stream->io_buf.len = rc;
            this_stream->state = NOOP;
            s_event_set(&this_stream->event);
            rc = sd_event_source_set_io_events(s, 0);
            if(rc < 0){
                log_error("sd_event_source_set_io_events: %s", strerror(-rc));
                this_stream->error = rc;
            }
        }
    }else if(revents & EPOLLOUT){
        if(this_stream->state == WRITE){
            if(this_stream->io_buf.len){
                rc = write(fd, this_stream->io_buf.buf, this_stream->io_buf.len);
            }else{
                rc = 0;
            }
            if(rc < 0){
                if(errno == EAGAIN){
                    return 0;
                }else{
                    log_error("write: %s", strerror(errno));
                    this_stream->error = -errno;
                }
            }else{
                this_stream->error = 0;
            }
            this_stream->io_buf.len = rc;
            this_stream->state = NOOP;
            s_event_set(&this_stream->event);
            rc = sd_event_source_set_io_events(s, 0);
            if(rc < 0){
                log_error("sd_event_source_set_io_events: %s", strerror(-rc));
                this_stream->error = rc;
            }
        }
    }else{
        if(revents & EPOLLHUP){
            this_stream->state = END;
            this_stream->error = 0;
        }else{
            this_stream->state = ERR;
            this_stream->error = -EIO;
        }
        rc = sd_event_source_set_enabled(s, SD_EVENT_OFF);
        s_event_set(&this_stream->event);
        if(this_stream->state != WRITE && this_stream->state != READ){
            if(this_stream->interrupt.target_task){
                interrupt_task(this_stream->interrupt.target_task, this_stream->interrupt.reason);
            }
        }
    }
    return 0;
}

int init_msg_stream(struct msg_stream **stream, sd_event *event, int fd){
    struct msg_stream *this_stream = (struct msg_stream *)malloc(sizeof(struct msg_stream));
    int rc = 0;

    if(this_stream == NULL){
        log_error("malloc: %s", strerror(errno));
        rc = -errno;
        goto err_out;
    }
    s_event_init(&this_stream->event);
    this_stream->state = NOOP;
    this_stream->source = NULL;
    this_stream->io_buf.buf = NULL;
    this_stream->io_buf.len = 0;
    this_stream->error = 0;
    this_stream->event_loop = sd_event_ref(event);
    this_stream->timer = NULL;
    this_stream->interrupt.target_task = NULL;
    this_stream->interrupt.reason = NULL;

    {
        socklen_t len = sizeof(this_stream->cred);
        rc = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &this_stream->cred, &len);

        if(rc < 0){
            log_error("getsockopt(SO_PEERCRED): %s", strerror(errno));
            goto err_free_stream;
        }

        if(len != sizeof(this_stream->cred)){
            log_error("getsockopt(SO_PEERCRED): unexpected return length");
            rc = -EIO;
            goto err_free_stream;
        }
    }

    rc = sd_event_add_io(event, &this_stream->source, fd, 0, msg_stream_handler, this_stream);
    if(rc < 0){
        log_error("sd_event_add_io: %s", strerror(-rc));
        goto err_free_stream;
    }
    rc = sd_event_source_set_io_fd_own(this_stream->source, true);
    if(rc < 0){
        log_error("sd_event_source_set_io_fd_own: %s", strerror(-rc));
        goto err_free_source;
    }
    *stream = this_stream;
    return rc;
err_free_source:
    sd_event_source_unref(this_stream->source);
err_free_stream:
    sd_event_unref(this_stream->event_loop);
    free(this_stream);
err_out:
    return rc;
}
void destroy_msg_stream(struct msg_stream *stream){
    if(stream->source != NULL){
        sd_event_source_disable_unref(stream->source);
        stream->source = NULL;
    }
    if(stream->timer != NULL){
        sd_event_source_disable_unref(stream->timer);
        stream->timer = NULL;
    }
    sd_event_unref(stream->event_loop);
    free(stream);
}

int shutdown_msg_stream(__async__, struct msg_stream *stream){
    int rc = 0;
    while(1){
        /* drain read buffer */
        char buf[1];
        rc = recv(sd_event_source_get_io_fd(stream->source), buf, 1, MSG_DONTWAIT);
        if(rc == 0){
            break;
        }else if(rc < 0){
            if(errno == EWOULDBLOCK){
                break;
            }else if(errno == EINTR){
                continue;
            }else{
                log_error("recv: %s", strerror(errno));
                return -errno;
            }
        }
    }
    rc = msg_stream_write(__await__, stream, NULL, 0, 0);
    if(rc < 0){
        log_error("msg_stream_write: %s", strerror(-rc));
        return rc;
    }
    rc = sd_event_source_set_enabled(stream->source, SD_EVENT_OFF);
    if(rc < 0){
        log_error("sd_event_source_set_enabled: %s", strerror(-rc));
        return rc;
    }
    sd_event_source_disable_unref(stream->timer);
    stream->timer = NULL;
    rc = sd_event_source_get_io_fd(stream->source);
    if(rc < 0){
        log_error("sd_event_source_get_io_fd: %s", strerror(-rc));
        return rc;
    }
    int fd = rc;
    rc = shutdown(fd, SHUT_RDWR);
    if(rc < 0){
        log_error("shutdown socket: %s", strerror(errno));
        rc = -errno;
        return rc;
    }
    sd_event_source_disable_unref(stream->source);
    stream->source = NULL;
    if(stream->state != ERR){
        stream->state = END;
        stream->error = 0;
    }
    return 0;
}

const struct ucred *msg_stream_get_peer_cred(const struct msg_stream *stream){
    return &stream->cred;
}

static int io_timer_handler(sd_event_source *s, uint64_t usec, void *userdata){
    struct msg_stream *this_stream = (struct msg_stream *)userdata;
    int rc = 0;
    this_stream->state = NOOP;
    this_stream->error = -ETIMEDOUT;
    rc = sd_event_source_set_io_events(this_stream->source, 0);
    if(rc < 0){
        log_error("sd_event_source_set_io_events: %s", strerror(-rc));
        this_stream->error = rc;
    }
    s_event_set(&this_stream->event);
    sd_event_source_disable_unref(this_stream->timer);
    this_stream->timer = NULL;
    return 0;
}

void msg_stream_reg_interrupt(__async__, struct msg_stream *stream, void *reason){
    if(reason == NULL){
        stream->interrupt.target_task = NULL;
        stream->interrupt.reason = NULL;
    }else{
        stream->interrupt.target_task = get_current_task_arg(__await__);
        stream->interrupt.reason = reason;
    }
}

static ssize_t msg_stream_do_io(__async__, struct msg_stream *stream, void *buf, size_t size, uint64_t usec, int read_or_write){
    int rc = 0;

    if(stream->state == ERR){
        return stream->error;
    }else if(stream->state == END){
        if(read_or_write == READ){
            return 0;
        }else{
            return -EPIPE;
        }
    }

    stream->state = read_or_write;
    stream->io_buf.buf = buf;
    stream->io_buf.len = size;

    if(usec != 0){
        rc = sd_event_add_time_relative(
            stream->event_loop, &stream->timer, CLOCK_MONOTONIC, usec, 0, io_timer_handler, stream
        );
        if(rc < 0){
            alog_error("sd_event_add_time_relative: %s", strerror(-rc));
            return rc;
        }
    }

    rc = sd_event_source_set_io_events(stream->source, read_or_write == READ ? EPOLLIN : EPOLLOUT);
    if(rc < 0){
        alog_error("sd_event_source_set_io_events: %s", strerror(-rc));
        return rc;
    }

    rc = s_event_wait(__await__, &stream->event);

    if(rc < 0){
        alog_info("wait interrupted");
        if(stream->timer){
            sd_event_source_disable_unref(stream->timer);
            stream->timer = NULL;
        }
        rc = sd_event_source_set_io_events(stream->source, 0);
        if(rc < 0){
            alog_error("sd_event_source_set_io_events: %s", strerror(-rc));
        }
        stream->state = NOOP;
        return -EINTR;
    }

    if(stream->error < 0){
        return stream->error;
    }else{
        return stream->io_buf.len;
    }
}

ssize_t msg_stream_read(__async__, struct msg_stream *stream, void *buf, size_t size, uint64_t usec){
    return msg_stream_do_io(__await__, stream, buf, size, usec, READ);
}
ssize_t msg_stream_write(__async__, struct msg_stream *stream, const void *buf, size_t size, uint64_t usec){
    return msg_stream_do_io(__await__, stream, (void *)buf, size, usec, WRITE);
}


#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434   /* System call # on most architectures */
#endif

static int pidfd_open(pid_t pid, unsigned int flags){
    return syscall(__NR_pidfd_open, pid, flags);
}

struct pidfd_event {
    s_event_t event;
    sd_event_source *source;
    int terminated;
    int wait_for_exit;
    struct {
        struct se_task_arg *target_task;
        void *reason;
    } interrupt;
};

static int pidfd_event_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata){
    struct pidfd_event *this_event = (struct pidfd_event *)userdata;
    log_trace("pidfd_event_handler caller for fd %d, revents: 0x%x", fd, revents);

    this_event->terminated = 1;
    if(this_event->wait_for_exit){
        s_event_set(&this_event->event);
        this_event->wait_for_exit = 0;
    }else{
        if(this_event->interrupt.target_task){
            interrupt_task(this_event->interrupt.target_task, this_event->interrupt.reason);
        }
    }
    return 0;
}

int init_pidfd_event(struct pidfd_event **pid_event, sd_event *event, pid_t pid){
    struct pidfd_event *this_event = (struct pidfd_event *)malloc(sizeof(struct pidfd_event));
    int rc = 0;

    if(this_event == NULL){
        log_error("malloc: %s", strerror(errno));
        rc = -errno;
        goto err_out;
    }
    s_event_init(&this_event->event);
    this_event->source = NULL;
    this_event->interrupt.target_task = NULL;
    this_event->interrupt.reason = NULL;
    this_event->terminated = 0;
    this_event->wait_for_exit = 0;

    int pidfd = pidfd_open(pid, 0);
    int pidfd_owned = 0;
    if(pidfd < 0){
        log_error("pidfd_open: %s", strerror(errno));
        rc = -errno;
        goto err_free_event;
    }

    rc = sd_event_add_io(event, &this_event->source, pidfd, EPOLLIN, pidfd_event_handler, this_event);
    if(rc < 0){
        log_error("sd_event_add_io: %s", strerror(-rc));
        goto err_close_pidfd;
    }
    rc = sd_event_source_set_io_fd_own(this_event->source, true);
    if(rc < 0){
        log_error("sd_event_source_set_io_fd_own: %s", strerror(-rc));
        goto err_free_source;
    }
    pidfd_owned = 1;
    rc = sd_event_source_set_enabled(this_event->source, SD_EVENT_ONESHOT);
    if(rc < 0){
        log_error("sd_event_source_set_enabled: %s", strerror(-rc));
        goto err_free_source;
    }
    *pid_event = this_event;
    return rc;
err_free_source:
    sd_event_source_unref(this_event->source);
err_close_pidfd:
    if(!pidfd_owned){
        close(pidfd);
    }
err_free_event:
    free(this_event);
err_out:
    return rc;
}
void destroy_pidfd_event(struct pidfd_event *pid_event){
    if(pid_event->source != NULL){
        sd_event_source_disable_unref(pid_event->source);
        pid_event->source = NULL;
    }
    free(pid_event);
}
void pidfd_event_reg_interrupt(__async__, struct pidfd_event *event, void *reason){
    if(reason == NULL){
        event->interrupt.target_task = NULL;
        event->interrupt.reason = NULL;
    }else{
        event->interrupt.target_task = get_current_task_arg(__await__);
        event->interrupt.reason = reason;
    }
}
int pidfd_event_wait_for_exit(__async__, struct pidfd_event *event){
    int rc = 0;
    if(event->terminated){
        return 0;
    }
    event->wait_for_exit = 1;
    if(rc < 0){
        log_error("sd_event_source_set_enabled: %s", strerror(-rc));
        return rc;
    }
    rc = s_event_wait(__await__, &event->event);
    if(rc < 0){
        alog_info("wait interrupted");
        rc = sd_event_source_set_enabled(event->source, SD_EVENT_OFF);
        if(rc < 0){
            log_error("sd_event_source_set_enabled: %s", strerror(-rc));
        }
        return -EINTR;
    }

    rc = 0;
    return rc;
}
