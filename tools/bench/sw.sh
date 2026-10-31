#!/bin/bash
# sw.sh <tx|rx> <timeout s> <command...>: send a command, keep USB alive with newlines,
# print the new log lines once "# ready" shows up. Uses tx.fifo/rx.fifo and tx.log/rx.log in the
# current directory (from portd.py: portd.py <xiao port> tx.log tx.fifo, portd.py <nicerf port> rx.log rx.fifo).
b=$1; to=$2; shift 2
L=$(wc -l < $b.log); echo "$*" > $b.fifo
end=$((SECONDS+to))
until sed -n "$((L+1)),\$p" $b.log | grep -q '# ready' || [ $SECONDS -ge $end ]; do sleep 0.5; echo "" > $b.fifo; done
sed -n "$((L+1)),\$p" $b.log | cut -d' ' -f2- | grep -vE '^H,|^>> *$'
