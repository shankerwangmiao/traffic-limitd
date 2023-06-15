#ifndef TCBPF_UTIL_H
# define TCBPF_UTIL_H

#include <bpf_protocol.h>
#include <stdint.h>

int tc_setup_inferface(const char *ifnames);
int open_and_load_bpf_obj(int max_tasks);
int close_bpf_obj(void);
int cgroup_rate_limit_set(uint64_t cg_id, const struct rate_limit *limit);
int cgroup_rate_limit_unset(uint64_t cg_id);
int cgroup_rate_limit_check(uint64_t cg_id);

#endif /* defined(TCBPF_UTIL_H) */
