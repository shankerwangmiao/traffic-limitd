#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#include <sys/socket.h>
#include <argp.h>

#define emit_try_help() \
  do \
    { \
      fprintf (stderr, "Try '%s --help' for more information.\n", \
               program_name); \
    } \
  while (0)

#define DEFAULT_CONTROL_SOCKET "/run/traffic-limitd.sock"

const char *program_name = NULL;

static void usage (int status){
    if (status != 0){
        emit_try_help();
    }else{
        printf("\
Usage: %s [OPTION]... [--] [HOOK-TYPE]\n\
", program_name);
        fputs("\
\n\
  -c, --control-socket=PATH       use PATH as control socket (default:"DEFAULT_CONTROL_SOCKET")\n\
", stdout);
    }
}

static struct option const long_options[] =
{
    {"control-socket", required_argument, NULL, 'c'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
};

int main(int argc, char *argv[]){
    program_name = argv[0];
    struct {
        const char *control_socket;
    } options = {
        .control_socket = DEFAULT_CONTROL_SOCKET,
    };;

    if(argc == 1){
        usage(0);
        return 0;
    }

    int opt;

    while ((opt = getopt_long (argc, argv, "+c:h", long_options, NULL)) != -1){
        switch(opt){
            case 'c':
                options.control_socket = optarg;
                break;
            case 'h':
                usage(0);
                return 0;
                break;
            default:
                usage(1);
                return 1;
                break;
        }
    }

    argc -= optind;
    argv += optind;

    if(argc == 0){
        fprintf(stderr, "No hook-type specified\n");
        usage(1);
        return 1;
    }


    int pkt_size = 0;
    int buf_size = 0;
    pkt_size = buf_size = strlen(argv[0]) + 1;
    buf_size *= 2;
    char *send_buf = malloc(buf_size);
    if(send_buf == NULL){
        fprintf(stderr, "unable to allocate memory\n");
        return 1;
    }
    strncpy(send_buf, argv[0], buf_size);
    while(1){
        int rc = read(STDIN_FILENO, send_buf + pkt_size, buf_size - pkt_size);
        if(rc < 0){
            if(errno == EINTR){
                continue;
            }
            perror("unable to read from stdin");
            return 1;
        }else{
            pkt_size += rc;
            if(pkt_size >= buf_size){
                buf_size *= 2;
                send_buf = realloc(send_buf, buf_size);
                if(send_buf == NULL){
                    fprintf(stderr, "unable to allocate memory\n");
                    return 1;
                }
            }
            if(rc == 0){
                send_buf[pkt_size++] = '\0';
                break;
            }
        }
    }

    int control_sock_fd;
    int rc = 0;
    struct sockaddr_un control_sock_addr;
    control_sock_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if(control_sock_fd < 0){
        perror("unable to create socket");
        return 1;
    }
    control_sock_addr.sun_family = AF_UNIX;
    strncpy(control_sock_addr.sun_path, options.control_socket, sizeof(control_sock_addr.sun_path) - 1);
    rc = connect(control_sock_fd, (struct sockaddr *)&control_sock_addr, sizeof(control_sock_addr));
    if(rc < 0){
        perror("unable to connect to control socket");
        return 1;
    }

    rc = send(control_sock_fd, send_buf, pkt_size, 0);
    if(rc < 0){
        perror("unable to send request");
        return 1;
    }


    while(1){
        ssize_t len = recv(control_sock_fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
        if (len < 0){
            if(errno == EINTR){
                continue;
            }
            perror("unable to receive response");
            return 1;
        }else if(len == 0){
            fprintf(stderr, "Unsuccessful response\n");
            return 1;
        }
        {
            char recv_buf[len];
            len = recv(control_sock_fd, recv_buf, len, MSG_DONTWAIT);
            if(len < 0){
                if(errno == EINTR){
                    continue;
                }else if(errno == EWOULDBLOCK){
                    continue;
                }
                perror("unable to receive response");
                return 1;
            }else if(len == 0){
                fprintf(stderr, "Unsuccessful response\n");
                return 1;
            }
            if(strncmp(recv_buf, "success", 7) == 0){
                break;
            }else{
                fprintf(stderr, "Malformed response\n");
                return 1;
            }
        }
    };
    shutdown(control_sock_fd, SHUT_RDWR);
    close(control_sock_fd);
    return 0;
}
