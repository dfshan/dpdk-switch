#!/bin/bash
set -u

pcs="hosts.info"
exp_pc="192.168.1.224"
recv_ip="192.168.0.224"
#ecn_ks=$(seq 2 2 40)
#ecn_ks=$(seq 60 5 200)
ecn_ks=$(seq 40 2 80)
log_dir="throughput-k"
switch_dir="$HOME/dpdk-switch"
tcp_dir="$HOME/tcp-test"
client_num=2

USAGE="USAGE: $0 <protocol: dctcp|ecn> <tso: on|off> <cedm: on|off> <client number> [ecn thresholds in lists]"
if (($# < 4)); then
    echo $USAGE
    exit
fi
protocol="$1"
tso="$2"
cedm="$3"
client_num="$4"
if (($# >= 5)); then
    ecn_ks="$5"
fi
if [ "$protocol" != 'dctcp' -a "$protocol" != 'ecn' ]; then
    echo "Argument 1 should be either 'dctcp' or 'ecn'!"
    echo $USAGE
    exit
fi
if [ "$tso" != 'on' -a "$tso" != 'off' ]; then
    echo "Argument 2 should be either 'on' or 'off'!"
    echo $USAGE
    exit
fi
if [ "$cedm" != 'on' -a "$cedm" != 'off' ]; then
    echo "Argument 3 should be either 'on' or 'off'!"
    echo $USAGE
    exit
fi
if (( $client_num > 3 )); then
    echo "Client number should smaller than 3!"
    exit
fi

function init_network () {
    local devname="em1"
    local setoffload="ethtool -K $devname gso off tso off gro off lro off"
    local setcoalescing="ethtool -C $devname rx-usecs 0 rx-frames 0 rx-usecs-irq 0 rx-frames-irq 0"
    local setsysctl="sysctl -w net.ipv4.tcp_ecn=1 net.ipv4.tcp_sack=0"
    local othersettings="iptables -F"
    while read ip; do
	    marked=$(echo $ip | grep "#")
	    if [ "$marked" != "" ]; then
	    	continue
	    fi
	    echo "----------------------------------------------------------"
	    echo "networking setting for $ip"
        cmd="ssh -n root@$ip \"$setoffload; $setcoalescing; $setsysctl; $othersettings\""
        ssh -n $exp_pc "$cmd"
    done < $pcs
}

function set_network () {
    local devname="em1"
    local USAGE="USAGE of function set_network: <protocol: dctcp|ecn> <tso: on|off>"
    if (( $# < 2 )); then
        echo $USAGE
        exit
    fi
    local protocol="$1"
    local tso="$2"
    if [ "$protocol" != 'dctcp' -a "$protocol" != 'ecn' ]; then
        echo "Argument 1 should be either 'dctcp' or 'ecn'!"
        echo $USAGE
        exit
    elif [ "$protocol" = 'ecn' ]; then
        protocol='reno'
    fi
    if [ "$tso" != 'on' -a "$tso" != 'off' ]; then
        echo $USAGE
        exit
    fi
    settso="ethtool -K $devname tso $tso"
    while read ip; do
	    marked=$(echo $ip | grep "#")
	    if [ "$marked" != "" ]; then
	    	continue
	    fi
	    echo "----------------------------------------------------------"
	    echo "Set CC to $protocol and TSO to $tso for $ip"
        cmd="ssh -n root@$ip \"ethtool -K $devname tso $tso; sysctl -w net.ipv4.tcp_congestion_control=$protocol\""
        ssh -n $exp_pc "$cmd"
    done < $pcs
}

function check_dir () {
    archive_dir="archive"
    if (($# < 1)); then
        echo "USAGE: $0 {directory name}"
        exit 1
    fi
    local dirname="$1"
    if [ -a $dirname ]; then
        local odir="$archive_dir/${dirname}_$(date +"%Y%m%d%H%M%S")"
        echo "Directory \"$dirname\" exists, move to \"$odir\""
        mkdir -p $archive_dir
        mv $dirname $odir
    fi
    mkdir -p $dirname
}

#check_dir $log_dir
mkdir -p $log_dir

sudo pkill -9 main
sudo $switch_dir/build/app/main -c 0xe --log-level=7 -- -p 0xf &
sleep 5s
init_network
set_network $protocol $tso
(cd $switch_dir; make)
log_prefix="${protocol}_tso_${tso}_k_"
ssh $exp_pc "pkill -9 tcp_server"
log_fname="${protocol}_tso_${tso}_cedm_${cedm}_n_${client_num}"
echo -n "" > $log_dir/$log_fname
if [ "$cedm" != 'on' -a "$cedm" != 'off' ]; then
    echo "Argument 3 should be either 'on' or 'off'!"
    echo $USAGE
    exit
fi
if [ "$cedm" = 'on' ]; then
    cedm='true'
else
    cedm='false'
fi
sed "/^\s*#/!s/^.*cedm_enable.*\$/cedm_enable = $cedm/g" $switch_dir/switch.conf.example > switch.conf
for k in $ecn_ks; do
	echo "-------- ecn_threshold=${k}KB  -------------------------"
    # change ecn threshold in configuration file
    sed -i "/^\s*#/!s/^.*ecn_threshold.*\$/ecn_threshold = $k/g" switch.conf
    # start switch
    sudo pkill -9 main
    sleep 5s
    sudo $switch_dir/build/app/main -c 0xe --log-level=7 -- -p 0xf &
    switch_pid=$!
    sleep 10s
    # start server
    ssh $exp_pc "$tcp_dir/tcp_server 5 6666 > /dev/null &"
    # start clients
    num=0
    while read ip; do
        if (( $num >= $client_num )); then
            break
        fi
	    marked=$(echo $ip | grep "#")
	    if [ "$marked" != "" ]; then
	    	continue
	    fi
        if [ "$ip" = "$recv_ip" ]; then
            continue
        fi
	    echo "-------- start client: $ip --> $recv_ip -------------------------"
        cmd="ssh -n $ip \"$tcp_dir/tcp_client $recv_ip 0 > /dev/null&\""
        ssh -n $exp_pc "$cmd"
        num=$(($num+1))
    done < $pcs
    sleep 31s
    ssh -n $exp_pc "pkill -9 tcp_server"
    sudo kill -2 $switch_pid
    throughput=$(ssh $exp_pc "cat ~/throughput.log")
    throughput=$(echo "$throughput" | awk 'NR>1 {sum += $2} END{sum -= $2; print int(sum/(NR-2))}')
    echo "$k $throughput" >> $log_dir/$log_fname
done
sudo pkill -9 main
