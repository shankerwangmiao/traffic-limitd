#define _GNU_SOURCE
#include "daemon.h"
#include <log.h>
#include <s_task.h>
#include <se_libs.h>
#include <fcntl.h>
#include <errno.h>

const struct bus_locator * const bus_systemd_mgr = &(struct bus_locator){
    .destination = "org.freedesktop.systemd1",
    .path = "/org/freedesktop/systemd1",
    .interface = "org.freedesktop.systemd1.Manager"
};

int initialize_sd_bus(struct daemon *daemon){
    int rc;
    rc = sd_bus_default_system(&daemon->sd_bus);
    if(rc < 0){
        log_error("sd_bus_default_system failed: %s", strerror(-rc));
        return rc;
    }
    rc = sd_bus_attach_event(daemon->sd_bus, daemon->event_loop, SD_EVENT_PRIORITY_NORMAL);
    if(rc < 0){
        log_error("sd_bus_attach_event failed: %s", strerror(-rc));
        goto err_clean_bus;
    }

    return 0;

err_clean_bus:
    sd_bus_unref(daemon->sd_bus);
    daemon->sd_bus = NULL;
    return rc;
}

struct sb_bus_call_arg {
    sd_bus_message *result;
    s_event_t finish_event;
    int error;
};

static int sb_bus_call_cb(sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
    struct sb_bus_call_arg *arg = userdata;
    if(sd_bus_message_is_method_error(m, NULL)){
        arg->error = -sd_bus_message_get_errno(m);
    }else{
        arg->error = 0;
        arg->result = sd_bus_message_ref(m);
    }
    s_event_set(&arg->finish_event);
    return 0;
}

int sb_bus_call(__async__, sd_bus *bus, sd_bus_message *m, sd_bus_message **result, uint64_t usec){
    sd_bus_slot *slot = NULL;
    struct sb_bus_call_arg arg = {
        .result = NULL,
        .error = 0,
    };
    s_event_init(&arg.finish_event);
    int rc = 0;

    rc = sd_bus_call_async(bus, &slot, m, sb_bus_call_cb, &arg, usec);
    if(rc < 0){
        alog_error("sd_bus_call_async failed: %s", strerror(-rc));
        return rc;
    }
    rc = s_event_wait(__await__, &arg.finish_event);
    if(rc < 0){
        alog_info("wait interrupted");
        rc = -EINTR;
        goto err_free_slot;
    }
    if(arg.error < 0){
        alog_error("sd_bus_call_async return failed: %s", strerror(-arg.error));
        rc = arg.error;
        goto err_free_slot;
    }
    *result = arg.result;
    rc = 0;

err_free_slot:
    sd_bus_slot_unref(slot);
    return rc;
}

static inline bool isempty(const char *a) {
        return !a || a[0] == '\0';
}

static inline const char *strempty(const char *s) {
        return s ?: "";
}

#define CHAR_TO_STR(x) ((char[2]) { x, 0 })

int sb_bus_call_methodv(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, sd_bus_message **result, const char *types, va_list ap) {
    sd_bus_message *m = NULL;
    int rc = 0;
    rc = sd_bus_message_new_method_call(bus, &m, locator->destination, locator->path, locator->interface, member);
    if (rc < 0){
        alog_error("sd_bus_message_new_method_call failed: %s", strerror(-rc));
        return rc;
    }

    if (!isempty(types)) {
        rc = sd_bus_message_appendv(m, types, ap);
        if (rc < 0){
            alog_error("sd_bus_message_appendv failed: %s", strerror(-rc));
            goto err_unref_msg;
        }
    }

    rc = sb_bus_call(__await__, bus, m, result, 0);

err_unref_msg:
    sd_bus_message_unref(m);
    return rc;
}

int sb_bus_call_method(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, sd_bus_message **result, const char *types, ...) {
    va_list ap;
    int rc;

    va_start(ap, types);
    rc = sb_bus_call_methodv(__await__, bus, locator, member, result, types, ap);
    va_end(ap);

    return rc;
}

int sb_bus_call_systemd_method(__async__, sd_bus *bus, const char *member, sd_bus_message **result, const char *types, ...){
    va_list ap;
    int rc;

    va_start(ap, types);
    rc = sb_bus_call_methodv(__await__, bus, bus_systemd_mgr, member, result, types, ap);
    va_end(ap);

    return rc;
}

