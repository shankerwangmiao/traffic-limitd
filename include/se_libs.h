#ifndef INC_SE_LIBS_H_
#define INC_SE_LIBS_H_

#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>
#include <s_task.h>
#include <sys/socket.h>

/* tasks */
/* se is short for S_event systemd-Event */
int se_task_usleep(__async__, sd_event *event, uint64_t usec);
int se_task_create(sd_event *event, size_t stack_size, s_task_fn_t entry, void *arg);
void se_task_register_memory_to_free(__async__, void *mem, void (*free_fn)(void *));

void interrupt_all_tasks(void *reason);
bool is_task_empty(void);
void *get_interrupt_reason(__async__);
size_t get_current_task_id(__async__);
void set_interrupt_disabled(__async__, int disabled);
extern const struct global_interrupt_reasons{
    uint8_t SYS_WILL_EXIT;
} global_interrupt_reasons;


/* msg stream */
struct msg_stream;
int init_msg_stream(struct msg_stream **stream, sd_event *event, int fd);
void destroy_msg_stream(struct msg_stream *stream);
void msg_stream_reg_interrupt(__async__, struct msg_stream *stream, void *reason);
ssize_t msg_stream_read(__async__, struct msg_stream *stream, void *buf, size_t size, uint64_t usec);
ssize_t msg_stream_write(__async__, struct msg_stream *stream, const void *buf, size_t size, uint64_t usec);
const struct ucred *msg_stream_get_peer_cred(const struct msg_stream *stream);

/* pidfd event*/
struct pidfd_event;
int init_pidfd_event(struct pidfd_event **pid_event, sd_event *event, pid_t pid);
void destroy_pidfd_event(struct pidfd_event *pid_event);
void pidfd_event_reg_interrupt(__async__, struct pidfd_event *event, void *reason);
int pidfd_event_wait_for_exit(__async__, struct pidfd_event *event);

/* sd_bus */
/* sb is short for S_event systemd-Bus*/
struct bus_locator{
    const char *destination;
    const char *path;
    const char *interface;
};
extern const struct bus_locator * const bus_systemd_mgr;
int sb_bus_call(__async__, sd_bus *bus, sd_bus_message *m, sd_bus_message **result, uint64_t usec);
int sb_bus_call_methodv(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, sd_bus_message **result, const char *types, va_list ap);
int sb_bus_call_method(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, sd_bus_message **result, const char *types, ...);
int sb_sd_GetUnitByPID(__async__, sd_bus *bus, pid_t pid, char **result);
int sb_bus_call_systemd_method(__async__, sd_bus *bus, const char *member, sd_bus_message **result, const char *types, ...);
int sb_bus_call_unit_method(__async__, sd_bus *bus, const char *path, const char *member, sd_bus_message **result, const char *types, ...);
int sb_bus_get_property(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, sd_bus_message **reply, const char *type);
int sb_bus_get_property_trivial(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, char type, void *ptr);
int sb_bus_get_property_string(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, char **result);
int sb_sd_Unit_Get_subprop(__async__, sd_bus *bus, const char *unit_obj, const char *member, sd_bus_message **reply, const char *type);
int sb_Unit_Get_subprop_string(__async__, sd_bus *bus, const char *unit_obj, const char *member, char **result);

struct sb_sd_wait_for_job_arg {
    s_event_t event;
    sd_bus *bus;
    sd_bus_slot *slot_job_removed;
    sd_bus_slot *slot_disconnected;
    const char * job_obj;
    char *result;
    int rc;
    int closed;
};
int sb_sd_init_wait_for_job(__async__, sd_bus *bus, struct sb_sd_wait_for_job_arg *arg);
void sb_sd_free_wait_for_job(struct sb_sd_wait_for_job_arg *arg);
int sb_sd_wait_for_job(__async__, struct sb_sd_wait_for_job_arg *arg, const char * const job_obj, char **result);

int start_transient_scope(__async__, sd_bus *bus, pid_t pid, char **out_scope_name, char **out_scope_obj);

#define DEF_SB_SD_UNIT_GET_STR_PROP(__type__, __name__) \
    static inline int sb_sd_##__type__##_Get_##__name__(__async__, sd_bus *bus, const char *unit_obj, char **result){ \
        return sb_bus_get_property_string(__await__, bus, &(struct bus_locator){ \
            .destination = bus_systemd_mgr->destination, \
            .path = unit_obj, \
            .interface = "org.freedesktop.systemd1."#__type__, \
        }, #__name__, result); \
    }
DEF_SB_SD_UNIT_GET_STR_PROP(Unit, Id)


#endif /* defined(INC_SE_LIBS_H_) */
