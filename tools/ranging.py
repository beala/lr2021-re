#!/usr/bin/env python3
"""RTToF ranging between two LR2021 boards running the console firmware.

  python3 tools/ranging.py [--n 200] [--sf 9] [--mhz 908] [--dbm -9] [--no-patch] [--out results.csv]

Manager = NiceRF board (RAK3401), subordinate = XIAO; swap with --manager/--subordinate.
Everything goes through the console's raw `rc` command, no firmware changes. Both radios
are reset with `set radio`, Semtech's PRAM patch is loaded on both (unless --no-patch),
then the manager runs --n exchanges and prints the distance and RSSI statistics.

The patch is not stored in this repo. It is read from Semtech's lr20xx_pram_lr2021.h
(Lora-net/usp, Clear BSD), downloaded once into ~/.cache/lr2021-re/, or from --patch-header.

This transmits on --mhz from both boards. Use a frequency and power you are allowed to use.
"""
import argparse
import os
import re
import statistics
import struct
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pram  # noqa: E402
from radio_ports import default_port  # noqa: E402

PATCH_URL = ('https://raw.githubusercontent.com/Lora-net/usp/v1.2.1/'
             'smtc_rac_lib/radio_drivers/lr20xx_driver/inc/lr20xx_pram_lr2021.h')
PATCH_VERSION = 0x00031304
BW_CODE = {250: 0x05, 500: 0x06, 1000: 0x07}
# Recommended TxRxDelay for 600 MHz-2 GHz, BW 500 kHz, SF5..SF12: Semtech usp v1.2.1,
# examples/main_examples/ranging_demo/app_ranging_hopping.c (Clear BSD). Calibrated on Semtech's
# boards; other boards need their own offset.
DELAY_BW500 = {5: 20628, 6: 20609, 7: 20568, 8: 20484, 9: 20305, 10: 19971, 11: 20295, 12: 20298}
ADDR = 0x32101222
IRQ_REQ_VALID, IRQ_RESP_DONE, IRQ_REQ_DIS = 1 << 2, 1 << 28, 1 << 29
IRQ_EXCH_VALID, IRQ_RNG_TIMEOUT, IRQ_TIMEOUT = 1 << 30, 1 << 31, 1 << 21
RNG_IRQS = IRQ_REQ_VALID | IRQ_RESP_DONE | IRQ_REQ_DIS | IRQ_EXCH_VALID | IRQ_RNG_TIMEOUT | IRQ_TIMEOUT
IRQ_DIO = {'xiao': 8, 'nicerf': 9}


def patch_blob(header):
    """Semtech's patch words from lr20xx_pram_lr2021.h, as a little-endian .bin in the cache."""
    cache = os.path.expanduser('~/.cache/lr2021-re')
    if header is None:
        header = os.path.join(cache, 'lr20xx_pram_lr2021.h')
        if not os.path.exists(header):
            os.makedirs(cache, exist_ok=True)
            print(f'downloading {PATCH_URL}')
            urllib.request.urlretrieve(PATCH_URL, header)
    text = open(header).read()
    body = text[text.index('pram_lr2021['):]
    body = body[body.index('{') + 1:body.index('}')]
    words = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body)]
    if words[0] != 0x600DB002:
        raise RuntimeError(f'{header}: no PRAM magic')
    if words[1] != PATCH_VERSION:
        print(f'warning: patch version {words[1]:08X}, tested with {PATCH_VERSION:08X}')
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, 'semtech_pram_lr2021.bin')
    with open(path, 'wb') as f:
        f.write(struct.pack(f'<{len(words)}I', *words))
    return path


def be32(v):
    return struct.pack('>I', v & 0xFFFFFFFF)


def ok(con, op, params=b'', nread=0, what=''):
    st, d = con.rc(op, nread, params)
    if st != 0:
        raise RuntimeError(f'{what or hex(op)} failed st={st}')
    return d


def irq(con):
    return struct.unpack('>I', ok(con, 0x0117, nread=4, what='GetAndClearIrqStatus'))[0]


def setup(con, name, args, role, blob):
    for c in (f'set radio {args.mhz},{args.bw},{args.sf},5', f'set tx {args.dbm}'):
        out = con.cmd(c, '->', 10)
        if 'OK' not in out:
            raise RuntimeError(f'{name}: {c}: {out!r}')
    if blob:
        pram.load(con, blob)
        magic, ver = con.mr(0x800FF8, 2)
        print(f'{name}: patch installed, magic {magic:08X} version {ver:08X}')
    else:
        con.cmd('hold on', 'hold st')
    ok(con, 0x0128, b'\x00', what='SetStandby RC')
    ok(con, 0x0207, b'\x04', what='SetPacketType RTToF')
    ok(con, 0x0220, bytes([(args.sf << 4) | BW_CODE[args.bw], 1 << 4]), what='SetLoraModulationParams')
    ok(con, 0x0221, bytes([0, 12, 0, 0x02]), what='SetLoraPacketParams')   # preamble 12, explicit, CRC
    ok(con, 0x0223, b'\x12', what='SetLoraSyncword')
    ok(con, 0x0115, bytes([IRQ_DIO[name]]) + be32(RNG_IRQS), what='SetDioIrqConfig')
    ok(con, 0x027B, be32(args.delay), what='SetRangingTxRxDelay')
    ok(con, 0x027C, bytes([args.nsym & 0x3F]), what='SetRangingParams')
    if role == 'subordinate':
        ok(con, 0x0278, be32(ADDR) + b'\x04', what='SetRangingAddr')
    else:
        ok(con, 0x0279, be32(ADDR), what='SetRangingReqAddr')
    ok(con, 0x020A, what='ResetRxStats')
    irq(con)


