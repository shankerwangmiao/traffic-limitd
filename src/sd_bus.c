#define _GNU_SOURCE
#include "daemon.h"
#include <log.h>
#include <s_task.h>
#include <se_libs.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <systemd/sd-id128.h>

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
    if(result){
        *result = arg.result;
    }else{
        sd_bus_message_unref(arg.result);
        arg.result = NULL;
    }
    rc = 0;

err_free_slot:
    sd_bus_slot_unref(slot);
    return rc;
}

static inline int bus_message_new_method_call(sd_bus *bus, sd_bus_message **m, const struct bus_locator *locator, const char *member) {
        return sd_bus_message_new_method_call(bus, m, locator->destination, locator->path, locator->interface, member);
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

#define SD_INTERFACE_NAME "org.freedesktop.systemd1."

int sb_sd_Unit_Get_subprop(__async__, sd_bus *bus, const char *unit_obj, const char *member, sd_bus_message **reply, const char *type) {
    int rc = 0;
    char *unit_name = NULL;
    rc = sb_sd_Unit_Get_Id(__await__, bus, unit_obj, &unit_name);
    if(rc < 0){
        alog_error("sb_sd_Unit_Get_Id failed: %s", strerror(-rc));
        goto fail;
    }
    int len = strlen(unit_name);
    if(len < 1){
        rc = -EINVAL;
        alog_error("unit_name is empty");
        goto fail_free_unit_name;
    }
    char *kind = NULL;
    for(int i = len - 1; i >= 0; i--){
        if(unit_name[i] == '.'){
            kind = unit_name + i + 1;
            break;
        }
    }
    if(kind == NULL || *kind == '\0'){
        rc = -EINVAL;
        alog_error("unit_name is invalid, cannot determine kind");
        goto fail_free_unit_name;
    }
    kind[0]=toupper(kind[0]);
    {
        char interface_buf[sizeof(SD_INTERFACE_NAME) + len - (kind - unit_name) + 10];
        sprintf(interface_buf, SD_INTERFACE_NAME "%s", kind);
        const struct bus_locator locator = {
            .destination = "org.freedesktop.systemd1",
            .path = unit_obj,
            .interface = interface_buf,
        };
        rc = sb_bus_get_property(__await__, bus, &locator, member, reply, type);
        if(rc < 0){
            alog_error("sb_bus_get_property failed: %s", strerror(-rc));
            goto fail_free_unit_name;
        }
        rc = 0;
    }

fail_free_unit_name:
    free(unit_name);
fail:
    return rc;
}

int sb_Unit_Get_subprop_string(__async__, sd_bus *bus, const char *unit_obj, const char *member, char **result) {
    int rc = 0;
    sd_bus_message *rep = NULL;
    const char *prop_str;
    char *n;
    rc = sb_sd_Unit_Get_subprop(__await__, bus, unit_obj, member, &rep, "s");
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

void sb_sd_free_wait_for_job(struct sb_sd_wait_for_job_arg *arg){
    if(!arg->closed){
        if(arg->slot_disconnected){
            sd_bus_slot_unref(arg->slot_disconnected);
            arg->slot_disconnected = NULL;
        }
        if(arg->slot_job_removed){
            sd_bus_slot_unref(arg->slot_job_removed);
            arg->slot_job_removed = NULL;
        }
        arg->closed = 1;
        sd_bus_unref(arg->bus);
    }
}

static int sb_sd_wait_for_job_bus_disconnected(sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
    struct sb_sd_wait_for_job_arg *arg = userdata;
    log_error("bus disconnected event");
    arg->rc = -ECONNRESET;
    char *result = strdup("disconnected");
    if(!result){
        log_error("strdup failed: %s", strerror(errno));
        /* ignore error */
    }
    arg->result = result;
    sb_sd_free_wait_for_job(arg);
    s_event_set(&arg->event);
    return 0;
}

static int sb_sd_wait_for_job_job_removed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
    struct sb_sd_wait_for_job_arg *arg = userdata;
    int rc;
    uint32_t id;
    const char *path, *unit, *result;
    if(arg->job_obj == NULL){
        return 0;
    }
    rc = sd_bus_message_read(m, "uoss", &id, &path, &unit, &result);
    if(rc < 0){
        log_error("sd_bus_message_read failed: %s", strerror(-rc));
        return 0;
    }

    if(strcmp(arg->job_obj, path) != 0){
        return 0;
    }

    log_trace("sb_sd_wait_for_job: job_removed with result: %s, for unit %s", result, unit);
    arg->job_obj = NULL;

    char *new_result = strdup(result);
    if(!new_result){
        log_error("strdup failed: %s", strerror(errno));
        /* ignore error */
    }
    arg->result = new_result;

    if(strcmp(result, "cancelled") == 0 || strcmp(result, "collected") == 0){
        arg->rc = -ECANCELED;
    }else if(strcmp(result, "timeout") == 0){
        arg->rc = -ETIMEDOUT;
    }else if(strcmp(result, "dependency") == 0){
        arg->rc = -EIO;
    }else if(strcmp(result, "invalid") == 0){
        arg->rc = -ENOEXEC;
    }else if(strcmp(result, "assert") == 0){
        arg->rc = -EPROTO;
    }else if(strcmp(result, "unsupported") == 0){
        arg->rc = -EOPNOTSUPP;
    }else if(strcmp(result, "once") == 0){
        arg->rc = -ESTALE;
    }else if(strcmp(result, "done") == 0 || strcmp(result, "skipped") == 0){
        arg->rc = 0;
    }else{
        arg->rc = -EIO;
    }
    s_event_set(&arg->event);
    return 0;
}

static int sb_sd_wait_for_job_signal_match_installed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
    struct sb_sd_wait_for_job_arg *arg = userdata;
    if(sd_bus_message_is_method_error(m, NULL)){
        arg->rc = -sd_bus_message_get_errno(m);
    }else{
        arg->rc = 0;
    }

    s_event_set(&arg->event);
    return 0;
}

