#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin

run() (
    set -eu
    mount -t proc proc /proc
    mount -t sysfs sysfs /sys
    mount -t devtmpfs dev /dev
    for module in /modules/*.ko; do insmod "$module"; done
    for attempt in $(seq 1 60); do
        [ -b /dev/sda ] && [ -b /dev/sdb ] && break
        sleep 1
    done
    [ -b /dev/sda ] && [ -b /dev/sdb ]
    input=''
    result_disk=''
    for device in /dev/sda /dev/sdb; do
        if [ "$(head -c 13 "$device")" = SHOP_INPUT_V1 ]; then input=$device; fi
        if [ "$(head -c 14 "$device")" = SHOP_OUTPUT_V1 ]; then result_disk=$device; fi
    done
    [ -n "$input" ] && [ -n "$result_disk" ] && [ "$input" != "$result_disk" ] || exit 2
    echo "SHOP_SIM_DISKS input=$input output=$result_disk"
    tar -xf "$input" -C /
    [ -s /runs ]
    grep -m1 'model name' /proc/cpuinfo
    grep -m1 '^flags' /proc/cpuinfo
    echo "SHOP_SIM_CPUS $(grep -c '^processor' /proc/cpuinfo)"
    mkdir -p /result
    export NCPU=8 LD_LIBRARY_PATH=/opt/arrow/lib
    if [ -f /shop-profile ]; then export PROFILE=1; fi
    while read -r label case_name repeat; do
        output=/result/$case_name/$label
        rm -rf "$output"
        mkdir -p "$output"
        chown 1000:1000 "$output"
        echo "SHOP_SIM_RUN $case_name $label $repeat"
        /bin/measure "/programs/$label" "/inputs/$case_name" "$output"
        for table in user_daily campaign_daily; do
            set -- "$output/$table"/*.parquet
            [ -s "$1" ] || exit 3
        done
        echo "SHOP_SIM_OK $case_name $label $repeat"
    done < /runs
    tar -cf "$result_disk" -C /result .
    echo SHOP_SIM_COMPLETE
)
run
result=$?
if [ "$result" -ne 0 ]; then echo "SHOP_SIM_FAILED $result"; fi
poweroff -f
