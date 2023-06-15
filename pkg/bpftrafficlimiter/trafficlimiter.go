package bpftrafficlimiter

/*
#cgo CFLAGS: -I${SRCDIR}/libebpf-traffic-limiter/include
#cgo LDFLAGS: -L${SRCDIR}/libebpf-traffic-limiter/objs -lebpf-traffic-limiter
#cgo pkg-config: libbpf

#define _GNU_SOURCE
#include <log.h>
#include <stdio.h>
#include <stdlib.h>
#include <tcbpf_util.h>
#include <bpf_protocol.h>
extern void doLog(int level, char *log1, char *log2);

static void klog_callback(log_Event *ev);
void init_log(){
	log_set_quiet(true);
	log_add_callback(klog_callback, NULL, 0);
}

static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static void klog_callback(log_Event *ev) {
	char buf[16];

	buf[strftime(buf, sizeof(buf), "%H:%M:%S", ev->time)] = '\0';

	size_t len = 0;
	char *str1, *str2;
	int rc = asprintf(&str1, "%s %-5s %s(%s:%d): ",
		buf, level_strings[ev->level], ev->func, ev->file, ev->line);
	if(rc < 0){
		goto error;
	}
	rc = vasprintf(&str2, ev->fmt, ev->ap);
	if(rc < 0){
		goto error_free_str1;
	}
	doLog(ev->level, str1, str2);
	free(str2);
error_free_str1:
	free(str1);
error:
	return;
}
const __u64 rate_unlimited = RATE_UNLIMITED;
*/
import "C"

import (
	"errors"
	"strings"
	"sync"
	"unsafe"

	"golang.org/x/sys/unix"
	"k8s.io/klog/v2"

	"k8s.innull.com/trafficlimitd/pkg/types"
)

type requestWhat int

const (
	undefined requestWhat = iota
	set
	unset
)

type request struct {
	what      requestWhat
	cgroupId  types.CgroupID
	limitInfo types.TrafficLimitInfo
	result    error
	done      chan struct{}
}

type limiter struct {
	loaded sync.Once
	reqs   chan *request
	closed chan struct{}
}

var Limiter limiter

func (l *limiter) doService() {
	for {
		select {
		case req := <-l.reqs:
			switch req.what {
			case set:
				if req.limitInfo.BytesPerSecond != 0 || req.limitInfo.PacketsPerSecond != 0 {
					limit := C.struct_rate_limit{
						byte_rate:   C.rate_unlimited,
						packet_rate: C.rate_unlimited,
					}
					if req.limitInfo.BytesPerSecond != 0 {
						limit.byte_rate = C.__u64(req.limitInfo.BytesPerSecond)
					}
					if req.limitInfo.PacketsPerSecond != 0 {
						limit.packet_rate = C.__u64(req.limitInfo.PacketsPerSecond)
					}
					rc := C.cgroup_rate_limit_set(C.uint64_t(req.cgroupId), &limit)
					if rc < 0 {
						req.result = unix.Errno(-rc)
					} else {
						req.result = nil
					}
				} else {
					req.result = nil
				}
			case unset:
				rc := C.cgroup_rate_limit_unset(C.uint64_t(req.cgroupId))
				if rc < 0 {
					req.result = unix.Errno(-rc)
				} else {
					req.result = nil
				}
			default:
				req.result = errors.New("Invalid Request")
			}
			close(req.done)
		case <-l.closed:
			break
		}
	}
}

func (l *limiter) LoadBPF(maxTasks int) error {
	notLoaded := false
	l.loaded.Do(func() {
		notLoaded = true
	})
	if !notLoaded {
		return errors.New("Loaded Again")
	}
	rc := C.open_and_load_bpf_obj(C.int(maxTasks))
	if rc < 0 {
		return unix.Errno(-rc)
	}
	l.reqs = make(chan *request)
	l.closed = make(chan struct{})
	go l.doService()
	klog.V(2).Info("BPF Loaded")
	return nil
}

func (l *limiter) Close() {
	_ = C.close_bpf_obj()
	close(l.closed)
}

func (l *limiter) ArmOnInterfaces(ifName []string) error {
	c_ifNames := C.CString(strings.Join(ifName, ","))
	defer C.free(unsafe.Pointer(c_ifNames))
	rc := C.tc_setup_inferface(c_ifNames)
	if rc < 0 {
		return unix.Errno(-rc)
	}
	klog.V(2).Info("BPF setup on interfaces:", ifName)
	return nil
}
func (l *limiter) subReq(req *request) error {
	req.result = nil
	req.done = make(chan struct{})
	select {
	case l.reqs <- req:
	case <-l.closed:
		return errors.New("Closed")
	}
	<-req.done
	return req.result
}
func (l *limiter) LimitTraffic(cgroupId types.CgroupID, trafficLimitInfo types.TrafficLimitInfo) error {
	req := &request{
		what:      set,
		cgroupId:  cgroupId,
		limitInfo: trafficLimitInfo,
	}
	return l.subReq(req)
}
func (l *limiter) UnlimitTraffic(cgroupId types.CgroupID) error {
	req := &request{
		what:     unset,
		cgroupId: cgroupId,
	}
	return l.subReq(req)
}

func init() {
	C.init_log()
}
