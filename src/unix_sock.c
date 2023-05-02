#define _GNU_SOURCE // for SOCK_NONBLOCK and SOCK_CLOEXEC
#include <log.h>
#include <stdlib.h>
#include <systemd/sd-event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "daemon.h"

struct listen_handler_arg {
    struct daemon *daemon;
    const char *server_path;
    void (*handler)(int fd);
};

static int unix_server_listen_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata){
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
    int rc = 0;
    struct listen_handler_arg *arg = (struct listen_handler_arg *)userdata;
    rc = unlink(arg->server_path);
    if(rc < 0){
        log_error("unlink(%s) failed: %s (ignored)", arg->server_path, strerror(errno));
    }
    free((void *)arg->server_path);
    free(userdata);
}

int setup_unix_listening_socket(struct daemon *daemon, void (*handler)(int fd)){
    int rc = 0;
    int fd = -1;

    int destroy_handler_armed = 0;
    int fd_owned = 0;

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(fd < 0){
        log_error("socket() failed: %s", strerror(errno));
        return -errno;
    }
    {
        int yes = 1;
        rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(rc < 0){
            log_error("setsockopt(SOL_SOCKET, SO_REUSEADDR) failed: %s", strerror(errno));
            rc = -errno;
            goto err_close_fd;
        }
    }
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, daemon->server_path, sizeof(addr.sun_path) - 1);

    rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if(rc < 0){
        log_error("bind() failed: %s", strerror(errno));
        rc = -errno;
        goto err_close_fd;
    }

    rc = listen(fd, SOMAXCONN);
    if(rc < 0){
        log_error("listen() failed: %s", strerror(errno));
        rc = -errno;
        goto err_close_fd;
    }

    struct listen_handler_arg *arg = malloc(sizeof(*arg));
    if(arg == NULL){
        log_error("malloc() failed: %s", strerror(errno));
        rc = -errno;
        goto err_close_fd;
    }

    arg->daemon = daemon;
    arg->handler = handler;
    arg->server_path = strdup(addr.sun_path);

    if(arg->server_path == NULL){
        log_error("strdup() failed: %s", strerror(errno));
        rc = -errno;
        goto err_free_mem;
    }

    rc = sd_event_add_io(
        daemon->event_loop, &daemon->server_unix_sock_event_source, fd, EPOLLIN, unix_server_listen_handler, arg
    );

    if(rc < 0){
        log_error("sd_event_add_io() failed: %s", strerror(-rc));
        goto err_free_str;
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

err_free_str:
    if(!destroy_handler_armed){
        free((void *)arg->server_path);
    }

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
