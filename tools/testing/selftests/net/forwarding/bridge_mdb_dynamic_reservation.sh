#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# Test 802.1Qat stream reservation admission control built from a bridge MDB
# dynamic-reservation entry plus a tc-flower egress rule.
#
# An MDB entry added with state "dynamic_reservation" marks a multicast group
# as a reserved stream (the allow-list, typically maintained by an SRP daemon).
# When the bridge forwards a multicast frame it tags the skb's
# "dynamic_reservation_hit" metadata if the destination is such a reserved
# stream. A tc-flower rule installed on the egress (listener-facing) bridge port
# then drops SR-class frames that did not hit a reservation:
#
#   tc filter add dev $swp egress protocol 802.1q flower \
#       vlan_prio $SR_PCP dynamic_reservation_hit 0 action drop
#
# Reserved streams (dynamic_reservation_hit 1) and best-effort traffic
# (vlan_prio != $SR_PCP) are unaffected. Host-delivered frames egress via the
# bridge itself, not a port, so they are not policed by this rule.
#
#                                +------------------------+
#                                | H1 (vrf) - talker      |
#                                | + $h1                  |
#                                +----|-------------------+
#                                     | PCP-tagged mcast
# +-----------------------------------|------------------------------------+
# | SW             $swp1 (ingress)                  BR0 (802.1q)            |
# |                                   +                                     |
# |   + $swp2 (listener, sr policer)      + $swp3 (listener, sr policer)    |
# +------------------|-------------------------|---------------------------+
#                    |                         |
#     +--------------|---------+   +-----------|------------+
#     | H2 (vrf) - listener    |   | H3 (vrf) - listener    |
#     | + $h2                  |   | + $h3                  |
#     +------------------------+   +------------------------+

ALL_TESTS="
	cfg_test
	fwd_sr_member_test
	fwd_foreign_blocked_test
	fwd_unicast_blocked_test
	fwd_sr_ipv6_test
"

NUM_NETIFS=6
source lib.sh
source tc_common.sh

# GRP is the reserved-stream group; GRP2 is a plain group that both swp2 and
# swp3 join, used to show that a foreign SR-class group is dropped at egress.
GRP=239.1.1.1
GRP_DMAC=01:00:5e:01:01:01
GRP2=239.1.1.2
GRP2_DMAC=01:00:5e:01:01:02
GRP3=239.1.1.3
GRP3_DMAC=01:00:5e:01:01:03
# IPv6 (MLD) groups: GRP6B is a plain group.
GRP6B=ff0e::2
GRP6B_DMAC=33:33:00:00:00:02
# Source for the (S, G) configuration check.
SRC=192.0.2.10
# PCP 3 is SR class A and matches the egress policer's vlan_prio.
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
	ip link add name br0 type bridge \
		vlan_filtering 1 vlan_default_pvid 0 \
		mcast_snooping 1 mcast_igmp_version 3 mcast_mld_version 2 \
		mcast_querier 1
	bridge vlan add vid $VID dev br0 self
	ip link set dev br0 up

	# A link-local address lets the bridge act as the IPv6 (MLD) querier,
	# mirroring the IGMP querier.
	ip address add fe80::1/64 dev br0 nodad

	local swp
	for swp in $swp1 $swp2 $swp3; do
		ip link set dev $swp master br0
		ip link set dev $swp up
		bridge vlan add vid $VID dev $swp
	done

	tc qdisc add dev $h2 clsact
	tc qdisc add dev $h3 clsact

	# Wait for the bridge's own querier to become active.
	sleep 10
}

