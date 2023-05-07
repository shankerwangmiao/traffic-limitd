#ifndef TRAFFIC_LIMITD_PROTOCOL_H
#define TRAFFIC_LIMITD_PROTOCOL_H

#include<stdint.h>
#include<stddef.h>

struct rate_limit {
    uint64_t byte_rate;
    uint64_t packet_rate;
};

#define RATE_UNLIMITED UINT64_MAX

struct rate_limit_msg {
    size_t length;
    enum {
        RATE_LIMIT_REQ,
        RATE_LIMIT_FAIL,
        RATE_LIMIT_LOG,
        RATE_LIMIT_PROCEED,
    } type;
    char attr[];
};

struct rate_limit_req_attr {
    struct rate_limit limit;
    uint64_t flags;
};

enum {
    RATE_LIMIT_REQ_NOWAIT = 1 << 0,
};

struct rate_limit_fail_attr {
    enum {
        RATE_LIMIT_FAIL_UNKNOWN,
        RATE_LIMIT_FAIL_WILL_WAIT,
        RATE_LIMIT_FAIL_INTERNAL,
        RATE_LIMIT_FAIL_NORESOURCE,
    } reason;
};

#endif /* defined(TRAFFIC_LIMITD_PROTOCOL_H) */
