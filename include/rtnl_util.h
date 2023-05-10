#ifndef _RTNL_UTIL_H
# define _RTNL_UTIL_H

#include <linux/netlink.h>

struct rtnl_handle{
    int    fd;
    __u32  seq;
    struct sockaddr_nl	local;
};

int rtnl_open(struct rtnl_handle *rth);
int rtnl_talk(struct rtnl_handle *rth, struct nlmsghdr *n, struct nlmsghdr **answer);
int rtnl_close(struct rtnl_handle *rth);

#endif /* defined(_RTNL_UTIL_H_) */