def stats(con):
    d = ok(con, 0x027D, nread=10, what='GetRangingStats')
    return dict(zip(('exch_valid', 'req_valid', 'resp_done', 'timeout', 'req_discarded'), struct.unpack('>5H', d)))


def s24(v):
    return v - (1 << 24) if v & 0x800000 else v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--manager', default='nicerf', choices=IRQ_DIO)
    ap.add_argument('--n', type=int, default=200, help='exchanges')
    ap.add_argument('--mhz', type=float, default=908.0)
    ap.add_argument('--sf', type=int, default=9, choices=range(5, 13))
    ap.add_argument('--bw', type=int, default=500, choices=BW_CODE)
    ap.add_argument('--dbm', type=int, default=-9, help='console TX drive level, both boards')
    ap.add_argument('--nsym', type=int, default=15, help='symbols in the response')
    ap.add_argument('--delay', type=int, help='TxRxDelay (default: Semtech table for BW 500)')
    ap.add_argument('--no-patch', action='store_true', help="don't load Semtech's PRAM patch")
    ap.add_argument('--patch-header', help='local lr20xx_pram_lr2021.h instead of downloading')
    ap.add_argument('--out', help='CSV of every exchange')
    args = ap.parse_args()
    if args.delay is None:
        args.delay = DELAY_BW500[args.sf] if args.bw == 500 else 0

    blob = None if args.no_patch else patch_blob(args.patch_header)
    ports = {'xiao': default_port('XIAO', '/dev/ttyACM0'), 'nicerf': default_port('RAK3401', '/dev/ttyACM1')}
    sub_name = 'xiao' if args.manager == 'nicerf' else 'nicerf'
    sub, mgr = pram.Console(ports[sub_name]), pram.Console(ports[args.manager])
    setup(sub, sub_name, args, 'subordinate', blob)
    setup(mgr, args.manager, args, 'manager', blob)
    ok(sub, 0x020C, b'\xFF\xFF\xFF', what='SetRx continuous')
    time.sleep(0.05)

    rows, missed = [], 0
    try:
        for i in range(args.n):
            ok(mgr, 0x020D, b'\x00\x00\x00', what='SetTx')
            t0, f = time.time(), 0
            while time.time() - t0 < 1.0:
                f = irq(mgr)
                if f & (IRQ_EXCH_VALID | IRQ_RNG_TIMEOUT | IRQ_TIMEOUT):
                    break
            if not f & IRQ_EXCH_VALID:
                missed += 1
                if missed <= 3:
                    print(f'exchange {i}: no result, irq {f:08X}')
                continue
            d = ok(mgr, 0x027A, b'\x00', nread=4, what='GetRangingResult')
            g = ok(mgr, 0x027A, b'\x02', nread=2, what='GetRangingResult gain')
            raw = s24(d[0] << 16 | d[1] << 8 | d[2])
            meters = raw * 150 / (4096 * args.bw / 1000)      # datasheet: Rng * 150 / (2^12 * BW_MHz)
            rows.append((i, raw, meters, -(d[3] >> 1), g[0]))
        print('manager    ', stats(mgr))
        print('subordinate', stats(sub))
    finally:
        for c in (sub, mgr):
            c.rc(0x0128, 0, b'\x00')

    if not rows:
        print('no valid exchanges')
        return
    raws, ms = [r[1] for r in rows], [r[2] for r in rows]
    med = statistics.median
    print(f'{len(rows)}/{args.n} valid: SF{args.sf} BW{args.bw} {args.mhz} MHz, delay {args.delay}, '
          f'patch {"off" if args.no_patch else "on"}')
    print(f'raw     median {med(raws)}  sd {statistics.pstdev(raws):.1f}  min {min(raws)}  max {max(raws)}')
    print(f'meters  median {med(ms):.2f}  mean {statistics.mean(ms):.2f}  sd {statistics.pstdev(ms):.2f}')
    print(f'rssi    median {med(r[3] for r in rows)} dBm, AGC gain steps {sorted(set(r[4] for r in rows))}')
    if args.out:
        with open(args.out, 'w') as fh:
            fh.write('i,raw,meters,rssi_dbm,gain_step\n')
            for r in rows:
                fh.write(f'{r[0]},{r[1]},{r[2]:.3f},{r[3]},{r[4]}\n')


if __name__ == '__main__':
    main()
