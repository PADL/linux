#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# Test the per-port semantics of the MDB_FLAGS_STREAM_RESERVED flag. A bridge
# port that holds at least one stream-reserved MDB membership is "SR-active":
# for multicast whose VLAN PCP maps (via an mqprio/TC configuration on the
# bridge netdev) to a non-zero traffic class, that port forwards only its own
# stream-reserved groups and drops every other group. TC 0 traffic, and ports
# that are not SR-active, are unaffected. Removing the last stream-reserved
# membership returns the port to normal forwarding.
#
#                                +------------------------+
#                                | H1 (vrf) - talker      |
#                                | + $h1                  |
#                                +----|-------------------+
#                                     | PCP-tagged mcast
# +-----------------------------------|------------------------------------+
# | SW                                + $swp1   BR0 (802.1q, mqprio)        |
# |                                                                         |
# |        + $swp2 (SR-active member)         + $swp3 (plain member)        |
# +--------|----------------------------------|----------------------------+
#          |                                  |
#     +----|-------------------+         +----|-------------------+
#     | H2 (vrf) - listener    |         | H3 (vrf) - listener    |
#     | + $h2                  |         | + $h3                  |
#     +------------------------+         +------------------------+

ALL_TESTS="
	cfg_test
	fwd_sr_member_test
	fwd_foreign_blocked_test
	fwd_tc_toggle_test
	fwd_sr_cleared_test
	fwd_sr_refcount_test
	fwd_sr_ipv6_test
"

NUM_NETIFS=6
source lib.sh
source tc_common.sh

# GRP is the stream-reserved group; GRP2 is a plain group that both swp2 and
# swp3 join, used to show that an SR-active port drops a non-reserved group.
GRP=239.1.1.1
GRP_DMAC=01:00:5e:01:01:01
GRP2=239.1.1.2
GRP2_DMAC=01:00:5e:01:01:02
GRP3=239.1.1.3
GRP3_DMAC=01:00:5e:01:01:03
# IPv6 (MLD) groups: GRP6 is stream-reserved, GRP6B is a plain group.
GRP6=ff0e::1
GRP6_DMAC=33:33:00:00:00:01
GRP6B=ff0e::2
GRP6B_DMAC=33:33:00:00:00:02
# Source for the (S, G) configuration check.
SRC=192.0.2.10
# PCP 3 is SR class A; the mqprio map below sends it to TC 1.
SR_PCP=3
BE_PCP=0
VID=10

h1_create()
{
	simple_if_init $h1
	vlan_create $h1 $VID v$h1 192.0.2.1/28
}

h1_destroy()
{
	vlan_destroy $h1 $VID
	simple_if_fini $h1
}

h2_create()
{
	simple_if_init $h2
	vlan_create $h2 $VID v$h2 192.0.2.2/28
}

h2_destroy()
{
	vlan_destroy $h2 $VID
	simple_if_fini $h2
}

h3_create()
{
	simple_if_init $h3
	vlan_create $h3 $VID v$h3 192.0.2.3/28
}

h3_destroy()
{
	vlan_destroy $h3 $VID
	simple_if_fini $h3
}

switch_create()
{
	# The bridge must have multiple TX queues so that an mqprio qdisc (which
	# populates the netdev prio->tc map the SR filter consults) can be
	# attached, and a multicast querier so that the bridge forwards
	# selectively (the SR filter runs in br_multicast_flood(), not the flood
	# path).
	ip link add name br0 numtxqueues 8 numrxqueues 8 type bridge \
		vlan_filtering 1 vlan_default_pvid 0 \
		mcast_snooping 1 mcast_igmp_version 3 mcast_mld_version 2 \
		mcast_querier 1
	bridge vlan add vid $VID dev br0 self
	ip link set dev br0 up

	# A link-local address lets the bridge act as the IPv6 (MLD) querier,
	# mirroring the IGMP querier; without an active querier the bridge floods
	# and the SR filter (in br_multicast_flood()) is bypassed.
	ip address add fe80::1/64 dev br0 nodad

	local swp
	for swp in $swp1 $swp2 $swp3; do
		ip link set dev $swp master br0
		ip link set dev $swp up
		bridge vlan add vid $VID dev $swp
	done

	# PCP $SR_PCP -> TC 1, everything else -> TC 0 (software mode).
	tc qdisc add dev br0 root handle 100: mqprio num_tc 2 \
		map 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 \
		queues 1@0 1@1 hw 0

	tc qdisc add dev $h2 clsact
	tc qdisc add dev $h3 clsact

	# Wait for the bridge's own querier to become active.
	sleep 10
}

