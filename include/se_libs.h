#include <systemd/sd-event.h>
#include <s_task.h>

int se_task_usleep(__async__, sd_event *event, uint64_t usec);
int se_task_create(sd_event *event, size_t stack_size, s_task_fn_t entry, void *arg);
