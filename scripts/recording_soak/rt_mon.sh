#!/bin/sh
echo "L $(cut -d' ' -f1-3 /proc/loadavg)"
echo "M $(awk '/MemAvailable/{print $2}' /proc/meminfo)"
echo "D $(ps -eo stat | grep -c '^D')"
echo "E $(ip -s link show eth0 | awk '/RX:/{getline; r=$3" "$4} /TX:/{getline; t=$3" "$4} END{print r" | "t}')"
echo "K $(dmesg | grep -cE 'Mips Failed|Oops|BUG:|hung task|blocked for more|I/O error')"