int sb_sd_init_wait_for_job(__async__, sd_bus *bus, struct sb_sd_wait_for_job_arg *arg){
    int rc = 0;
    *arg = (struct sb_sd_wait_for_job_arg){
        .job_obj = NULL,
        .result = NULL,
        .slot_job_removed = NULL,
        .slot_disconnected = NULL,
        .rc = 0,
        .bus = sd_bus_ref(bus),
        .closed = 1,
    };
    s_event_init(&arg->event);

    rc = sd_bus_match_signal_async(bus, &arg->slot_job_removed, sd_bus_is_bus_client(bus) ? "org.freedesktop.systemd1" : NULL, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "JobRemoved", sb_sd_wait_for_job_job_removed, sb_sd_wait_for_job_signal_match_installed, arg);
    if(rc < 0){
        alog_error("sd_bus_match_signal_async(JobRemoved) failed: %s", strerror(-rc));
        goto fail;
    }

    rc = s_event_wait(__await__, &arg->event);
    if(rc < 0){
        alog_info("wait interrupted");
        rc = -EINTR;
        goto fail_free_slot_job_removed;
    }
    if(arg->rc < 0){
        rc = arg->rc;
        goto fail_free_slot_job_removed;
    }

// Local signal, installed callback will not be called
    rc = sd_bus_match_signal_async(bus, &arg->slot_disconnected, "org.freedesktop.DBus.Local", NULL, "org.freedesktop.DBus.Local", "Disconnected", sb_sd_wait_for_job_bus_disconnected, NULL, arg);
    if(rc < 0){
        alog_error("sd_bus_match_signal_async(Disconnected) failed: %s", strerror(-rc));
        goto fail_free_slot_job_removed;
    }

    arg->closed = 0;
    rc = 0;

    return rc;

fail_free_slot_job_removed:
    sd_bus_slot_unref(arg->slot_job_removed);
    arg->slot_job_removed = NULL;
fail:
    sd_bus_unref(arg->bus);
    return rc;
}

int sb_sd_wait_for_job(__async__, struct sb_sd_wait_for_job_arg *arg, const char * const job_obj, char **result){
    int rc = 0;
    if(!arg->closed){
        arg->job_obj = job_obj;
        rc = s_event_wait(__await__, &arg->event);
        if(rc < 0){
            alog_info("wait interrupted");
            rc = -EINTR;
            if(result){
                char *new_result = strdup("interrupt");
                if(new_result){
                    *result = new_result;
                }
            }
            goto fail_free_handlers;
        }
    }
    rc = arg->rc;
    if(result && arg->result){
        *result = arg->result;
    }else if(arg->result){
        free(arg->result);
        arg->result = NULL;
    }

    return rc;
fail_free_handlers:
    sb_sd_free_wait_for_job(arg);
    return rc;
}

