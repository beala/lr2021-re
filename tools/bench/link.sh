#!/bin/bash
# link.sh <tx args...>: rx on the RAK, run "tx <args>" on the xiao, stop rx, print T + SUM
D=$(dirname "$0")
L=$(wc -l < rx.log); echo rx > rx.fifo; sleep 1.2
"$D/sw.sh" tx 600 tx "$@" | grep -E '^T,|ERR'
sleep 1.2; echo x > rx.fifo
timeout 10 bash -c "until sed -n '$((L+1)),\$p' rx.log | grep -q '^.* SUM,'; do sleep 0.2; done"
sed -n "$((L+1)),\$p" rx.log | cut -d' ' -f2- | grep -E '^SUM,|ERR|RECOV'
sed -n "$((L+1)),\$p" rx.log | cut -d' ' -f2- | grep -E '^S,' | tail -3
