#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PADL only: confirm that a kernel carries every PADL Intel network driver
# patch listed in intel-manifest.tsv. Read-only: it never writes to the tree,
# the modules or the host.
#
#   check-intel-patches.sh tree [GIT_DIR_OR_WORKTREE [REV]]
#	Every "patch" line's subject must be a commit reachable from REV
#	(default HEAD). A commit whose patch-id differs from the manifest's is
#	reported (a backport or a different version of the patch), not failed.
#
#   check-intel-patches.sh host
#	On a running host: every "marker" line is tested against the loaded
#	module (/sys/module/<mod>) through its file (modinfo -n, which must
#	match /sys/module/<mod>/srcversion), /proc/kallsyms and, if mounted,
#	the dynamic debug control file.
#
#   check-intel-patches.sh modules igc=FILE [igb=FILE] [ice=FILE] ...
#	The markers against module files (or objects) not loaded anywhere.
#	A .ko.zst, .ko.xz or .ko.gz is decompressed to a pipe.
#
# Options (before the mode): -m MANIFEST (default: intel-manifest.tsv next
# to this script), -s SINCE (tree mode: git log --since, default 2026-01-01).
#
# Exit status: 0 when every check passes, 1 when any fails, 2 on usage.

set -u

here=$(cd "$(dirname "$0")" && pwd)
manifest=$here/intel-manifest.tsv
since=2026-01-01

while getopts m:s: opt; do
	case $opt in
	m) manifest=$OPTARG ;;
	s) since=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

mode=${1:-}
[ -n "$mode" ] && shift
[ -r "$manifest" ] || { echo "no manifest: $manifest" >&2; exit 2; }

fail=0
pass() { printf 'PASS  %s\n' "$*"; }
bad() { printf 'FAIL  %s\n' "$*"; fail=1; }
note() { printf 'NOTE  %s\n' "$*"; }

tree_check() {
	local dir=${1:-.} rev=${2:-HEAD} log h subj pid want n=0 miss=0
	local paths=(drivers/net/ethernet/intel net/xdp include/net)

	git -C "$dir" rev-parse --verify -q "$rev^{commit}" >/dev/null ||
		{ echo "not a revision in $dir: $rev" >&2; exit 2; }
	echo "tree $(git -C "$dir" rev-parse --short=13 "$rev") ($rev) in $dir"
	log=$(git -C "$dir" log --no-merges --since="$since" \
		--format='%H%x09%s%x09%ae' "$rev" -- "${paths[@]}")

	while IFS=$'\t' read -r type drv up want subj; do
		[ "$type" = patch ] || continue
		n=$((n + 1))
		h=$(printf '%s\n' "$log" | awk -F'\t' -v s="$subj" \
			'$2 == s { print $1; exit }')
		if [ -z "$h" ]; then
			bad "$drv: missing: $subj"
			miss=$((miss + 1))
			continue
		fi
		pid=$(git -C "$dir" show --format= "$h" | git patch-id --stable |
			cut -d' ' -f1)
		if [ "$pid" = "$want" ]; then
			pass "$drv: ${h:0:13} $subj"
		else
			note "$drv: ${h:0:13} $subj: patch-id differs from the manifest's (another version or a backport)"
		fi
	done < <(grep -v '^#' "$manifest")

	echo "$((n - miss)) of $n manifest patches present"

	# PADL Intel driver commits the manifest does not list, for information.
	printf '%s\n' "$log" | while IFS=$'\t' read -r h subj mail; do
		[ "$mail" = lukeh@padl.com ] || continue
		case $subj in
		igc:* | igb:* | ice:* | Revert*) ;;
		*) continue ;;
		esac
		awk -F'\t' -v s="$subj" '$1 == "patch" && $5 == s { f = 1 }
			END { exit !f }' "$manifest" ||
			note "not in manifest: ${h:0:13} $subj"
	done | head -40
}

# The bytes of a module file, decompressed.
mod_cat() {
	case $1 in
	*.zst) zstd -dc -- "$1" ;;
	*.xz) xz -dc -- "$1" ;;
	*.gz) gzip -dc -- "$1" ;;
	*) cat -- "$1" ;;
	esac
}

