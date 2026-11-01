#!/usr/bin/env python3
"""APRS (AFSK 1200) demodulated by the LR2021 itself, in the background: a PRAM blob
(firmware/pram/demod.bin) takes an interrupt each time the radio's RX FIFO fills a buffer of raw
I/Q, decodes AX.25 frames on the chip, and raises RxDone on the IRQ line for each good frame. The
MCU waits for that line like for any received packet, then reads only the frame. See
docs/pram-demod.md.

  python3 tools/aprs_rx.py listen --seconds 60          # print frames as the chip decodes them
  python3 tools/aprs_rx.py bench --trials 20            # the other board sends frames (tools/pll.py)
  python3 tools/aprs_rx.py iq                           # debug: CW tone through the buffer switching

The receiving board runs the console firmware (firmware/src/console, command `aprswait`).
`bench` and `iq` transmit from the other board (default 908 MHz, -9 dBm): use a frequency, power
and mode you are allowed to use, into dummy loads on the bench. `listen` prints whatever is on
the channel; other people's traffic isn't something to publish.
"""
import argparse
import os
import random
import struct
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'bench'))
import pram  # noqa: E402
import aprs  # noqa: E402
import pll  # noqa: E402
from radio_ports import default_port  # noqa: E402

BLOB = os.path.join(HERE, '..', 'firmware', 'pram', 'demod.bin')
BUF_A = 0x807800                       # FIFO buffers in free RAM (docs/lr2021-cpu.md#memory)
OUT = 0x101A00                         # frame / debug output in PRAM
T = 480                                # FIFO size = bytes per buffer (0.96 ms at 250 kS/s)
STAT = ['bufs', 'max_bytes', 'vector_base', 'isr_max', 'overruns', 'samples', 'flags', 'frames',
        'bad_fcs', 'lost', 'out_len']


def ok(c, op, params=b'', nread=0):
    st, d = c.rc(op, nread, params)
    if st != 0:
        raise RuntimeError(f'rc {op:04X} {params.hex()} st={st}')
    return d


def setup(port, mhz):
    c = pram.Console(port)
    if 'OK' not in c.cmd(f'set radio {mhz},62.5,7,5', '->', 10):
        raise RuntimeError('set radio failed')
    c.cmd('hold on', 'hold st')
    pram.load(c, BLOB)
    if c.rc(0x012D, 4, b'\x02')[1] != b'DEM1':
        raise RuntimeError('blob did not answer')
    return c


def stream_start(c):
    """Raw I/Q into the RX FIFO, placed in free RAM (docs/raw-iq.md#streaming-through-the-rx-fifo-hw)."""
    ok(c, 0x0128, b'\0')                                       # standby
    ok(c, 0x0207, b'\x0b')                                     # packet type 0x0B (raw)
    ok(c, 0x0290, bytes([0xFF, 0, 0, 0, 0, 0x08, 0, 0, 0, 0, 1]))   # source 0, FIFO on
    pll.mw(c, 0xF30028, BUF_A - 0x800000)
    pll.mw(c, 0xF30030, T)
    ok(c, 0x0114, b'\x3f\x3f')
    ok(c, 0x020C, b'\xff\xff\xff')                             # RX, no timeout
    ok(c, 0x011E)
    ok(c, 0x0114, b'\x3f\x3f')


def bg(c, op, extra=b''):
    """Entry 6: 1 start, 0 stop, 2 status, 3 drop output bytes."""
    return dict(zip(STAT, struct.unpack('<11I', ok(c, 0x012D, bytes([6, op]) + extra, 44))))


def start(c, debug=False):
    stream_start(c)
    ok(c, 0x0117, nread=4)                                     # clear the host IRQs
    return bg(c, 1, struct.pack('>HB', T, 2 if debug else 0))


def stop(c):
    s = bg(c, 0)
    ok(c, 0x0128, b'\0')
    return s


def wait(c, seconds, on_frame):
    """Console `aprswait`: the MCU sleeps until RxDone, then prints the frames."""
    c.s.reset_input_buffer()
    c.s.write(f'aprswait {int(seconds * 1000)}\r'.encode())
    buf, end = b'', time.time() + seconds + 3
    while time.time() < end:
        buf += c.s.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.decode(errors='replace').strip()
            if line.startswith('APRS '):
                _, ms, hx = line.split()
                on_frame(int(ms) / 1e3, bytes.fromhex(hx))
            elif line.startswith('aprswait done'):
                return line
    raise RuntimeError('aprswait did not finish')


def show(f):
    try:
        return aprs.show(f)
    except Exception:
        return f.hex()


