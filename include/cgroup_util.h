#ifndef CGROUP_UTIL_H
# define CGROUP_UTIL_H

int cg_find_unified(void);
int cg_path_get_cgroupid(const char *path, uint64_t *ret);

#endif /* defined(CGROUP_UTIL_H) */