switch_destroy()
{
	tc qdisc del dev $h3 clsact
	tc qdisc del dev $h2 clsact
	tc qdisc del dev br0 root handle 100: mqprio 2>/dev/null

	local swp
	for swp in $swp3 $swp2 $swp1; do
		bridge vlan del vid $VID dev $swp
		ip link set dev $swp down
		ip link set dev $swp nomaster
	done

	ip link set dev br0 down
	bridge vlan del vid $VID dev br0 self
	ip link del dev br0
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	swp1=${NETIFS[p2]}
	swp2=${NETIFS[p3]}
	h2=${NETIFS[p4]}
	swp3=${NETIFS[p5]}
	h3=${NETIFS[p6]}

	vrf_prepare
	forwarding_enable

	h1_create
	h2_create
	h3_create
	switch_create
}

cleanup()
{
	pre_cleanup

	switch_destroy
	h3_destroy
	h2_destroy
	h1_destroy

	forwarding_restore
	vrf_cleanup
}

# Probe whether the running kernel and iproute2 understand the flag. If not,
# the whole suite is skipped, so it is safe to invoke on an unpatched system.
stream_reserved_supported()
{
	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved 2>/dev/null
	if [[ $? -ne 0 ]]; then
		return 1
	fi
	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID
	return 0
}

cfg_test()
{
	RET=0

	# The flag must be rejected on host groups.
	bridge mdb add dev br0 port br0 grp $GRP permanent vid $VID \
		stream_reserved 2>/dev/null
	check_fail $? "stream_reserved accepted on a host group"

	# Add a port group with the flag and confirm it is reflected in dump.
	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved
	check_err $? "Failed to add stream_reserved entry"
	bridge -d mdb show dev br0 | grep -q "$GRP.*stream_reserved"
	check_err $? "stream_reserved flag not shown in dump"

	# The other state flags must not be disturbed: a permanent entry stays
	# permanent and carries no group timer when stream_reserved is set.
	bridge -d mdb get dev br0 grp $GRP vid $VID | grep -q "permanent"
	check_err $? "stream_reserved entry not kept \"permanent\""
	bridge -d -s mdb get dev br0 grp $GRP vid $VID | grep -q " 0.00"
	check_err $? "\"permanent\" stream_reserved entry has a pending group timer"

	# The flag is also accepted, and reported, on a source-specific (S, G).
	bridge mdb add dev br0 port $swp2 grp $GRP3 src $SRC permanent vid $VID \
		stream_reserved
	check_err $? "stream_reserved rejected on an (S, G) entry"
	bridge -d mdb show dev br0 | grep "$SRC" | grep -q stream_reserved
	check_err $? "stream_reserved flag not shown on (S, G) entry"
	bridge mdb del dev br0 port $swp2 grp $GRP3 src $SRC vid $VID

	# Replacing without the flag must clear it.
	bridge mdb replace dev br0 port $swp2 grp $GRP permanent vid $VID
	bridge -d mdb show dev br0 | grep -q "$GRP.*stream_reserved"
	check_fail $? "stream_reserved flag not cleared on replace"

	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID

	log_test "MDB stream_reserved configuration"
}

rx_filter_install()
{
	local dev=$1 pref=$2 grp=$3 ethtype=${4:-ipv4}

	tc filter add dev $dev ingress protocol 802.1q pref $pref handle $pref \
		flower vlan_ethtype $ethtype vlan_id $VID dst_ip $grp action drop
}

rx_filter_uninstall()
{
	local dev=$1 pref=$2

	tc filter del dev $dev ingress protocol 802.1q pref $pref handle $pref \
		flower
}

send_mc()
{
	local grp=$1 dmac=$2 pcp=$3

	$MZ $h1 -a own -b $dmac -c 1 -p 64 \
		-A 192.0.2.1 -B $grp -t udp -Q $pcp:$VID -q
}

send_mc6()
{
	local grp=$1 dmac=$2 pcp=$3

	$MZ -6 $h1 -a own -b $dmac -c 1 -p 64 \
		-A 2001:db8:1::1 -B $grp -t udp -Q $pcp:$VID -q
}

# An SR-class frame for a port's own stream-reserved group is delivered to it.
fwd_sr_member_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved
	rx_filter_install $h2 1 $GRP

	send_mc $GRP $GRP_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 1 1
	check_err $? "SR-class frame not delivered to its reserved member"

	rx_filter_uninstall $h2 1
	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID

	log_test "MDB stream_reserved member delivery"
}

# swp2 is SR-active (reserved member of GRP). A non-reserved group GRP2 in the
# SR class is dropped on swp2 but delivered to the plain member swp3, while a
# best-effort (TC 0) frame reaches both.
fwd_foreign_blocked_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved
	bridge mdb add dev br0 port $swp2 grp $GRP2 permanent vid $VID
	bridge mdb add dev br0 port $swp3 grp $GRP2 permanent vid $VID

	rx_filter_install $h2 2 $GRP2
	rx_filter_install $h3 2 $GRP2

	# SR-class: blocked on the SR-active port, delivered to the plain port.
	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "non-reserved SR-class frame leaked to SR-active port"
	tc_check_packets "dev $h3 ingress" 2 1
	check_err $? "non-reserved SR-class frame not delivered to plain port"

	# Best-effort (TC 0): unaffected, delivered to both.
	send_mc $GRP2 $GRP2_DMAC $BE_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "best-effort frame not delivered to SR-active port"
	tc_check_packets "dev $h3 ingress" 2 2
	check_err $? "best-effort frame not delivered to plain port"

	rx_filter_uninstall $h3 2
	rx_filter_uninstall $h2 2

	bridge mdb del dev br0 port $swp3 grp $GRP2 permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP2 permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID

	log_test "MDB stream_reserved blocks non-reserved SR-class group"
}

