#define _GNU_SOURCE // for SOCK_NONBLOCK and SOCK_CLOEXEC
#include <stdlib.h>
#include <systemd/sd-event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-daemon.h>
#include <fcntl.h>
#include <assert.h>

#include <log.h>
#include "daemon.h"

struct listen_handler_arg {
    struct daemon *daemon;
    void (*handler)(int fd);
};

static int unix_server_listen_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata){

    assert(s);
    assert(userdata);

    (void) revents;

    struct listen_handler_arg *arg = (struct listen_handler_arg *)userdata;
    void (*handler)(int fd) = arg->handler;

    int cfd = -1;
    int rc = 0;

    cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if(cfd < 0){
        log_error("accept4() failed: %s", strerror(errno));
        rc = -errno;
        return rc;
    }

    handler(cfd);

    return 0;
}

static void unix_server_listen_destroy_handler(void *userdata){

    assert(userdata);

    free(userdata);
}

int setup_unix_listening_socket(struct daemon *daemon, void (*handler)(int fd)){

    assert(daemon);
    assert(handler);
    assert(daemon->event_loop);

    int rc = 0;
    int fd = -1;

    int destroy_handler_armed = 0;
    int fd_owned = 0;

    if(sd_listen_fds(0) < 1){
        log_error("No listening sockets passed to us by systemd");
        rc = -EINVAL;
        return rc;
    }

    fd = SD_LISTEN_FDS_START + 0;
    if(!sd_is_socket(fd, AF_UNIX, SOCK_SEQPACKET, 1)){
        log_error("File descriptor %d is not a UNIX SEQPACKET listening socket", fd);
        rc = -EINVAL;
        return rc;
    }

    {
        int flags = fcntl(fd, F_GETFL);
        if(flags == -1){
            log_error("fcntl(F_GETFL) failed: %s", strerror(errno));
            rc = -errno;
            return rc;
        }
        if(!(flags & O_NONBLOCK)){
            log_warn("Listening socket is not non-blocking, setting O_NONBLOCK");
            flags |= O_NONBLOCK;
            rc = fcntl(fd, F_SETFL, flags);
            if(rc == -1){
                log_error("fcntl(F_SETFL) failed: %s", strerror(errno));
                rc = -errno;
                return rc;
            }
        }
    }

    struct listen_handler_arg *arg = malloc(sizeof(*arg));
    if(arg == NULL){
        log_error("malloc() failed: %s", strerror(errno));
        rc = -errno;
        goto err_close_fd;
    }

    arg->daemon = daemon;
    arg->handler = handler;

    rc = sd_event_add_io(
        daemon->event_loop, &daemon->server_unix_sock_event_source, fd, EPOLLIN, unix_server_listen_handler, arg
    );

    if(rc < 0){
        log_error("sd_event_add_io() failed: %s", strerror(-rc));
        goto err_free_mem;
    }

    rc = sd_event_source_set_io_fd_own(daemon->server_unix_sock_event_source, true);

    if(rc < 0){
        log_error("sd_event_source_set_io_fd_own() failed: %s", strerror(-rc));
        goto err_unref_src;
    }

    fd_owned = 1;

    rc = sd_event_source_set_destroy_callback(daemon->server_unix_sock_event_source, unix_server_listen_destroy_handler);

    if(rc < 0){
        log_error("sd_event_source_set_destroy_callback() failed: %s", strerror(-rc));
        goto err_unref_src;
    }
    destroy_handler_armed = 1;

    sd_event_source_set_description(daemon->server_unix_sock_event_source, "unix_server_listen_handler");

    return rc;

err_unref_src:
    sd_event_source_disable_unrefp(&daemon->server_unix_sock_event_source);

err_free_mem:
    if(!destroy_handler_armed){
        free(arg);
    }

err_close_fd:
    if(!fd_owned){
        close(fd);
    }
    return rc;
}
