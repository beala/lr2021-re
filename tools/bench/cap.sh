#!/bin/bash
# cap.sh <name> <iqtx args (6)> -- <cap args>: start a long raw TX on the xiao, capture on the RAK
# mid-burst, save the capture bytes to caps/<name>.bin
D=$(dirname "$0"); mkdir -p caps; name=$1; shift
tx="$1 $2 $3 $4 $5 $6"; shift 6; [ "$1" = "--" ] && shift
LT=$(wc -l < tx.log); echo "iqtx $tx" > tx.fifo
sleep ${CAPDELAY:-1.5}
L=$(wc -l < rx.log)
"$D/sw.sh" rx 20 cap "$@" | grep -E "^# cap|ERR"
sed -n "$((L+1)),\$p" rx.log | cut -d' ' -f2- | grep '^C,' | cut -c3- | xxd -r -p > caps/$name.bin
until sed -n "$((LT+1)),\$p" tx.log | grep -q '# ready'; do sleep 0.5; echo "" > tx.fifo; done
sed -n "$((LT+1)),\$p" tx.log | cut -d' ' -f2- | grep '^Q' | head -2
ls -la caps/$name.bin | awk '{print $5" bytes"}'
