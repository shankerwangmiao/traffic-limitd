package cgrouputils

import (
	"fmt"
	"path/filepath"
	"sync"

	"github.com/containerd/cgroups/v2/cgroup2"
	"github.com/vishvananda/netlink/nl"
	"golang.org/x/sys/unix"
	"k8s.io/klog/v2"

	"k8s.innull.com/trafficlimitd/pkg/types"
)

var (
	cgv2MountPoint string = ""
	checkOnce      sync.Once
	cgv2DirFd      int = -1
	dirfdOnce      sync.Once
)

const unifiedMountpoint = "/sys/fs/cgroup"

func GetCgroupV2MountPoint() (string, error) {
	checkOnce.Do(func() {
		var st unix.Statfs_t
		if err := unix.Statfs(unifiedMountpoint, &st); err != nil {
			return
		}
		if st.Type == unix.CGROUP2_SUPER_MAGIC {
			// Unified Hierarchy
			cgv2MountPoint = unifiedMountpoint
		} else if st.Type == unix.TMPFS_MAGIC {
			// Hybrid Hierarchy
			// cgroup v2 should be mounted on a /sys/fs/cgroup/unified
			pathToDetect := filepath.Join(unifiedMountpoint, "unified")
			err := unix.Statfs(pathToDetect, &st)
			if err == nil && st.Type == unix.CGROUP2_SUPER_MAGIC {
				cgv2MountPoint = pathToDetect
			} else {
				// We should detect another corner case, where cgroup v2 is mounted on a /sys/fs/cgroup/systemd
				// It is the behavior of systemd v232
				pathToDetect := filepath.Join(unifiedMountpoint, "systemd")
				err := unix.Statfs(pathToDetect, &st)
				if err == nil && st.Type == unix.CGROUP2_SUPER_MAGIC {
					cgv2MountPoint = pathToDetect
				} else {
					return
				}
			}
		}
		klog.Infof("Detected cgroup v2 mountpoint: %v", cgv2MountPoint)
	})
	if cgv2MountPoint == "" {
		return "", fmt.Errorf("cgroup v2 mountpoint not found")
	} else {
		return cgv2MountPoint, nil
	}
}

func GetCgroupID(pid int) (types.CgroupID, error) {
	dirfdOnce.Do(func() {
		cgPath, err := GetCgroupV2MountPoint()
		if err != nil {
			return
		}
		fd, err := unix.Open(cgPath, unix.O_PATH|unix.O_DIRECTORY, 0)
		if err != nil {
			return
		}
		cgv2DirFd = fd
	})
	if cgv2DirFd == -1 {
		return 0, fmt.Errorf("cgroup v2 dirfd not found")
	} else {
		cgrp, err := cgroup2.PidGroupPath(pid)
		if err != nil {
			return 0, err
		}
		cgrp = filepath.Join(".", cgrp)
		handle, _, err := unix.NameToHandleAt(cgv2DirFd, cgrp, 0)
		if err != nil {
			return 0, fmt.Errorf("NameToHandleAt failed: %w", err)
		}
		b := handle.Bytes()[:8]
		cgID := types.CgroupID(nl.NativeEndian().Uint64(b))

		return cgID, nil
	}
}
