#include <net/if.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>


#include <log.h>
#include <tcbpf_util.h>
#include <rtnl_util.h>

#define TCA_BUF_MAX	(64*1024)

enum qidsc_kind{
    QDISC_KIND_MQ,
    QDISC_KIND_FQ,
    QDISC_KIND_NOQUEUE,
    QDISC_KIND_CLSACT,
    QDISC_KIND_OTHER,
};


#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))


static inline __u32 rta_getattr_u32(const struct rtattr *rta)
{
	return *(__u32 *)RTA_DATA(rta);
}

static inline const char *rta_getattr_str(const struct rtattr *rta)
{
	return (const char *)RTA_DATA(rta);
}

int addattr_l(struct nlmsghdr *n, size_t maxlen, int type, const void *data, int alen){
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen) {
        log_error("addattr_l: message buffer overflow");
        abort();
    }
    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = len;
    if (alen)
        memcpy(RTA_DATA(rta), data, alen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
    return 0;
}

struct iface_attr{
    int num_tx_queues;
    enum qidsc_kind qdisc_kind;
};

static int get_iface_props(struct rtnl_handle *rth, unsigned int ifindex, struct iface_attr *result){

    assert(rth);
    assert(result);

    struct {
        struct nlmsghdr	n;
        struct ifinfomsg i;
        char buf[1024];
    } req = {
        .n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
        .n.nlmsg_flags = NLM_F_REQUEST,
        .n.nlmsg_type = RTM_GETLINK,
        .i.ifi_family = AF_UNSPEC,
        .i.ifi_index = ifindex,
    };

    struct iface_attr found_result = {0};

    int rc = 0;
    struct nlmsghdr *answer = NULL;
    rc = rtnl_talk(rth, &req.n, &answer);
    if(rc < 0){
        log_error("get_iface_props: rtnl_talk failed: %s", strerror(-rc));
        goto fail;
    }
    struct ifinfomsg *if_info = NLMSG_DATA(answer);
    if (answer->nlmsg_type != RTM_NEWLINK && answer->nlmsg_type != RTM_DELLINK){
        log_error("get_iface_props: unexpected answer type %d", answer->nlmsg_type);
        rc = -EINVAL;
        goto fail_free_ans;
    }
    struct rtattr *rt_attr = IFLA_RTA(if_info);
    int attr_len = answer->nlmsg_len - NLMSG_LENGTH(sizeof(*if_info));
    int found = 0;
    while (RTA_OK(rt_attr, attr_len)) {
        unsigned short type = rt_attr->rta_type & ~NLA_F_NESTED;
        if(type == IFLA_NUM_TX_QUEUES){
            found_result.num_tx_queues = rta_getattr_u32(rt_attr);
            found |= 1 << 0;
        }else if(type == IFLA_QDISC){
            const char *qdisc_name = rta_getattr_str(rt_attr);
            if(strcmp(qdisc_name, "fq") == 0){
                found_result.qdisc_kind = QDISC_KIND_FQ;
            }else if(strcmp(qdisc_name, "mq") == 0){
                found_result.qdisc_kind = QDISC_KIND_MQ;
            }else if(strcmp(qdisc_name, "noqueue") == 0){
                found_result.qdisc_kind = QDISC_KIND_NOQUEUE;
            }else if(strcmp(qdisc_name, "clsact") == 0){
                found_result.qdisc_kind = QDISC_KIND_CLSACT;
            }else{
                found_result.qdisc_kind = QDISC_KIND_OTHER;
            }
            found |= 1 << 1;
        }
        if((found & 3) == 3){
            break;
        }
        rt_attr = RTA_NEXT(rt_attr, attr_len);
    }
    if((found & 3) != 3){
        log_error("get_iface_props: IFLA_NUM_TX_QUEUES and IFLA_QDISC not found");
        rc = -EINVAL;
        goto fail_free_ans;
    }
    rc = 0;
    *result = found_result;

fail_free_ans:
    free(answer);
fail:
    return rc;
}