int sb_bus_call_unit_method(__async__, sd_bus *bus, const char *path, const char *member, sd_bus_message **result, const char *types, ...){
    va_list ap;
    int rc;
    const struct bus_locator unit_locator = {
        .destination = bus_systemd_mgr->destination,
        .path = path,
        .interface = "org.freedesktop.systemd1.Unit",
    };

    va_start(ap, types);
    rc = sb_bus_call_methodv(__await__, bus, &unit_locator, member, result, types, ap);
    va_end(ap);

    return rc;
}

int sb_bus_get_property(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, sd_bus_message **reply, const char *type) {
    sd_bus_message *rep = NULL;
    const struct bus_locator prop_locator = {
        .destination = locator->destination,
        .path = locator->path,
        .interface = "org.freedesktop.DBus.Properties",
    };
    int rc = 0;

    rc = sb_bus_call_method(__await__, bus, &prop_locator, "Get", &rep, "ss", strempty(locator->interface), member);
    if (rc < 0){
        alog_error("sb_bus_call_method(Get) failed: %s", strerror(-rc));
        goto fail;
    }

    rc = sd_bus_message_enter_container(rep, 'v', type);
    if (rc < 0) {
        alog_error("sd_bus_message_enter_container failed: %s", strerror(-rc));
        goto err_unref_msg;
    }
    *reply = sd_bus_message_ref(rep);
    rc = 0;
err_unref_msg:
    sd_bus_message_unref(rep);
fail:
    return rc;
}
int sb_bus_get_property_trivial(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, char type, void *ptr) {
    int rc = 0;
    sd_bus_message *rep = NULL;
    rc = sb_bus_get_property(__await__, bus, locator, member, &rep, CHAR_TO_STR(type));
    if(rc < 0){
        alog_error("sb_bus_get_property failed: %s", strerror(-rc));
        goto fail;
    }
    rc = sd_bus_message_read_basic(rep, type, ptr);
    if(rc < 0){
        alog_error("sd_bus_message_read_basic failed: %s", strerror(-rc));
        goto err_unref_msg;
    }
    rc = 0;
err_unref_msg:
    sd_bus_message_unref(rep);
fail:
    return rc;
}

int sb_bus_get_property_string(__async__, sd_bus *bus, const struct bus_locator *locator, const char *member, char **result) {
    int rc = 0;
    sd_bus_message *rep = NULL;
    const char *prop_str;
    char *n;
    rc = sb_bus_get_property(__await__, bus, locator, member, &rep, "s");
    if(rc < 0){
        alog_error("sb_bus_get_property failed: %s", strerror(-rc));
        goto fail;
    }
    rc = sd_bus_message_read_basic(rep, 's', &prop_str);
    if(rc < 0){
        alog_error("sd_bus_message_read_basic failed: %s", strerror(-rc));
        goto err_unref_msg;
    }
    n = strdup(prop_str);
    if(n == NULL){
        rc = -errno;
        alog_error("strdup failed: %s", strerror(-rc));
        goto err_unref_msg;
    }
    *result = n;
    rc = 0;
err_unref_msg:
    sd_bus_message_unref(rep);
fail:
    return rc;
}

int sb_sd_GetUnitByPID(__async__, sd_bus *bus, pid_t pid, char **result){
    int rc;
    sd_bus_message *result_msg = NULL;
    const char *unit = NULL;
    char *unit_dup = NULL;
    rc = sb_bus_call_systemd_method(__await__, bus, "GetUnitByPID", &result_msg, "u", pid);
    if(rc < 0){
        alog_error("sb_bus_call_method(GetUnitByPID) failed: %s", strerror(-rc));
        goto err_bus_call;
    }
    rc = sd_bus_message_read(result_msg, "o", &unit);
    if(rc < 0){
        alog_error("sd_bus_message_read(GetUnitByPID) failed: %s", strerror(-rc));
        goto err_unref_msg;
    }
    unit_dup = strdup(unit);
    if(unit_dup == NULL){
        rc = -errno;
        alog_error("strdup failed: %s", strerror(-rc));
        goto err_unref_msg;
    }
    *result = unit_dup;
err_unref_msg:
    sd_bus_message_unref(result_msg);
err_bus_call:
    return rc;
}

int start_transient_scope(__async__, sd_bus *bus, pid_t pid, char **cgroup_name){
    return 0;
}
