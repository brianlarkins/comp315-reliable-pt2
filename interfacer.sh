#!/bin/bash

/sbin/modprobe dummy

/sbin/ip link add eth10 type dummy
/sbin/ip link add eth11 type dummy
/sbin/ip link add eth12 type dummy

/sbin/ip addr add 172.16.3.1/24 brd + dev eth10
/sbin/ip addr add 172.16.4.1/24 brd + dev eth11
/sbin/ip addr add 172.16.5.1/24 brd + dev eth12

/sbin/tc qdisc add dev eth10 root netem delay 50ms 5ms
/sbin/tc qdisc add dev eth11 root netem delay 50ms 5ms
/sbin/tc qdisc add dev eth12 root netem delay 50ms 5ms

/sbin/tc qdisc change dev eth10 root netem corrupt 0.1%
/sbin/tc qdisc change dev eth11 root netem loss 5%
/sbin/tc qdisc change dev eth12 root netem corrupt 0.1%
/sbin/tc qdisc change dev eth12 root netem loss 5%
/sbin/tc qdisc change dev eth12 root netem duplicate 5%

#/sbin/ip link delete eth10
#/sbin/ip link delete eth11
#/sbin/ip link delete eth12
