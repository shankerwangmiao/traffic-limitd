#ifndef INC_SE_LIBS_H_
#define INC_SE_LIBS_H_

#include <systemd/sd-event.h>
#include <s_task.h>

struct msg_stream;

int se_task_usleep(__async__, sd_event *event, uint64_t usec);
int se_task_create(sd_event *event, size_t stack_size, s_task_fn_t entry, void *arg);
void se_task_register_memory_to_free(__async__, void *mem, void (*free_fn)(void *));

void interrupt_all_tasks(void *reason);
bool is_task_empty(void);
void *get_interrupt_reason(__async__);
size_t get_current_task_id(__async__);

int init_msg_stream(struct msg_stream **stream, sd_event *event, int fd);
void destroy_msg_stream(struct msg_stream *stream);
void msg_stream_reg_interrupt(__async__, struct msg_stream *stream, void *reason);
ssize_t msg_stream_read(__async__, struct msg_stream *stream, void *buf, size_t size, uint64_t usec);
ssize_t msg_stream_write(__async__, struct msg_stream *stream, const void *buf, size_t size, uint64_t usec);

enum {
    MSG_STREAM_ERROR = 1,
    MSG_STREAM_TIMEOUT = 2
};

extern const struct global_interrupt_reasons{
    uint8_t SYS_WILL_EXIT;
} global_interrupt_reasons;

#endif /* defined(INC_SE_LIBS_H_) */
