#include <stdio.h>
#include <linux/netlink.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>


#include <log.h>
#include <rtnl_util.h>


int rtnl_open(struct rtnl_handle *rth){

    assert(rth);

	static const int sndbuf = 32768;
	static const int one = 1;

    int rc = 0;
    memset(rth, 0, sizeof(*rth));

    rc = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if(rc < 0){
        log_error("Cannot open netlink socket: %s", strerror(errno));
        rc = -errno;
        goto fail;
    }
    rth->fd = rc;

    rc = setsockopt(rth->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if(rc < 0){
        log_error("setsockopt(SO_SNDBUF): %s", strerror(errno));
        rc = -errno;
        goto fail_close_fd;
    }
    rc = setsockopt(rth->fd, SOL_SOCKET, SO_RCVBUF, &sndbuf, sizeof(sndbuf));
    if(rc < 0){
        log_error("setsockopt(SO_RCVBUF): %s", strerror(errno));
        rc = -errno;
        goto fail_close_fd;
    }

    rc = setsockopt(rth->fd, SOL_NETLINK, NETLINK_EXT_ACK, &one, sizeof(one));
    /* error ignored */

    memset(&rth->local, 0, sizeof(rth->local));
    rth->local.nl_family = AF_NETLINK;
    rth->local.nl_groups = 0;

    rc = bind(rth->fd, (struct sockaddr *)&rth->local, sizeof(rth->local));
    if(rc < 0){
        log_error("Cannot bind netlink socket: %s", strerror(errno));
        rc = -errno;
        goto fail_close_fd;
    }

    socklen_t addr_len = sizeof(rth->local);
    rc = getsockname(rth->fd, (struct sockaddr *)&rth->local, &addr_len);
    if(rc < 0){
        log_error("Cannot getsockname(): %s", strerror(errno));
        rc = -errno;
        goto fail_close_fd;
    }
    if (addr_len != sizeof(rth->local)) {
		log_error("Address length mismatch, got %d, should be %d", addr_len, sizeof(rth->local));
        rc = -EINVAL;
        goto fail_close_fd;
	}
    if (rth->local.nl_family != AF_NETLINK) {
		fprintf(stderr, "Got wrong address family %d\n", rth->local.nl_family);
        rc = -EINVAL;
		goto fail_close_fd;
	}
	rth->seq = time(NULL);
	return 0;

fail_close_fd:
    close(rth->fd);
fail:
    return rc;
}

static int rtnl_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen){
	int rc;

	do {
		rc = recvfrom(fd, buf, len, flags, src_addr, addrlen);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));

	if (rc < 0) {
        log_error("netlink receive error %s (%d)", strerror(errno), errno);
		return -errno;
	}
	if (rc == 0) {
		log_error("unexcepted EOF on netlink\n");
		return -ENODATA;
	}
	return rc;
}

static int rtnl_recv(int fd, char **answer, struct sockaddr *src_addr, socklen_t *addrlen){

	char *buf;
	int rc;
    static const int min_buf_len = 32768;
    int length = 0;

	rc = rtnl_recvfrom(fd, NULL, 0, MSG_PEEK | MSG_TRUNC, src_addr, addrlen);
    if (rc < 0){
        goto fail;
    }
    if(rc < min_buf_len){
        length = min_buf_len;
    }
	buf = malloc(length);
	if (!buf) {
        log_error("malloc error: not enough buffer");
        rc = -ENOMEM;
        goto fail;
	}
    rc = rtnl_recvfrom(fd, buf, length, 0, src_addr, addrlen);
	if (rc < 0) {
        goto fail_free_buf;
	}

	if (answer){
        *answer = buf;
    }else{
        free(buf);
    }
	return rc;
fail_free_buf:
    free(buf);
fail:
    return rc;
}

int rtnl_talk(struct rtnl_handle *rth, struct nlmsghdr *n, struct nlmsghdr **answer){
    assert(rth);
    assert(rth->fd >= 0);
    assert(n);

    struct sockaddr_nl nladdr = { .nl_family = AF_NETLINK };
    unsigned int this_seq = rth->seq++;
    int rc = 0;

    if(answer == NULL){
        n->nlmsg_flags |= NLM_F_ACK;
    }
    n->nlmsg_seq = this_seq;

    rc = sendto(rth->fd, n, n->nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr));

    if(rc < 0){
        log_error("Cannot talk to rtnetlink: %s", strerror(errno));
        rc = -errno;
        goto fail;
    }
    char *buf;
    socklen_t addr_len = sizeof(nladdr);
    rc = rtnl_recv(rth->fd, &buf, (struct sockaddr *)&nladdr, &addr_len);
    if(rc < 0){
        goto fail;
    }
    if(addr_len != sizeof(nladdr)){
        log_error("Address length mismatch, got %d, should be %d", addr_len, sizeof(nladdr));
        rc = -EINVAL;
        goto fail_freebuf;
    }
    int answer_len = rc;
    struct nlmsghdr *nlhdr = (struct nlmsghdr *)buf;
    while(1){
        if(!NLMSG_OK(nlhdr, answer_len)){
            log_error("Cannot parse netlink message");
            rc = -EINVAL;
            goto fail_freebuf;
        }

        if (
            nladdr.nl_pid != 0 ||
            nlhdr->nlmsg_pid != rth->local.nl_pid ||
            nlhdr->nlmsg_seq != this_seq
        ) {
            /* Don't forget to skip that message. */
            goto next_msg;
        }

        if (nlhdr->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlhdr);
            if(nlhdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))){
                log_error("Got error message with invalid length");
                rc = -EINVAL;
                goto fail_freebuf;
            }

            int error = err->error;
            if(error < 0){
                log_error("NETLINK error: %s", strerror(-error));
                rc = error;
            }else{
                rc = 0;
            }
            goto fail_freebuf;
        }

        if(answer){
            //XXX: answer may not be pointed to buf and the caller cannot free() it.
            *answer = nlhdr;
        }else{
            free(buf);
        }
        return 0;

    next_msg:
        if(nlhdr->nlmsg_flags & NLMSG_DONE){
            break;
        }
        nlhdr = NLMSG_NEXT(nlhdr, answer_len);
    }
    rc = -ENOMSG;
    log_error("Did not receive netlink answer to our request");
fail_freebuf:
    free(buf);
fail:
    return rc;

}
int rtnl_close(struct rtnl_handle *rth){
    assert(rth);
    assert(rth->fd >= 0);

    close(rth->fd);
    rth->fd = -1;

    return 0;
}
