#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <errno.h>
#include <log.h>
#include <cgroup_util.h>
#include <assert.h>

static int cgroupv2_root_fd = -1;

#define F_TYPE_EQUAL(a, b) (a == (typeof(a)) b)

int cg_find_unified(void) {
    struct statfs fs;
    const char *cgv2_path = NULL;

    /* Checks if we support the unified hierarchy. Returns an
        * error when the cgroup hierarchies aren't mounted yet or we
        * have any other trouble determining if the unified hierarchy
        * is supported. */

    if(cgroupv2_root_fd != -1){
        return 0;
    }

    int rc = 0;
    rc = statfs("/sys/fs/cgroup/", &fs);
    if (rc < 0){
        log_error("statfs(\"/sys/fs/cgroup/\") failed: %s", strerror(errno));
        return -errno;
    }
    if (F_TYPE_EQUAL(fs.f_type, CGROUP2_SUPER_MAGIC)) {
        log_debug("Found cgroup2 on /sys/fs/cgroup/, full unified hierarchy");
        cgv2_path = "/sys/fs/cgroup/";
    } else if (F_TYPE_EQUAL(fs.f_type, TMPFS_MAGIC)) {
        rc = statfs("/sys/fs/cgroup/unified/", &fs);
        if ( rc == 0 &&
            F_TYPE_EQUAL(fs.f_type, CGROUP2_SUPER_MAGIC)
        ) {
            log_debug("Found cgroup2 on /sys/fs/cgroup/unified, unified hierarchy for systemd controller");
            cgv2_path = "/sys/fs/cgroup/unified/";
        } else {
            rc = statfs("/sys/fs/cgroup/systemd/", &fs);
            if (rc < 0) {
                if (errno == ENOENT) {
                    /* Some other software may have set up /sys/fs/cgroup in a configuration we do not recognize. */
                    log_debug("Unsupported cgroupsv1 setup detected: name=systemd hierarchy not found");
                    return -ENOMEDIUM;
                }
                log_error("statfs(\"/sys/fs/cgroup/systemd\" failed: %s", strerror(errno));
                return -errno;
            }

            if (F_TYPE_EQUAL(fs.f_type, CGROUP2_SUPER_MAGIC)) {
                log_debug("Found cgroup2 on /sys/fs/cgroup/systemd, unified hierarchy for systemd controller (v232 variant)");
                cgv2_path = "/sys/fs/cgroup/systemd/";
            } else if (F_TYPE_EQUAL(fs.f_type, CGROUP_SUPER_MAGIC)) {
                log_debug("Found cgroup on /sys/fs/cgroup/systemd, legacy hierarchy");
                return -ENOMEDIUM;
            } else {
                log_debug("Unexpected filesystem type %llx mounted on /sys/fs/cgroup/systemd, assuming legacy hierarchy",
                            (unsigned long long) fs.f_type);
                return -ENOMEDIUM;
            }
        }
    } else if (F_TYPE_EQUAL(fs.f_type, SYSFS_MAGIC)) {
        log_error("No filesystem is currently mounted on /sys/fs/cgroup.");
        return -ENOMEDIUM;
    } else{
        log_error("Unknown filesystem type %llx mounted on /sys/fs/cgroup.", (unsigned long long)fs.f_type);
        return -ENOMEDIUM;
    }
    rc = open(cgv2_path, O_PATH|O_DIRECTORY);
    if(rc < 0){
        log_error("Failed to obtain a handle of %s: %s", cgv2_path, strerror(errno));
        return -errno;
    }
    cgroupv2_root_fd = rc;
    return 0;
}

union cg_file_handle{
    struct file_handle file_handle;
    uint8_t space[offsetof(struct file_handle, f_handle) + sizeof(uint64_t)];
} cg_file_handle;

#define CG_FILE_HANDLE_INIT { .file_handle.handle_bytes = sizeof(uint64_t) }
#define CG_FILE_HANDLE_CGROUPID(fh) (*(uint64_t*) (fh).file_handle.f_handle)

int cg_path_get_cgroupid(const char *path, uint64_t *ret) {
    union cg_file_handle fh = CG_FILE_HANDLE_INIT;
    int mnt_id = -1;

    assert(path);
    assert(ret);
    if(cgroupv2_root_fd < 0){
        return -ENOMEDIUM;
    }

    size_t path_len = strlen(path);
    char formatted_path[path_len + 1 + 2 + 10];
    snprintf(formatted_path, sizeof(formatted_path), "./%s", path);

    int rc = name_to_handle_at(cgroupv2_root_fd, formatted_path, &fh.file_handle, &mnt_id, 0);
    if (rc < 0){
        log_error("Failed to obtain a handle of cgroup %s: %s", path, strerror(errno));
        return -errno;
    }
    *ret = CG_FILE_HANDLE_CGROUPID(fh);
    return 0;
}
