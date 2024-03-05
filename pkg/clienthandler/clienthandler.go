package clienthandler

// #include <sys/socket.h>
// #include <sys/errno.h>
// static int readpeek(int fd) {
//      struct timeval timeout;
//      timeout.tv_sec = 10;
//      timeout.tv_usec = 0;
//      int rc;
//      rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(struct timeval));
//      if (rc < 0) {
//        return -errno;
//      }
//	    char buf;
//	    rc = recv(fd, &buf, 1, MSG_PEEK|MSG_TRUNC);
//      if (rc < 0) {
//            rc = -errno;
//            if (rc == -EWOULDBLOCK) {
//                  rc = -ETIMEDOUT;
//            }
//            return rc;
//	    }else{
//            return rc;
//		}
// }
// const int io_timeout = 10;
import "C"

import (
	"bytes"
	"context"
	"encoding/json"
	"sync"
	"time"

	criAnnotations "github.com/containerd/containerd/pkg/cri/annotations"
	crioAnnotations "github.com/containers/podman/v4/pkg/annotations"
	runtimespec "github.com/opencontainers/runtime-spec/specs-go"
	"golang.org/x/sys/unix"
	"k8s.io/klog/v2"
	kubeLabels "k8s.io/kubelet/pkg/types"

	"k8s.innull.com/trafficlimitd/pkg/cgrouputils"
	"k8s.innull.com/trafficlimitd/pkg/server"

	types "k8s.innull.com/trafficlimitd/pkg/types"
)

type ClientHandler struct {
	containerIDToCgroup sync.Map
	fetcher             types.TrafficLimitInfoFetcher
	limiter             types.TrafficLimiter
}

func New(fetcher types.TrafficLimitInfoFetcher, limiter types.TrafficLimiter) *ClientHandler {
	return &ClientHandler{
		fetcher: fetcher,
		limiter: limiter,
	}
}

