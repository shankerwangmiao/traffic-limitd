#!/bin/bash

set -e -o pipefail
shopt -s nullglob

if ! command -v bpftool &>/dev/null; then
	echo "bpftool needed" >&2
	exit 1
fi

if ! command -v tc &>/dev/null; then
	echo "tc needed" >&2
	exit 1
fi

ebpfo="cgroup_rate_limit.o"
bpf_fs="/sys/fs/bpf"
pin_prog="$bpf_fs/cgroup_rate_limit"
limit_map="$bpf_fs/rate_limit_map"

if [ ! -f "$ebpfo" ]; then
	echo "ebpf object cgroup_rate_limit.o missing" >&2
	exit 1;
fi

usage() {
	echo "Usage: $0 [load|unload] <intf>"
	echo "Load or unload cgroup traffic controller on interface <intf>"
	exit 1
}

if [ "x$1" != "xload" ] && [ "x$1" != "xunload" ]; then
	usage
fi

if [ -z "$2" ]; then
	usage
fi

intf="$2"

if [ ! -d "/sys/class/net/$intf" ]; then
	echo "Error: interface $intf not found"
	exit 1
fi

if [ "x$1" = "xunload" ]; then
	tc qdisc del dev "$intf" root || true
	tc qdisc del dev "$intf" clsact || true
	rm -f "$pin_prog" "$limit_map" || true
	exit 0
fi

tx_queues=("/sys/class/net/$intf/queues/tx-"*)
num_tx_queues=${#tx_queues[@]}

tc qdisc add dev "$intf" root handle 1: mq
for i in $(seq 1 "$num_tx_queues"); do
	tc qdisc add dev "$intf" parent 1:"$(printf '%x' $i)" \
		handle "$(printf '%x' "$((i+1))")": fq
done
tc qdisc add dev "$intf" clsact

rm -f "$pin_prog" "$limit_map" || true
bpftool prog load "$ebpfo" "$pin_prog" type classifier

tc filter add dev "$intf" egress bpf direct-action pinned "$pin_prog"