switch_destroy()
{
	tc qdisc del dev $h3 clsact
	tc qdisc del dev $h2 clsact

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

# Install/remove the SR admission policer on an egress bridge port: drop
# SR-class (PCP $SR_PCP) frames that did not hit a dynamic-reservation MDB entry.
sr_policer_install()
{
	local dev=$1

	tc qdisc add dev $dev clsact
	tc filter add dev $dev egress protocol 802.1q pref 1 handle 1 flower \
		vlan_prio $SR_PCP dynamic_reservation_hit 0 action drop
}

sr_policer_uninstall()
{
	local dev=$1

	tc filter del dev $dev egress pref 1 handle 1 flower
	tc qdisc del dev $dev clsact
}

# Probe whether the running kernel and iproute2 understand the
# dynamic_reservation MDB state and the dynamic_reservation_hit flower key. If
# not, the whole suite is skipped, so it is safe to invoke on an unpatched
# system.
dynamic_reservation_supported()
{
	bridge mdb add dev br0 port $swp2 grp $GRP dynamic_reservation \
		vid $VID 2>/dev/null
	if [[ $? -ne 0 ]]; then
		return 1
	fi
	bridge mdb del dev br0 port $swp2 grp $GRP vid $VID

	tc qdisc add dev $swp2 clsact 2>/dev/null || return 1
	tc filter add dev $swp2 egress protocol 802.1q pref 1 handle 1 flower \
		vlan_prio $SR_PCP dynamic_reservation_hit 0 action drop \
		2>/dev/null
	local rc=$?
	tc qdisc del dev $swp2 clsact 2>/dev/null
	return $rc
}

cfg_test()
{
	RET=0

	# A dynamic_reservation port entry is accepted and dumped with the
	# matching state.
	bridge mdb add dev br0 port $swp2 grp $GRP dynamic_reservation vid $VID
	check_err $? "Failed to add dynamic_reservation entry"
	bridge mdb show dev br0 | grep "$GRP" | grep -q dynamic_reservation
	check_err $? "dynamic_reservation state not shown in dump"

	# It behaves like a permanent entry: it carries no group timer.
	bridge -s mdb get dev br0 grp $GRP vid $VID | grep "port $swp2" | \
		grep -q " 0.00"
	check_err $? "dynamic_reservation entry has a pending group timer"

	# Replacing without the state clears it back to permanent.
	bridge mdb replace dev br0 port $swp2 grp $GRP permanent vid $VID
	bridge mdb show dev br0 | grep "$GRP" | grep -q dynamic_reservation
	check_fail $? "dynamic_reservation state not cleared on replace"
	bridge mdb del dev br0 port $swp2 grp $GRP vid $VID

	# The state is also accepted, and reported, on a source-specific (S, G).
	bridge mdb add dev br0 port $swp2 grp $GRP3 src $SRC \
		dynamic_reservation vid $VID
	check_err $? "dynamic_reservation rejected on an (S, G) entry"
	bridge mdb show dev br0 | grep "$SRC" | grep -q dynamic_reservation
	check_err $? "dynamic_reservation state not shown on (S, G) entry"
	bridge mdb del dev br0 port $swp2 grp $GRP3 src $SRC vid $VID

	log_test "MDB dynamic_reservation configuration"
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

# An arbitrary unicast DA: the bridge floods it as unknown unicast, so it
# reaches the listeners unless dropped.
UC_DMAC=00:de:ad:be:ef:02

send_uc()
{
	local dip=$1 pcp=$2

	$MZ $h1 -a own -b $UC_DMAC -c 1 -p 64 \
		-A 192.0.2.1 -B $dip -t udp -Q $pcp:$VID -q
}

# An SR-class frame for a reserved stream hits the dynamic-reservation entry, so
# the egress policer admits it and it is delivered to the stream's member.
fwd_sr_member_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP dynamic_reservation vid $VID
	sr_policer_install $swp2
	tc filter add dev $h2 ingress protocol 802.1q pref 1 handle 1 flower \
		vlan_ethtype ipv4 vlan_id $VID dst_ip $GRP action drop

	send_mc $GRP $GRP_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 1 1
	check_err $? "reserved-stream SR-class frame not admitted to its member"

	tc filter del dev $h2 ingress pref 1 handle 1 flower
	sr_policer_uninstall $swp2
	bridge mdb del dev br0 port $swp2 grp $GRP vid $VID

	log_test "MDB dynamic_reservation member delivery"
}

# A foreign (non-reserved) group GRP2 at SR class does not hit a reservation, so
# the egress policer drops it on each listener port, while a best-effort (PCP 0)
# frame is admitted and delivered to both.
fwd_foreign_blocked_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP2 permanent vid $VID
	bridge mdb add dev br0 port $swp3 grp $GRP2 permanent vid $VID
	sr_policer_install $swp2
	sr_policer_install $swp3

	tc filter add dev $h2 ingress protocol 802.1q pref 2 handle 2 flower \
		vlan_ethtype ipv4 vlan_id $VID dst_ip $GRP2 action drop
	tc filter add dev $h3 ingress protocol 802.1q pref 2 handle 2 flower \
		vlan_ethtype ipv4 vlan_id $VID dst_ip $GRP2 action drop

	# SR-class: dropped at egress, reaches neither listener.
	send_mc $GRP2 $GRP2_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "foreign SR-class frame leaked to a listener"
	tc_check_packets "dev $h3 ingress" 2 0
	check_err $? "foreign SR-class frame leaked to a listener"

	# Best-effort (PCP 0): unaffected, delivered to both.
	send_mc $GRP2 $GRP2_DMAC $BE_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "best-effort frame not delivered"
	tc_check_packets "dev $h3 ingress" 2 1
	check_err $? "best-effort frame not delivered"

	tc filter del dev $h3 ingress pref 2 handle 2 flower
	tc filter del dev $h2 ingress pref 2 handle 2 flower
	sr_policer_uninstall $swp3
	sr_policer_uninstall $swp2
	bridge mdb del dev br0 port $swp3 grp $GRP2 permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP2 permanent vid $VID

	log_test "MDB dynamic_reservation blocks foreign SR-class group"
}

