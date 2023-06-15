#ifndef TRAFFIC_LIMITD_BPF_PROTOCOL_H
#define TRAFFIC_LIMITD_BPF_PROTOCOL_H

#include <linux/types.h>

struct rate_limit {
    __u64 byte_rate;
    __u64 packet_rate;
};

#define RATE_UNLIMITED (~(__u64)0)

#endif /* defined(TRAFFIC_LIMITD_BPF_PROTOCOL_H) */