# The gate only engages while the prio->tc map has a non-zero class. With the
# mqprio qdisc removed, an SR-active port forwards the non-reserved group again.
fwd_tc_toggle_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved
	bridge mdb add dev br0 port $swp2 grp $GRP2 permanent vid $VID

	rx_filter_install $h2 2 $GRP2

	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "non-reserved SR-class frame leaked while TC enabled"

	# Drop the TC configuration; the prio->tc map is gone, gate is inert.
	tc qdisc del dev br0 root handle 100: mqprio

	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "frame not delivered after TC configuration removed"

	tc qdisc add dev br0 root handle 100: mqprio num_tc 2 \
		map 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 \
		queues 1@0 1@1 hw 0

	rx_filter_uninstall $h2 2

	bridge mdb del dev br0 port $swp2 grp $GRP2 permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID

	log_test "MDB stream_reserved gate follows TC configuration"
}

# Removing the last stream-reserved membership makes the port no longer
# SR-active, so the previously blocked group forwards normally again.
fwd_sr_cleared_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved
	bridge mdb add dev br0 port $swp2 grp $GRP2 permanent vid $VID

	rx_filter_install $h2 2 $GRP2

	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "non-reserved SR-class frame leaked while port SR-active"

	# Drop the last stream-reserved membership on swp2.
	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID

	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "frame not delivered after last reserved member removed"

	rx_filter_uninstall $h2 2

	bridge mdb del dev br0 port $swp2 grp $GRP2 permanent vid $VID

	log_test "MDB stream_reserved cleared on last member removal"
}

# A port stays SR-active while it holds any stream-reserved membership. With two
# such groups, dropping one must not unblock a foreign group; dropping the last
# one must. Exercises the per-port refcount beyond 0<->1.
fwd_sr_refcount_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP permanent vid $VID \
		stream_reserved
	bridge mdb add dev br0 port $swp2 grp $GRP3 permanent vid $VID \
		stream_reserved
	bridge mdb add dev br0 port $swp2 grp $GRP2 permanent vid $VID

	rx_filter_install $h2 2 $GRP2

	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "foreign SR-class frame leaked with two reserved memberships"

	# One reserved membership remains: the port is still SR-active.
	bridge mdb del dev br0 port $swp2 grp $GRP permanent vid $VID
	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "foreign SR-class frame leaked with one reserved membership left"

	# Last reserved membership gone: the port is no longer SR-active.
	bridge mdb del dev br0 port $swp2 grp $GRP3 permanent vid $VID
	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "foreign frame not delivered after last reserved membership removed"

	rx_filter_uninstall $h2 2
	bridge mdb del dev br0 port $swp2 grp $GRP2 permanent vid $VID

	log_test "MDB stream_reserved per-port refcount"
}

# The per-port semantics are protocol-independent: an IPv6/MLD non-reserved
# group at SR-class is dropped on the SR-active port and delivered to a plain
# member, just as for IPv4.
fwd_sr_ipv6_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP6 permanent vid $VID \
		stream_reserved
	bridge mdb add dev br0 port $swp2 grp $GRP6B permanent vid $VID
	bridge mdb add dev br0 port $swp3 grp $GRP6B permanent vid $VID

	rx_filter_install $h2 2 $GRP6B ipv6
	rx_filter_install $h3 2 $GRP6B ipv6

	send_mc6 $GRP6B $GRP6B_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "non-reserved SR-class IPv6 frame leaked to SR-active port"
	tc_check_packets "dev $h3 ingress" 2 1
	check_err $? "non-reserved SR-class IPv6 frame not delivered to plain port"

	send_mc6 $GRP6B $GRP6B_DMAC $BE_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "best-effort IPv6 frame not delivered to SR-active port"
	tc_check_packets "dev $h3 ingress" 2 2
	check_err $? "best-effort IPv6 frame not delivered to plain port"

	rx_filter_uninstall $h3 2
	rx_filter_uninstall $h2 2

	bridge mdb del dev br0 port $swp3 grp $GRP6B permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP6B permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP6 permanent vid $VID

	log_test "MDB stream_reserved blocks non-reserved SR-class IPv6 group"
}

trap cleanup EXIT

setup_prepare
setup_wait

if ! stream_reserved_supported; then
	log_test_skip "MDB stream_reserved" \
		"kernel or iproute2 lacks MDB_FLAGS_STREAM_RESERVED support"
	exit $EXIT_STATUS
fi

tests_run

exit $EXIT_STATUS
