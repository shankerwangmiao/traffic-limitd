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
#include <protocol.h>
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
Usage: %s [OPTION]... [--] COMMAND [ARG]...\n\
", program_name);
        fputs("\
\n\
  -p, --packet-rate=RATE          limit packet rate to RATE (default: no limit)\n\
  -b, --bit-rate=RATE             limit bit rate to RATE (default: no limit)\n\
  -w, --wait=WAIT_TIME            wait for available resource for at most WAIT_TIME seconds (default: infinity) \n\
  -c, --control-socket=PATH       use PATH as control socket (default:"DEFAULT_CONTROL_SOCKET")\n\
", stdout);
        fputs("\
\n\
RATE can be suffixed with K, M, G, T to denote 1e3, 1e6, 1e9, 1e12 bits per second, respectively.\n\
\n\
WAIT_TIME can be suffixed with m, h, d to denote minutes, hours, days, respectively.\n\
When WAIT_TIME is 0, this command will fail immediately when no resource available.\n\
", stdout);
    }
}

static struct option const long_options[] =
{
    {"packet-rate", required_argument, NULL, 'p'},
    {"bit-rate", required_argument, NULL, 'b'},
    {"wait", required_argument, NULL, 'w'},
    {"control-socket", required_argument, NULL, 'c'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
};

enum parse_suffix_result{
    PARSE_SUFFIX_OK = 0,
    PARSE_SUFFIX_INVALID = -1,
    PARSE_SUFFIX_INVALID_SUFFIX = -2,
};
static enum parse_suffix_result parseRate(const char *string, uint64_t *out_rate){
    uint64_t raw_rate;
    uint64_t multiplier = 1;
    char multiplier_suffix;
    int count = sscanf(string, "%lu%c", &raw_rate, &multiplier_suffix);
    if(count < 1){
        return PARSE_SUFFIX_INVALID;
    }
    if(count == 2){
        switch (multiplier_suffix){
            case 'T': case 't':
                multiplier *= 1000;
                // fall through
            case 'G': case 'g':
                multiplier *= 1000;
                // fall through
            case 'M': case 'm':
                multiplier *= 1000;
                // fall through
            case 'K': case 'k':
                multiplier *= 1000;
                break;
            default:
                return PARSE_SUFFIX_INVALID_SUFFIX;
        }
    }
    *out_rate = raw_rate * multiplier;
    return 0;
}
static enum parse_suffix_result parseTime(const char *string, int64_t *out_time){
    int64_t raw_time;
    uint64_t multiplier = 1;
    char multiplier_suffix;
    int count = sscanf(string, "%lu%c", &raw_time, &multiplier_suffix);
    if(count < 1){
        return PARSE_SUFFIX_INVALID;
    }
    if(raw_time < 0){
        return PARSE_SUFFIX_INVALID;
    }
    if(count == 2){
        switch (multiplier_suffix){
            case 'd': case 'D':
                multiplier *= 24;
                // fall through
            case 'h': case 'H':
                multiplier *= 60;
                // fall through
            case 'm': case 'M':
                multiplier *= 60;
                break;
            default:
                return PARSE_SUFFIX_INVALID_SUFFIX;
        }
    }
    *out_time = raw_time * multiplier;
    return 0;
}


static volatile int timeout_triggered = 0;

static void timeout_handler(int sig){
    timeout_triggered = 1;
}

int main(int argc, char *argv[]){
    program_name = argv[0];
    struct {
        uint64_t packet_rate;
        uint64_t byte_rate;
        int64_t wait_time;
        const char *control_socket;
    } options = {
        .packet_rate = 0,
        .byte_rate = 0,
        .wait_time = -1,
        .control_socket = DEFAULT_CONTROL_SOCKET,
    };

    int opt;

    if(argc == 1){
        usage(0);
        return 0;
    }

    while ((opt = getopt_long (argc, argv, "p:b:w:c:h", long_options, NULL)) != -1){
        switch(opt){
            case 'p':
                if(parseRate(optarg, &options.packet_rate) != PARSE_SUFFIX_OK){
                    fprintf(stderr, "Invalid packet rate: \"%s\"\n", optarg);
                    return 1;
                }
                break;
            case 'b':
                if(parseRate(optarg, &options.byte_rate) != PARSE_SUFFIX_OK){
                    fprintf(stderr, "Invalid bit rate: \"%s\"\n", optarg);
                    return 1;
                }
                options.byte_rate /= 8;
                break;
            case 'w':
                if(parseTime(optarg, &options.wait_time) != PARSE_SUFFIX_OK){
                    fprintf(stderr, "Invalid wait time: \"%s\"\n", optarg);
                    return 1;
                }
                break;
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
        fprintf(stderr, "No command specified\n");
        usage(1);
        return 1;
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
    strncpy(control_sock_addr.sun_path, options.control_socket, sizeof(control_sock_addr.sun_path));
    rc = connect(control_sock_fd, (struct sockaddr *)&control_sock_addr, sizeof(control_sock_addr));
    if(rc < 0){
        perror("unable to connect to control socket");
        return 1;
    }
    char send_buf[sizeof(struct rate_limit_msg) + sizeof(struct rate_limit_req_attr)];
    struct rate_limit_msg *req_msg = (struct rate_limit_msg *)send_buf;
    struct rate_limit_req_attr *req_attr = (struct rate_limit_req_attr *)(&req_msg->attr);
    req_msg->length = sizeof(send_buf);
    req_msg->type = RATE_LIMIT_REQ;
    req_attr->limit.byte_rate = options.byte_rate == 0 ? RATE_UNLIMITED : options.byte_rate;
    req_attr->limit.packet_rate = options.packet_rate == 0 ? RATE_UNLIMITED : options.packet_rate;
    req_attr->flags = 0;
    req_attr->flags |= options.wait_time < 0 ? RATE_LIMIT_REQ_NOWAIT : 0;

    rc = send(control_sock_fd, send_buf, sizeof(send_buf), 0);
    if(rc < 0){
        perror("unable to send request");
        return 1;
    }

    sighandler_t orig_sig;
    if(options.wait_time > 0){
        // set timeout handler
        orig_sig = signal(SIGALRM, timeout_handler);
        if(orig_sig == SIG_ERR){
            perror("unable to set signal handler");
            return 1;
        }
        alarm(options.wait_time);
    }

    while(1){
        ssize_t len = recv(control_sock_fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
        if (len < 0){
            if(errno == EINTR){
                goto interrupted;
            }
            perror("unable to receive response");
            return 1;
        }else if(len == 0){
            fprintf(stderr, "unexcepted connection closed\n");
            return 1;
        }
        {
            char recv_buf[len];
            len = recv(control_sock_fd, recv_buf, len, MSG_DONTWAIT);
            if(len < 0){
                if(errno == EINTR){
                    goto interrupted;
                }else if(errno == EWOULDBLOCK){
                    continue;
                }
                perror("unable to receive response");
                return 1;
            }else if(len == 0){
                fprintf(stderr, "unexcepted connection closed\n");
                return 1;
            }
            struct rate_limit_msg *resp_msg = (struct rate_limit_msg *)recv_buf;
            if((size_t)len < sizeof(struct rate_limit_msg)){
                fprintf(stderr, "invalid response received from daemon\n");
                return 1;
            }
            len = resp_msg->length < (size_t)len ? resp_msg->length : (size_t)len;
            switch(resp_msg->type){
                case RATE_LIMIT_FAIL:{
                    struct rate_limit_fail_attr *fail_attr = (struct rate_limit_fail_attr *)resp_msg->attr;
                    if((size_t)len < sizeof(struct rate_limit_fail_attr) + sizeof(struct rate_limit_msg)){
                        fprintf(stderr, "invalid response received from daemon\n");
                        return 1;
                    }
                    const char *reason;
                    switch (fail_attr->reason){
                        default:
                            /*fall through*/
                        case RATE_LIMIT_FAIL_UNKNOWN:
                            reason = "reason unknown";
                            break;
                        case RATE_LIMIT_FAIL_WILL_WAIT:
                            reason = "no enough resource";
                            break;
                        case RATE_LIMIT_FAIL_INTERNAL:
                            reason = "internal error";
                            break;
                        case RATE_LIMIT_FAIL_NORESOURCE:
                            reason = "too many rate limited tasks";
                            break;
                    }
                    fprintf(stderr, "unable to start task: %s\n", reason);
                    return 1;
                    break;
                }
                case RATE_LIMIT_LOG:
                    fprintf(stderr, "%.*s\n", (int)((size_t)len - sizeof(struct rate_limit_msg)), resp_msg->attr);
                    break;
                case RATE_LIMIT_PROCEED:
                    goto out_recv_loop;
                    break;
                default:
                    fprintf(stderr, "invalid response received from daemon\n");
                    return 1;
                    break;
            }
        }

        continue;
    interrupted:
        if(options.wait_time > 0 && timeout_triggered){
            fprintf(stderr, "unable to start task: waited too long for available resource\n");
            return 1;
        }
    };
out_recv_loop:
    if(options.wait_time > 0){
        alarm(0);
        // restore signal handler
        orig_sig = signal(SIGALRM, orig_sig);
        if(orig_sig == SIG_ERR){
            perror("unable to restore signal handler");
            return 1;
        }
    }

    char *arg_cmdline[argc + 1];
    for(int i = 0; i < argc; i++){
        arg_cmdline[i] = argv[i];
    }
    arg_cmdline[argc] = NULL;
    rc = execvp(arg_cmdline[0], arg_cmdline);
    if(rc < 0){
        perror("unable to execute command");
        return 1;
    }
    return 0;
}