# Unicast can never belong to a reserved stream, so an SR-class unicast frame
# never hits a reservation and is dropped by the egress policer. A best-effort
# unicast frame is unaffected.
fwd_unicast_blocked_test()
{
	RET=0

	sr_policer_install $swp2
	tc filter add dev $h2 ingress protocol 802.1q pref 5 handle 5 flower \
		vlan_ethtype ipv4 vlan_id $VID dst_ip 192.0.2.2 action drop

	send_uc 192.0.2.2 $SR_PCP
	tc_check_packets "dev $h2 ingress" 5 0
	check_err $? "SR-class unicast leaked through the egress policer"

	send_uc 192.0.2.2 $BE_PCP
	tc_check_packets "dev $h2 ingress" 5 1
	check_err $? "best-effort unicast not delivered"

	tc filter del dev $h2 ingress pref 5 handle 5 flower
	sr_policer_uninstall $swp2

	log_test "MDB dynamic_reservation blocks SR-class unicast"
}

# The semantics are protocol-independent: a foreign IPv6/MLD group at SR class
# is dropped at egress, while best-effort is delivered.
fwd_sr_ipv6_test()
{
	RET=0

	bridge mdb add dev br0 port $swp2 grp $GRP6B permanent vid $VID
	bridge mdb add dev br0 port $swp3 grp $GRP6B permanent vid $VID
	sr_policer_install $swp2
	sr_policer_install $swp3

	tc filter add dev $h2 ingress protocol 802.1q pref 2 handle 2 flower \
		vlan_ethtype ipv6 vlan_id $VID dst_ip $GRP6B action drop
	tc filter add dev $h3 ingress protocol 802.1q pref 2 handle 2 flower \
		vlan_ethtype ipv6 vlan_id $VID dst_ip $GRP6B action drop

	send_mc6 $GRP6B $GRP6B_DMAC $SR_PCP
	tc_check_packets "dev $h2 ingress" 2 0
	check_err $? "foreign SR-class IPv6 frame leaked to a listener"
	tc_check_packets "dev $h3 ingress" 2 0
	check_err $? "foreign SR-class IPv6 frame leaked to a listener"

	send_mc6 $GRP6B $GRP6B_DMAC $BE_PCP
	tc_check_packets "dev $h2 ingress" 2 1
	check_err $? "best-effort IPv6 frame not delivered"
	tc_check_packets "dev $h3 ingress" 2 1
	check_err $? "best-effort IPv6 frame not delivered"

	tc filter del dev $h3 ingress pref 2 handle 2 flower
	tc filter del dev $h2 ingress pref 2 handle 2 flower
	sr_policer_uninstall $swp3
	sr_policer_uninstall $swp2
	bridge mdb del dev br0 port $swp3 grp $GRP6B permanent vid $VID
	bridge mdb del dev br0 port $swp2 grp $GRP6B permanent vid $VID

	log_test "MDB dynamic_reservation blocks foreign SR-class IPv6 group"
}

trap cleanup EXIT

setup_prepare
setup_wait

if ! dynamic_reservation_supported; then
	log_test_skip "MDB dynamic_reservation" \
		"kernel or iproute2 lacks MDB_DYNAMIC_RESERVATION / dynamic_reservation_hit support"
	exit $EXIT_STATUS
fi

tests_run

exit $EXIT_STATUS
