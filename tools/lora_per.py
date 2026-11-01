#!/usr/bin/env python3
"""LoRa packet delivery between the two console boards, with optional raw commands on each side.

  python3 tools/lora_per.py --bw 62.5 --sf 7 --n 100
  python3 tools/lora_per.py --tx-rc 0223:34 --rx-rc 0223:12          # sync word 0x34 into a 0x12 receiver
  python3 tools/lora_per.py --bw 500 --sf 5 --rx-rc 021F:0008         # undocumented 0x021F on the receiver

The XIAO transmits and the NiceRF board receives (swap with --tx nicerf). Both are reset with
`set radio`, then each --tx-rc / --rx-rc (OPCODE:hexparams, repeatable) is sent through the
console's raw `rc` command. The receiver is restarted so its settings apply. Prints packets
received intact, CRC and header errors, and the mean RSSI/SNR.

This transmits on --mhz from one board. Use a frequency and power you are allowed to use.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pram  # noqa: E402
from radio_ports import default_port  # noqa: E402

PAYLOAD = '00112233445566778899AABBCCDDEEFF'


def raw(con, spec):
    op, _, params = spec.partition(':')
    st, _ = con.rc(int(op, 16), 0, bytes.fromhex(params))
    if st != 0:
        raise RuntimeError(f'rc {spec}: st={st}')


def run(tx, rx, n, payload=PAYLOAD):
    """Send n packets. The receiver's port is drained after every packet: if it isn't, the
    host tty buffer fills after about 40 RXSTAT lines and the board drops the rest."""
    count = lambda s, k: int(re.search(k + r'=(\d+)', s)[1])
    s0 = rx.cmd('rxstats', '-> >')
    rx.s.reset_input_buffer()
    buf, txerr = b'', 0
    for _ in range(n):
        if 'OK' not in tx.cmd('loratx ' + payload, '->', 5):
            txerr += 1
        buf += rx.s.read(1 << 16)
    quiet = time.time()
    while time.time() - quiet < 1.0:
        b = rx.s.read(1 << 16)
        if b:
            buf, quiet = buf + b, time.time()
    s1 = rx.cmd('rxstats', '-> >')
    good = [line for line in buf.decode(errors='replace').splitlines()
            if line.startswith('RXSTAT') and 'crc=1' in line and line.rstrip().endswith(payload)]
    mean = lambda k: (sum(float(re.search(k + r'=(-?[\d.]+)', g)[1]) for g in good) / len(good)) if good else None
    return dict(n=n, good=len(good), txerr=txerr, rssi=mean('rssi'), snr=mean('snr'),
                **{k: count(s1, k) - count(s0, k) for k in ('crc_err', 'hdr_err')})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tx', default='xiao', choices=('xiao', 'nicerf'))
    ap.add_argument('--mhz', type=float, default=908.0)
    ap.add_argument('--bw', type=float, default=62.5)
    ap.add_argument('--sf', type=int, default=7)
    ap.add_argument('--dbm', type=int, default=-9)
    ap.add_argument('--n', type=int, default=100)
    ap.add_argument('--tx-rc', action='append', default=[], metavar='OP:HEX')
    ap.add_argument('--rx-rc', action='append', default=[], metavar='OP:HEX')
    args = ap.parse_args()

    ports = {'xiao': default_port('XIAO', '/dev/ttyACM0'), 'nicerf': default_port('RAK3401', '/dev/ttyACM1')}
    tx = pram.Console(ports[args.tx])
    rx = pram.Console(ports['nicerf' if args.tx == 'xiao' else 'xiao'])
    try:
        for c in (tx, rx):
            for cmd in (f'set radio {args.mhz},{args.bw:g},{args.sf},5', f'set tx {args.dbm}'):
                if 'OK' not in c.cmd(cmd, '->', 10):
                    raise RuntimeError(cmd)
        tx.cmd('hold on', 'hold st')
        for s in args.tx_rc:
            raw(tx, s)
        for s in args.rx_rc:
            raw(rx, s)
        rx.cmd('hold on', 'hold st')
        rx.cmd('hold off', 'hold st')
        r = run(tx, rx, args.n)
        f = lambda v: 'n/a' if v is None else f'{v:.1f}'
        print(f'{r["good"]}/{r["n"]} received, crc_err {r["crc_err"]}, hdr_err {r["hdr_err"]}, '
              f'tx errors {r["txerr"]}, rssi {f(r["rssi"])} dBm, snr {f(r["snr"])} dB')
    finally:
        for c in (tx, rx):   # back to defaults (set radio resets the chip), receiver held
            c.cmd(f'set radio {args.mhz},{args.bw:g},{args.sf},5', '->', 10)
            c.cmd('hold on', 'hold st')


if __name__ == '__main__':
    main()
