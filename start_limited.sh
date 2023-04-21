#!/bin/bash
set -e -o pipefail
myuid=$(id --user)
mygid=$(id --group)
sysrun="systemd-run"
if [ "$myuid" -ne 0 ]; then
	if ! command -v sudo &>/dev/null; then
		echo "You are not root and sudo is not installed" >&2
		exit 1;
	fi
	sysrun="sudo $sysrun"
fi

if ! command -v bpftool &>/dev/null; then
	echo "bpftool is needed" >&2
	exit 1;
fi

if ! command -v findmnt &>/dev/null; then
	echo "findmnt is needed" >&2
	exit 1;
fi

if ! command -v systemd-run &>/dev/null; then
	echo "systemd-run is needed" >&2
	exit 1;
fi

if ! command -v nsenter &>/dev/null; then
	echo "nsenter is needed" >&2
	exit 1;
fi

if ! cgrp2_root=$(findmnt --types "cgroup2" --output "TARGET" --first-only --noheadings); then
	echo "Error: cannot find cgroupv2 mount point" >&2
	if grep "^0::" /proc/1/cgroup >/dev/null; then
		echo "Cgroupv2 is available, but cannot find its mount point" >&2
		echo "Are you in chroot?" >&2
	else
		echo "Cgroupv2 is not available, too old kernel or too old systemd" >&2
	fi
	exit 1;
fi

usage() {
	echo "Usage: $0 [ --bps NUM ] [ --pps NUM ] [ -- ] COMMAND [ ARG .. ]"
	echo "Run command COMMAND, limit its pps and bps"
	echo "NUM can be suffixed with k, m, g , K, M, G"
	exit 1;
}

pps=0
bps=0

confmap="/sys/fs/bpf/rate_limit_map"

if [ ! -e "$confmap" ]; then
	echo "Cgroup traffic controller not loaded" >&2
	exit 1;
fi

while :; do
	case $1 in
		--help | --usage)
			usage
			break
			;;
		--)
			shift
			break
			;;
		--bps)
			shift
			bps=$1
			if [ -z "$bps" ]; then
				echo "--bps: missing argument" >&2
				usage >&2
			fi
			if ! bps=$(numfmt --from si --to-unit 8 -- "$(echo "$1" | tr '[:lower:]' '[:upper:]')"); then
				echo "Maiformed number: $1" >&2
				exit 1
			fi
			shift
			;;
		--pps)
			shift
			pps=$1
			if [ -z "$pps" ]; then
				echo "--pps: missing argument" >&2
				usage >&2
			fi
			if ! pps=$(numfmt --from si --to-unit 1 -- "$(echo "$1" | tr '[:lower:]' '[:upper:]')"); then
				echo "Maiformed number: $1" >&2
				exit 1
			fi
			shift
			;;
		*)
			break
			;;
	esac
done

bps=$(printf "%016x\n" "$bps" | fold -w 2 | tac | tr '\n' ' ')
pps=$(printf "%016x\n" "$pps" | fold -w 2 | tac | tr '\n' ' ')

script="id=\$(printf \"%016x\\n\" \"\$(stat --printf=\"%i\" \"$cgrp2_root/\$(grep \"^0::\" /proc/\$\$/cgroup | sed \"s/^0:://\")\")\" | fold -w 2 | tac | tr \"\\n\" \" \");"
start_script="$script
bpftool map update pinned \"$confmap\" key hex \$id value hex $bps $pps ; 
"
stop_script="
bpftool map delete pinned \"$confmap\" key hex \$id || true;
"
#
#sleep 10000d &
#nspid=$!
#disown
#
#exec $sysrun --wait --send-sighup \
#	--service-type=oneshot --collect --pipe \
#	-p ExecStartPre="+:nsenter --all --target $nspid -- /bin/bash -e -o pipefail -c '$start_script'" \
#	-p ExecStopPost="+:nsenter --all --target $nspid -- /bin/bash -e -o pipefail -c '$stop_script kill -9 $nspid'" \
#       	nsenter --all --target "$nspid" --setuid "$myuid" --wd="$PWD" --setgid "$mygid" -- "$@"
# 

exec $sysrun --collect --scope bash --noprofile --norc -e -o pipefail -c "
	trap \"\" INT QUIT TERM HUP ABRT STOP
	$start_script
	(trap \"-\" INT QUIT TERM HUP ABRT STOP; exec \"\$0\" \"\$@\") && ret=\$? || ret=\$?;
	$stop_script
	exit \$ret;
" nsenter --no-fork --setuid "$myuid" --setgid "$mygid" -- env --unset="SUDO_USER" --unset="SUDO_COMMAND" --unset="SUDO_UID" --unset="SUDO_GID" -- "HOME=$HOME" "PATH=$PATH" "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" "USER=$USER" "MAIL=$MAIL" "$@" 
