#define _DEFAULT_SOURCE
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/pkt_cls.h>
#include <netinet/if_ether.h>
#include <errno.h>
#include <linux/ipv6.h>


#ifndef __maybe_unused
# define __maybe_unused         __attribute__((__unused__))
#endif

#undef __always_inline          /* stddef.h defines its own */
#define __always_inline         inline __attribute__((always_inline))

// helper macros for branch prediction
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Helper macro to print out debug messages */
#define bpf_printk(fmt, ...)                            \
({                                                      \
        char ____fmt[] = fmt;                           \
        bpf_trace_printk(____fmt, sizeof(____fmt),      \
                         ##__VA_ARGS__);                \
})

#ifndef memset
# define memset(dest, chr, n)   __builtin_memset((dest), (chr), (n))
#endif

#ifndef memcpy
# define memcpy(dest, src, n)   __builtin_memcpy((dest), (src), (n))
#endif

#ifndef memmove
# define memmove(dest, src, n)  __builtin_memmove((dest), (src), (n))
#endif

#define NS_PER_SEC 1000000000ull

#define MAP_MAX_LEN 1024
#define DROP_HORIZON (2 * NS_PER_SEC)

typedef unsigned long long time_ns_t, cgroup_id_t;

struct rate_limit_priv {
	time_ns_t next_avail_ts;
};

struct rate_limit {
	uint64_t byte_rate;
	uint64_t packet_rate;
};


struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, cgroup_id_t);
	__type(value, struct rate_limit);
	__uint(max_entries, MAP_MAX_LEN);
	__uint(map_flags, BPF_F_RDONLY_PROG);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} rate_limit_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__type(key, cgroup_id_t);
	__type(value, struct rate_limit_priv);
	__uint(max_entries, MAP_MAX_LEN);
	__uint(pinning, LIBBPF_PIN_NONE);
} rate_limit_priv_map SEC(".maps");



SEC("tc/cgroup_rate_limit")
long cgroup_rate_limit(struct __sk_buff *skb){
	const size_t this_pkt_len = skb->len;
	const unsigned long long cgid = bpf_skb_cgroup_id(skb);

	const struct rate_limit * const rlcf = bpf_map_lookup_elem(&rate_limit_map, &cgid);
	if (!rlcf){
		return TC_ACT_OK;
	}
	const time_ns_t delay_ns_byte = rlcf->byte_rate ? (this_pkt_len * NS_PER_SEC + rlcf->byte_rate / 2) / rlcf->byte_rate : 0;
	const time_ns_t delay_ns_pkt  = rlcf->packet_rate ? (NS_PER_SEC + rlcf->packet_rate / 2) / rlcf->packet_rate : 0;
	const time_ns_t delay_ns = delay_ns_pkt > delay_ns_byte ? delay_ns_pkt : delay_ns_byte;

	struct rate_limit_priv volatile *priv = bpf_map_lookup_elem(&rate_limit_priv_map, &cgid);
	const unsigned long long now = bpf_ktime_get_ns();
	if(priv){
		const time_ns_t next_avail_ts = priv->next_avail_ts;
		if(next_avail_ts < now){
			//racy, not an issue, same value expected
			priv->next_avail_ts = now + delay_ns;
			skb->tstamp = now;
			return TC_ACT_OK;
		}else if(next_avail_ts > now + DROP_HORIZON){
			return TC_ACT_SHOT;
		}else{
			skb->tstamp = next_avail_ts;
			__sync_fetch_and_add(&priv->next_avail_ts, delay_ns);
		}
	}else{
		struct rate_limit_priv new_priv = {.next_avail_ts = now + delay_ns};
		bpf_map_update_elem(&rate_limit_priv_map, &cgid, &new_priv, BPF_ANY);
		skb->tstamp = now;
	}
	return TC_ACT_OK;
}


char __license[] SEC("license") = "MIT";