int start_transient_scope(__async__, sd_bus *bus, pid_t pid, char **out_scope_name, char **out_scope_obj){
    char *unit = NULL;
    int rc = 0;
    rc = sb_sd_GetUnitByPID(__await__, bus, pid, &unit);
    if(rc < 0){
        alog_error("sb_sd_GetUnitByPID failed: %s", strerror(-rc));
        goto fail;
    }
    alog_trace("Orig unit id: %s", unit);

    char *slice = NULL;
    rc = sb_Unit_Get_subprop_string(__await__, bus, unit, "Slice", &slice);
    if (rc < 0){
        alog_error("sb_Unit_Get_subprop_string(Slice) failed: %s", strerror(-rc));
        goto fail_free_unit;
    }
    alog_trace("The slice of the unit: %s", slice);

    sd_id128_t rnd;
    rc = sd_id128_randomize(&rnd);

    if(rc < 0){
        alog_error("sd_id128_randomize failed: %s", strerror(-rc));
        goto fail_free_slice;
    }

    char *scope_name = NULL;
    rc = asprintf(&scope_name, "traffic-limitd-scope-"SD_ID128_FORMAT_STR".scope", SD_ID128_FORMAT_VAL(rnd));
    if(rc < 0){
        rc = -errno;
        alog_error("asprintf failed: %s", strerror(-rc));
        goto fail_free_slice;
    }

    struct sb_sd_wait_for_job_arg wait_for_job_arg;
    rc = sb_sd_init_wait_for_job(__await__, bus, &wait_for_job_arg);
    if(rc < 0){
        alog_error("sb_sd_init_wait_for_job failed: %s", strerror(-rc));
        goto fail_free_scope_name;
    }

    sd_bus_message *m_req = NULL;
    rc = bus_message_new_method_call(bus, &m_req, bus_systemd_mgr, "StartTransientUnit");
    if(rc < 0){
        alog_error("bus_message_new_method_call failed: %s", strerror(-rc));
        goto fail_free_wait_for_job;
    }
     /* Name and Mode */
    rc = sd_bus_message_append(m_req, "ss", scope_name, "fail");
    if(rc < 0){
        alog_error("sd_bus_message_append failed: %s", strerror(-rc));
        goto fail_free_m_req;
    }
    /* Properties */
    rc = sd_bus_message_open_container(m_req, 'a', "(sv)");
    if(rc < 0){
        alog_error("sd_bus_message_open_container failed: %s", strerror(-rc));
        goto fail_free_m_req;
    }
    {
        rc = sd_bus_message_append(m_req, "(sv)", "CollectMode", "s", "inactive-or-failed");
        if(rc < 0){
            alog_error("sd_bus_message_append failed: %s", strerror(-rc));
            goto fail_free_m_req;
        }

        rc = sd_bus_message_append(m_req, "(sv)", "Slice", "s", slice);
        if(rc < 0){
            alog_error("sd_bus_message_append failed: %s", strerror(-rc));
            goto fail_free_m_req;
        }

        rc = sd_bus_message_append(m_req, "(sv)", "PIDs", "au", 1, (uint32_t) pid);
        if(rc < 0){
            alog_error("sd_bus_message_append failed: %s", strerror(-rc));
            goto fail_free_m_req;
        }
    }
    rc = sd_bus_message_close_container(m_req);
    if (rc < 0){
        alog_error("sd_bus_message_close_container failed: %s", strerror(-rc));
        goto fail_free_m_req;
    }
    /* Auxiliary units*/
    rc = sd_bus_message_append(m_req, "a(sa(sv))", 0);
    if (rc < 0){
        alog_error("sd_bus_message_append failed: %s", strerror(-rc));
        goto fail_free_m_req;
    }

    sd_bus_message *m_res = NULL;
    rc = sb_bus_call(__await__, bus, m_req, &m_res, 0);
    if(rc < 0){
        alog_error("sb_bus_call failed: %s", strerror(-rc));
        goto fail_free_m_req;
    }

    const char *job_obj = NULL;
    rc = sd_bus_message_read(m_res, "o", &job_obj);
    if(rc < 0){
        alog_error("sd_bus_message_read failed: %s", strerror(-rc));
        goto fail_free_m_res;
    }
    alog_trace("Job object: %s", job_obj);

    char *job_result = NULL;
    rc = sb_sd_wait_for_job(__await__, &wait_for_job_arg, job_obj, &job_result);
    sb_sd_free_wait_for_job(&wait_for_job_arg);
    if(rc < 0){
        alog_error("sb_sd_wait_for_job failed: %s, because %s", strerror(-rc), job_result);
        free(job_result);
        goto fail_free_m_res;
    }
    free(job_result);

    sd_bus_message *new_scope_obj_msg = NULL;
    rc = sb_bus_call_systemd_method(__await__, bus, "GetUnit", &new_scope_obj_msg, "s", scope_name);

    if(rc < 0){
        alog_error("sb_bus_get_property failed: %s", strerror(-rc));
        goto fail_free_m_res;
    }

    const char *new_scope_obj = NULL;
    rc = sd_bus_message_read(new_scope_obj_msg, "o", &new_scope_obj);
    if(rc < 0){
        alog_error("sd_bus_message_read failed: %s", strerror(-rc));
        goto fail_free_m_scope_obj_msg;
    }
    alog_trace("New scope object: %s", new_scope_obj);

    char *new_out_scope_obj = strdup(new_scope_obj);
    if(!new_out_scope_obj){
        rc = -errno;
        alog_error("strdup failed: %s", strerror(-rc));
        goto fail_free_m_scope_obj_msg;
    }
    rc = 0;
    *out_scope_obj = new_out_scope_obj;
    *out_scope_name = scope_name;
    scope_name = NULL;

fail_free_m_scope_obj_msg:
    sd_bus_message_unref(new_scope_obj_msg);
fail_free_m_res:
    sd_bus_message_unref(m_res);
fail_free_m_req:
    sd_bus_message_unref(m_req);
fail_free_wait_for_job:
    sb_sd_free_wait_for_job(&wait_for_job_arg);
fail_free_scope_name:
    if(scope_name){
        free(scope_name);
    }
fail_free_slice:
    free(slice);
fail_free_unit:
    free(unit);
fail:
    return rc;
}
