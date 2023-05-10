#ifndef TCBPF_UTIL_H
# define TCBPF_UTIL_H

int tc_setup_inferface(const char *ifnames);
int open_and_load_bpf_obj(void);
int close_bpf_obj(void);

#endif /* defined(TCBPF_UTIL_H) */