# marker_check MOD FILE KALLSYMS DYNDBG_CONTROL
#   KALLSYMS / DYNDBG_CONTROL: a file with the loaded module's lines, or "".
marker_check() {
	local mod=$1 file=$2 ksyms=$3 dctl=$4 tmp has_dyndbg=0
	local type drv kind value what found

	tmp=$(mktemp) || exit 2
	if ! mod_cat "$file" >"$tmp" 2>/dev/null; then
		bad "$mod: cannot read $file"
		rm -f "$tmp"
		return
	fi
	if [ -n "$dctl" ] || grep -aq '__dyndbg' "$tmp"; then
		has_dyndbg=1
	fi

	while IFS=$'\t' read -r type drv kind value what; do
		[ "$type" = marker ] && [ "$drv" = "$mod" ] || continue
		case $kind in
		str+ | str-)
			if grep -aqF -- "$value" "$tmp"; then found=1; else found=0; fi
			if [ "$kind" = str+ ] && [ $found = 1 ]; then
				pass "$mod: string present \"$value\": $what"
			elif [ "$kind" = str- ] && [ $found = 0 ]; then
				pass "$mod: string absent \"$value\": $what"
			elif [ "$kind" = str+ ]; then
				bad "$mod: string missing \"$value\": $what"
			else
				bad "$mod: stock string present \"$value\": $what"
			fi
			;;
		dbg+)
			if [ $has_dyndbg = 0 ]; then
				note "$mod: built without dynamic debug, not tested: \"$value\""
			elif { [ -n "$dctl" ] && grep -qF -- "$value" "$dctl"; } ||
			     { [ -z "$dctl" ] && grep -aqF -- "$value" "$tmp"; }; then
				pass "$mod: debug format present \"$value\": $what"
			else
				bad "$mod: debug format missing \"$value\": $what"
			fi
			;;
		sym+ | sym?)
			found=0
			if [ -n "$ksyms" ]; then
				grep -qE " $value(\.[a-z0-9.]+)?([[:space:]]|$)" "$ksyms" && found=1
			else
				grep -aqP "\x00\Q$value\E(\.[a-z0-9.]+)?\x00" "$tmp" && found=1
			fi
			if [ $found = 1 ]; then
				pass "$mod: symbol $value: $what"
			elif [ "$kind" = 'sym?' ]; then
				note "$mod: symbol $value not found (static, a compiler may inline it): $what"
			else
				bad "$mod: symbol $value missing: $what"
			fi
			;;
		esac
	done < <(grep -v '^#' "$manifest")
	rm -f "$tmp"
}

host_check() {
	local mod file src want ks dc c mods tested=0
	mods=$(awk -F'\t' '$1 == "marker" { print $2 }' "$manifest" | sort -u)

	echo "host $(uname -n) $(uname -r)"
	for mod in $mods; do
		if [ ! -d "/sys/module/$mod" ]; then
			note "$mod: not loaded, not tested"
			continue
		fi
		tested=$((tested + 1))
		file=$(modinfo -n "$mod" 2>/dev/null)
		if [ -z "$file" ] || [ ! -r "$file" ]; then
			bad "$mod: loaded, but its module file is not found"
			continue
		fi
		want=$(cat "/sys/module/$mod/srcversion" 2>/dev/null)
		src=$(modinfo -F srcversion "$file" 2>/dev/null)
		if [ -n "$want" ] && [ "$src" != "$want" ]; then
			bad "$mod: $file (srcversion $src) is not the loaded module ($want)"
			continue
		fi
		echo "$mod: $file srcversion ${src:-?}"

		ks=$(mktemp)
		grep -E "\[$mod\]" /proc/kallsyms >"$ks" 2>/dev/null
		[ -s "$ks" ] || { rm -f "$ks"; ks=; }
		dc=
		for c in /sys/kernel/debug/dynamic_debug/control \
			 /proc/dynamic_debug/control; do
			if [ -r "$c" ]; then
				dc=$(mktemp)
				grep -F "[$mod]" "$c" >"$dc" 2>/dev/null
				break
			fi
		done
		marker_check "$mod" "$file" "$ks" "$dc"
		rm -f ${ks:+"$ks"} ${dc:+"$dc"}
	done
	[ "$tested" -gt 0 ] || bad "none of the manifest's modules is loaded"
}

modules_check() {
	local arg mod file
	[ $# -gt 0 ] || { echo "modules: give MOD=FILE ..." >&2; exit 2; }
	for arg; do
		mod=${arg%%=*}
		file=${arg#*=}
		echo "$mod: $file"
		marker_check "$mod" "$file" "" ""
	done
}

case $mode in
tree) tree_check "$@" ;;
host) host_check ;;
modules) modules_check "$@" ;;
*)
	sed -n '4,27p' "$0" >&2
	exit 2
	;;
esac

exit $fail