func (h *ClientHandler) Handle(ctx context.Context, conn *server.ClientConn) {
	defer conn.Conn.Close()
	klog.Infof("Handling client from PID=%v", conn.PeerCredentials.Pid)
	f, err := conn.Conn.File()
	if err != nil {
		klog.Errorf("Failed to get file descriptor: %v while handling client from PID=%v", err, conn.PeerCredentials.Pid)
		return
	}
	defer f.Close()
	rc := C.readpeek(C.int(f.Fd()))
	if rc < 0 {
		klog.Errorf("Failed to readpeek: %v while handling client from PID=%v", unix.Errno(-rc), conn.PeerCredentials.Pid)
		return
	} else if rc == 0 {
		klog.Errorf("Failed to readpeek: EOF while handling client from PID=%v", conn.PeerCredentials.Pid)
		return
	}
	f.Close()
	length := rc
	buf := make([]byte, length)
	conn.Conn.Read(buf)
	klog.V(2).Infof("Read %v bytes while handling client from PID=%v: ", length, conn.PeerCredentials.Pid)

	args := bytes.Split(buf, []byte{0})
	if len(args) < 2 {
		klog.Errorf("Invalid request while handling client from PID=%v", conn.PeerCredentials.Pid)
		return
	}
	hookFrom := string(args[0])
	if hookFrom != "createContainer" && hookFrom != "poststop" {
		klog.Infof("Unknown hook %v called by client from PID=%v ", hookFrom, conn.PeerCredentials.Pid)
		_ = writeSuccess(conn)
		return
	}

	var state runtimespec.State
	err = json.Unmarshal(args[1], &state)
	if err != nil {
		klog.Errorf("Invalid request while handling client from PID=%v, Error=%v", conn.PeerCredentials.Pid, err)
		return
	}
	if state.ID == "" {
		klog.Errorf("Invalid request while handling client from PID=%v, ID is empty", conn.PeerCredentials.Pid)
		return
	}
	if hookFrom == "createContainer" {
		{
			cgrpID, err := cgrouputils.GetCgroupID(int(conn.PeerCredentials.Pid))
			if err != nil {
				klog.Errorf("Cannot get cgroupv2 id while handling client from PID=%v, CtrID=%v: Error=%v", conn.PeerCredentials.Pid, state.ID, err)
				goto FAIL
			}

			klog.V(2).Infof("Got cgroupv2 id for PID=%v, CtrID=%v: %v", conn.PeerCredentials.Pid, state.ID, cgrpID)

			_, exists := h.containerIDToCgroup.LoadOrStore(state.ID, cgrpID)
			if exists {
				klog.Errorf("Invalid request while handling client from PID=%v, dup container ID", conn.PeerCredentials.Pid)
				goto FAIL
			}

			containerType, ok := getKeyFromMap(state.Annotations, criAnnotations.ContainerType, crioAnnotations.ContainerType)
			if !ok {
				klog.Errorf("Invalid request while handling client from PID=%v, CtrID=%v: container type not found in annotations", conn.PeerCredentials.Pid, state.ID)
				goto FAIL_RELEASE
			}
			if containerType != criAnnotations.ContainerTypeContainer && containerType != crioAnnotations.ContainerTypeContainer {
				_ = writeSuccess(conn)
				goto FAIL_RELEASE
			}

			// KubernetesPodNamespaceLabel should be in labels, but SpecAddAnnotations in cri-o copies all labels to annotations
			// See: https://github.com/cri-o/cri-o/blob/v1.28.0/internal/factory/container/container.go#L261
			podNamespace, ok := getKeyFromMap(state.Annotations, criAnnotations.SandboxNamespace, kubeLabels.KubernetesPodNamespaceLabel)
			if !ok {
				klog.Errorf("Invalid request while handling client from PID=%v, CtrID=%v: pod namespace not found in annotations", conn.PeerCredentials.Pid, state.ID)
				goto FAIL_RELEASE
			}
			podName, ok := getKeyFromMap(state.Annotations, criAnnotations.SandboxName, kubeLabels.KubernetesPodNameLabel)
			if !ok {
				klog.Errorf("Invalid request while handling client from PID=%v, CtrID=%v: pod name not found in annotations", conn.PeerCredentials.Pid, state.ID)
				goto FAIL_RELEASE
			}
			podUUID, ok := getKeyFromMap(state.Annotations, criAnnotations.SandboxUID, kubeLabels.KubernetesPodUIDLabel)
			if !ok {
				klog.Errorf("Invalid request while handling client from PID=%v, CtrID=%v: pod uuid not found in annotations", conn.PeerCredentials.Pid, state.ID)
				goto FAIL_RELEASE
			}
			containerName, ok := getKeyFromMap(state.Annotations, criAnnotations.ContainerName, kubeLabels.KubernetesContainerNameLabel)
			if !ok {
				klog.Errorf("Invalid request while handling client from PID=%v, CtrID=%v: container name not found in annotations", conn.PeerCredentials.Pid, state.ID)
				goto FAIL_RELEASE
			}
			klog.V(2).Infof("Got pod info from container annotations for PID=%v, CtrID=%v: %v/%v/%v = %v", conn.PeerCredentials.Pid, state.ID, podNamespace, podName, containerName, podUUID)

			trafficLimitInfo, err := h.fetcher.GetTrafficLimitInfo(ctx, types.ContainerHandle{
				PodUUID:       podUUID,
				PodNamespace:  podNamespace,
				PodName:       podName,
				ContainerName: containerName,
			})
			if err != nil {
				klog.Errorf("Cannot get traffic limit info while handling client from PID=%v, CtrID=%v: Error=%v", conn.PeerCredentials.Pid, state.ID, err)
				goto FAIL_RELEASE
			}
			klog.V(2).Infof("Got traffic limit info for PID=%v, CtrID=%v (%v/%v/%v): %v", conn.PeerCredentials.Pid, state.ID, podNamespace, podName, containerName, trafficLimitInfo)

			err = h.limiter.LimitTraffic(cgrpID, trafficLimitInfo)
			if err != nil {
				klog.Errorf("Cannot set traffic limit for client from PID=%v, CtrID=%v: Error=%v", conn.PeerCredentials.Pid, state.ID, err)
				goto FAIL_RELEASE
			}
			klog.Infof("Set limit for client PID=%v, CtrID=%v", conn.PeerCredentials.Pid, state.ID)

			err = writeSuccess(conn)
			if err != nil {
				klog.Warningf("Write response failed for client PID=%v, CtrID=%v: %v", err)
			}
			return
		}

	FAIL_RELEASE:
		h.containerIDToCgroup.Delete(state.ID)
	FAIL:
		return

	} else if hookFrom == "poststop" {
		val, exists := h.containerIDToCgroup.Load(state.ID)
		if !exists {
			klog.Errorf("Invalid request while handling client from PID=%v, CtrID=%v: createContainer hook not called", conn.PeerCredentials.Pid, state.ID)
		} else {
			cgrpID := val.(types.CgroupID)
			_ = h.limiter.UnlimitTraffic(cgrpID)
			klog.Infof("Removed limit for client PID=%v, CtrID=%v", conn.PeerCredentials.Pid, state.ID)
			h.containerIDToCgroup.Delete(state.ID)
		}
		writeSuccess(conn)
		return
	}

}

func getKeyFromMap[Key comparable, Val any](m map[Key]Val, keys ...Key) (Val, bool) {
	for _, key := range keys {
		if val, ok := m[key]; ok {
			return val, true
		}
	}
	var zero Val
	return zero, false
}

func writeSuccess(conn *server.ClientConn) error {
	conn.Conn.SetWriteDeadline(time.Now().Add(time.Duration(C.io_timeout) * time.Second))
	successBuf := []byte("success\000")
	_, err := conn.Conn.Write(successBuf)
	return err
}
