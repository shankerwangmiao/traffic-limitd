#include "daemon.h"
#include <log.h>

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