static inline const char *qdisc_name(enum qidsc_kind q){
    const char *s = NULL;
    switch(q){
        case QDISC_KIND_FQ:
            s = "fq";
            break;
        case QDISC_KIND_MQ:
            s = "mq";
            break;
        case QDISC_KIND_NOQUEUE:
            s = "noqueue";
            break;
        case QDISC_KIND_CLSACT:
            s = "clsact";
            break;
        default:
            s = NULL;
            break;
    }
    return s;
}

static int tc_replace_qdisc(struct rtnl_handle *rth, unsigned int ifindex, __u32 parent, __u32 handle, enum qidsc_kind kind){
    struct {
        struct nlmsghdr	n;
        struct tcmsg    t;
        char            buf[TCA_BUF_MAX];
    } req = {
        .n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
        .n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE,
        .n.nlmsg_type = RTM_NEWQDISC,
        .t.tcm_family = AF_UNSPEC,
        .t.tcm_ifindex = ifindex,
        .t.tcm_parent = parent,
        .t.tcm_handle = handle,
    };
    const char *kind_name = NULL;

    kind_name = qdisc_name(kind);
    if(kind_name == NULL){
            log_error("Invalid qdisc kind %d", kind);
            return -EINVAL;
    }
    addattr_l(&req.n, sizeof(req), TCA_KIND, kind_name, strlen(kind_name)+1);
    int rc = rtnl_talk(rth, &req.n, NULL);
    if(rc < 0){
        log_error("Cannot replace qdisc on ifindex %u: %s", ifindex, strerror(-rc));
        return rc;
    }
    rc = 0;
    return rc;
}

static int tc_del_qdisc(struct rtnl_handle *rth, unsigned int ifindex, __u32 parent, __u32 handle){
    struct {
        struct nlmsghdr	n;
        struct tcmsg    t;
        char            buf[TCA_BUF_MAX];
    } req = {
        .n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
        .n.nlmsg_flags = 0,
        .n.nlmsg_type = RTM_DELQDISC,
        .t.tcm_family = AF_UNSPEC,
        .t.tcm_ifindex = ifindex,
        .t.tcm_parent = parent,
        .t.tcm_handle = handle,
    };

    int rc = rtnl_talk(rth, &req.n, NULL);
    if(rc < 0){
        log_error("Cannot replace qdisc on ifindex %u: %s", ifindex, strerror(-rc));
        return rc;
    }
    rc = 0;
    return rc;
}

