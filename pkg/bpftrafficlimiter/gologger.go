package bpftrafficlimiter

/*
#cgo CFLAGS: -I${SRCDIR}/libebpf-traffic-limiter/include
#cgo LDFLAGS: -L${SRCDIR}/objs -lebpf-traffic-limiter
#cgo pkg-config: libbpf

#include <log.h>
*/
import "C"

import (
	"k8s.io/klog/v2"
)

//export doLog
func doLog(level C.int, str1 *C.char, str2 *C.char) {
	log := C.GoString(str1) + C.GoString(str2)
	if level == C.LOG_TRACE {
		klog.V(3).InfoDepth(1, log)
	} else if level == C.LOG_DEBUG {
		klog.V(2).InfoDepth(1, log)
	} else if level == C.LOG_INFO {
		klog.InfoDepth(1, log)
	} else if level == C.LOG_WARN {
		klog.WarningDepth(1, log)
	} else if level == C.LOG_ERROR {
		klog.ErrorDepth(1, log)
	} else if level == C.LOG_FATAL {
		klog.ErrorDepth(1, log)
	} else {
		klog.InfoDepth(1, log)
	}
}
