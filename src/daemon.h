#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>

struct daemon{
    sd_event *event_loop;
    sd_event_source *server_unix_sock_event_source;
    sd_bus *sd_bus;
};

int setup_unix_listening_socket(struct daemon *daemon, void (*handler)(int fd));
int initialize_sd_bus(struct daemon *daemon);