static int tc_setup_one_inferface(struct rtnl_handle *rth, const char *ifname){
    unsigned int ifindex = 0;
    ifindex = if_nametoindex(ifname);
    if(ifindex == 0){
        if(errno == ENODEV){
            log_error("if_nametoindex(%s) failed: No such interface.", ifname);
        }else{
            log_error("if_nametoindex(%s) failed: %s", ifname, strerror(errno));
        }
        return -errno;
    }
    log_trace("setting up queues on interface %s (ifindex %u)", ifname, ifindex);

    int rc = 0;
    struct iface_attr iface_attr = {0};
    rc = get_iface_props(rth, ifindex, &iface_attr);
    if(rc < 0){
        log_error("get_iface_props(%s) failed: %s", ifname, strerror(-rc));
        return rc;
    }
    log_trace("iface %s: num_tx_queues = %d, qdisc_kind = %s", ifname, iface_attr.num_tx_queues, qdisc_name(iface_attr.qdisc_kind));
    if(iface_attr.num_tx_queues == 0){
        log_error("iface %s: num_tx_queues = 0", ifname);
        return -EINVAL;
    }
    if(iface_attr.num_tx_queues == 1){
        log_info("iface %s: single queue", ifname);
        // If the root qdisc is already fq, do nothing.
        if(iface_attr.qdisc_kind != QDISC_KIND_FQ){
            log_trace("tc qdisc replace dev %s root fq", ifname);
            rc = tc_replace_qdisc(rth, ifindex, TC_H_ROOT, TC_H_UNSPEC, QDISC_KIND_FQ);
            if(rc < 0){
                log_error("tc qdisc replace dev %s root fq failed: %s", ifname, strerror(-rc));
                return rc;
            }
        }
    }else{
        log_info("iface %s: multi queue", ifname);
        //First try to attach mq to handle 1:,
        //if unsuccessful, it might be because 1: has already been used by other qdisc type,
        //so try another handle 2:.
        int root_handle;
        for(root_handle = 1; root_handle <= 2; root_handle++){
            log_trace("tc qdisc replace dev %s root handle %x: mq", ifname, root_handle);
            rc = tc_replace_qdisc(rth, ifindex, TC_H_ROOT, TC_H_MAKE(root_handle << 16, 0), QDISC_KIND_MQ);
            if(rc < 0){
                log_error("tc qdisc replace dev %s root handle %x: mq failed: %s", ifname, root_handle, strerror(-rc));
                if(root_handle == 1){
                    log_info("will try another handle");
                }
            }else{
                break;
            }
        }
        if(root_handle > 2){
            return rc;
        }
        //for each tx queue, replace the sub qdisc to fq.
        for(int i = 1; i <= iface_attr.num_tx_queues; i++){
            log_trace("tc qdisc replace dev %s parent %x:%x handle %x: fq", ifname, root_handle, i, i + root_handle);
            rc = tc_replace_qdisc(rth, ifindex, TC_H_MAKE(root_handle << 16, i), TC_H_MAKE((i + root_handle) << 16, 0), QDISC_KIND_FQ);
            if(rc < 0){
                log_error("tc qdisc replace dev %s parent %x:%x handle %x: fq", ifname, root_handle, i, i + root_handle, strerror(-rc));
                return rc;
            }
        }
    }
    int nr_try = 0;
    do{
        log_trace("tc qdisc replace dev %s clsact", ifname);
        rc = tc_replace_qdisc(rth, ifindex, TC_H_CLSACT, TC_H_MAKE(TC_H_CLSACT, 0), QDISC_KIND_CLSACT);
        if(rc < 0){
            log_error("tc qdisc replace dev %s clsact failed: %s", ifname, strerror(-rc));
            if(nr_try > 0){
                return rc;
            }
            log_info("trying del first");
            log_trace("tc qdisc del dev %s clsact", ifname);
            rc = tc_del_qdisc(rth, ifindex, TC_H_CLSACT, TC_H_MAKE(TC_H_CLSACT, 0));
            if(rc < 0){
                log_error("tc qdisc del dev %s clsact failed: %s", ifname, strerror(-rc));
                return rc;
            }
            nr_try++;
        }
    }while(rc < 0);

    log_info("tc setup for %s done", ifname);
    return 0;
}

int tc_setup_inferface(const char *ifnames){
    assert(ifnames);

    size_t str_len = strlen(ifnames);

    if(str_len == 0){
        log_error("ifnames is empty");
        return -EINVAL;
    }

    struct rtnl_handle rth;
    int rc = 0;

    rc = rtnl_open(&rth);
    if(rc < 0){
        log_error("rtnl_open() failed: %s", strerror(-rc));
        goto fail;
    }

    {
        char buf[str_len + 1];
        strncpy(buf, ifnames, str_len + 1);

        char *savetokptr = NULL;
        for(char *token = strtok_r(buf, ",", &savetokptr); token; token = strtok_r(NULL, ",", &savetokptr)){
            rc = tc_setup_one_inferface(&rth, token);
            if(rc < 0){
                log_error("tc_setup_one_inferface(%s) failed: %s", token, strerror(-rc));
                goto fail_close_rtnl;
            }
        }
        rc = 0;
    }

fail_close_rtnl:
    rtnl_close(&rth);
fail:
    return rc;
}
