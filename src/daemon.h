#include <systemd/sd-event.h>

struct daemon{
    sd_event *event_loop;
    sd_event_source *server_unix_sock_event_source;
};

int setup_unix_listening_socket(struct daemon *daemon, void (*handler)(int fd));