def fmt(s):
    return (f"{s['bufs']} buffers ({s['samples'] / 31250:.1f} s) demodulated in the background, FIFO overruns "
            f"{s['overruns']}, handler time up to {s['isr_max'] / 500:.2f} ms of 0.96, HDLC flags {s['flags']}, "
            f"bad FCS {s['bad_fcs']}")


def cmd_listen(args):
    c = setup(args.port, args.mhz)
    start(c)
    print(f'listening on {args.mhz} MHz for {args.seconds:.0f} s (decoding on the chip, MCU waits for RxDone)')
    line = wait(c, args.seconds, lambda t, f: print(f'{t:7.2f}  {show(f)}', flush=True))
    print(line)
    print(fmt(stop(c)))


def cmd_bench(args):
    rx = setup(args.port, args.mhz)
    tx = pll.setup(args.tx_port, args.mhz, args.dbm)
    start(rx)
    got, sent = [], []
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('line', wait(rx, args.trials * 2.0 + 2, lambda t, f: got.append(show(f)))))
    pll.carrier_on(tx, args.mhz)
    try:
        th.start()
        time.sleep(0.5)
        rng = random.Random(args.seed)
        for i in range(args.trials):
            info = f'>LR2021 on-chip RX {i:03d} ' + ''.join(rng.choice('ABCDEFGHJKLMNPQRSTUVWXYZ23456789') for _ in range(8))
            tb = aprs.tones(aprs.frame('N0CALL', 'APZ001', info), preamble_flags=args.flags)
            by = bytearray((len(tb) + 7) // 8)
            for j, b in enumerate(tb):
                if b:
                    by[j >> 3] |= 1 << (j & 7)
            pll.write_table(tx, by)
            pll.afsk(tx, len(tb), args.dev)
            sent.append(f'N0CALL>APZ001:{info}')
        th.join()
    finally:
        pll.carrier_off(tx)
        s = stop(rx)
    hits = sum(1 for x in sent if x in got)
    extra = [g for g in got if g not in sent]
    print(res.get('line'))
    print(fmt(s))
    print(f'{hits}/{len(sent)} frames decoded on the chip, {len(extra)} other')
    for g in extra:
        print('  other:', g)


def cmd_iq(args):
    rx = setup(args.port, args.mhz)
    tx = pll.setup(args.tx_port, args.mhz, args.dbm)
    try:
        ok(tx, 0x0128, b'\0'); ok(tx, 0x0200, pll.be32(int(round((args.mhz + args.khz / 1e3) * 1e6)))); ok(tx, 0x020E, b'\x02')
        start(rx, debug=True)
        time.sleep(0.2)
        s = stop(rx)
    finally:
        pll.carrier_off(tx)
    w = rx.mr(OUT, 128)
    raw = np.frombuffer(b''.join(struct.pack('<I', x) for x in w), np.int8).astype(float)
    z = raw[0::2] + 1j * raw[1::2]
    dp = np.angle(z[1:] * np.conj(z[:-1]))
    step = np.median(dp)
    jump = dp - step
    print(f'{len(z)} decimated samples (31.25 kS/s, ~{len(z) * 8 * 2 / T:.0f} buffer switches; {s["bufs"]} buffers in all), '
          f'tone {step / 2 / np.pi * 31.25:.3f} kHz, |z| {np.abs(z).mean():.1f}')
    print(f'phase step per sample: rms deviation {jump.std():.3f} rad, largest {np.abs(jump).max():.2f} rad '
          f'(a lost input sample = {step / 8:.3f} rad)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--board', choices=['xiao', 'nicerf'], default='xiao', help='the receiving board')
    ap.add_argument('--mhz', type=float, default=908.0)
    ap.add_argument('--dbm', type=int, default=-9, help='transmit power of the other board (bench, iq)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('listen')
    a.add_argument('--seconds', type=float, default=60)
    b = sub.add_parser('bench')
    b.add_argument('--trials', type=int, default=10)
    b.add_argument('--dev', type=float, default=3000, help='peak deviation, Hz')
    b.add_argument('--flags', type=int, default=30, help='preamble flags')
    b.add_argument('--seed', type=int, default=1)
    q = sub.add_parser('iq')
    q.add_argument('--khz', type=float, default=2.0, help='tone offset')
    args = ap.parse_args()
    xiao, nicerf = default_port('XIAO', '/dev/ttyACM0'), default_port('RAK3401', '/dev/ttyACM1')
    args.port, args.tx_port = (xiao, nicerf) if args.board == 'xiao' else (nicerf, xiao)
    {'listen': cmd_listen, 'bench': cmd_bench, 'iq': cmd_iq}[args.cmd](args)


if __name__ == '__main__':
    main()
