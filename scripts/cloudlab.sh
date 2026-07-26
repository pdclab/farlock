#!/bin/bash

user="Jiatang"
machines=("apt030" "apt035" "apt033" "apt028" "apt034" "apt005" "apt029" "apt032" "apt018" "apt031")
domain="apt.emulab.net"
nodes=20

# Check if a command is provided
if [ $# -eq 0 ]; then
    echo "Usage: $0 <command> [args...]"
    echo "For scp: $0 scp <source> <destination>"
    exit 1
fi

# Determine if the command is scp
if [ "$1" == "scp" ]; then
    if [ $# -lt 3 ]; then
        echo "Usage: $0 scp <source> <destination>"
        exit 1
    fi

    source="$2"
    destination="$3"

    # Execute scp on each machine
    for m in "${machines[@]}"; do
        echo "Copying files to ${m}.${domain}..."
        scp "$source" "${user}@${m}.${domain}:$destination" &
    done
elif [ "$1" == "test" ]; then
    i=0
    for m in "${machines[@]}"; do
        echo "Executing on ${m}.${domain}..."
        ssh -n -f "${user}@${m}.${domain}" "cd farlock/benchmark; nohup python test.py --node-id $i > output.log 2>&1 &"
        i=$((i+1))
        if [ $i -ge $nodes ]; then
            break
        fi
    done
elif [ "$1" == "setup" ]; then
    i=1
    for m in "${machines[@]}"; do
        echo "Executing on ${m}.${domain}..."
        ssh "${user}@${m}.${domain}" "sudo apt update;sudo apt install -y rdma-core librdmacm-dev libibverbs-dev ibverbs-utils perftest infiniband-diags cmake" # libboost-all-dev libmemcached-dev memcached
        ssh "${user}@${m}.${domain}" "sudo ip addr add 10.30.2.$i/24 dev ibp130s0;sudo ip link set dev ibp130s0 up" 
        i=$((i+1))
    done
elif [ "$1" == "run" ]; then
    i=0
    for m in "${machines[@]}"; do
        #echo "Executing on ${m}.${domain}..."
        ssh "${user}@${m}.${domain}" "cd farlock/benchmark;./$2 $i" &
        i=$((i+1))
        if [ $i -ge $3 ]; then
            break
        fi
    done
else
    # Execute ssh command on each machine
    for m in "${machines[@]}"; do
        echo "Executing on ${m}.${domain}..."
        ssh "${user}@${m}.${domain}" "$@" &
    done
fi

# Wait for all background jobs to finish
wait