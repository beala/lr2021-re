#!/usr/bin/env python3
"""Multi-channel LoRa receiver: the LR2021 hops over a channel table by itself (PRAM blob
firmware/pram/scan.bin), runs CAD on each channel and drops into RX on the first one with a
preamble. See docs/pram-scanner.md.

By default the scan runs in the background (entry 4): one command starts it, the chip hops from
its own event loop with BUSY low, and only RxDone reaches the IRQ pin. The console's `scanwait`
sleeps on that pin, reads each packet and resumes the scan. --blocking uses the older entry 1, a
command the host re-issues every ~600 ms.

  python3 tools/scan.py listen --ch 902.5:910:0.5 --seconds 60          # 16 channels, SF7/62.5 kHz
  python3 tools/scan.py listen --ch 903:7,904:8,905:9,906:10            # one SF per channel
  python3 tools/scan.py bench --channels 20 --trials 30                 # XIAO sends on random channels

Channels: comma-separated MHz[:SF[:BW kHz]] entries, or start:stop:step in MHz. Defaults SF7,
62.5 kHz, CR 4/5. Each channel's RX preamble follows MeshCore's rule (32 symbols at SF7-8, 16
from SF9). `listen` prints whatever it receives: on a real channel that is other people's
traffic, so don't publish its output. `bench` transmits from the other board on 902.5-910.25 MHz
into the dummy loads; use a frequency and power you are allowed to use.
"""
import argparse
import os
import random
import re
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pram  # noqa: E402
from radio_ports import default_port  # noqa: E402

BLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'pram', 'scan.bin')
TAB = 0x807800                       # channel table in free RAM (docs/lr2021-cpu.md#memory)
MAX_CH = 32
BW_CODE = {31.25: 2, 62.5: 3, 125: 4, 250: 5, 500: 6}
# Datasheet CAD detect peaks per CAD length (symbols) and SF
PEAKS = {1: {5: 60, 6: 60, 7: 60, 8: 64, 9: 64, 10: 66, 11: 70, 12: 74},
         2: {5: 56, 6: 56, 7: 56, 8: 58, 9: 58, 10: 60, 11: 64, 12: 68},
         3: {5: 51, 6: 51, 7: 52, 8: 54, 9: 56, 10: 60, 11: 60, 12: 65},
         4: {5: 51, 6: 51, 7: 51, 8: 54, 9: 56, 10: 60, 11: 60, 12: 64}}
# CAD flags, byte 1 of SetLoraCadParams: bit 4 pbl_any (detect only preamble symbols), bits 3:0
# pnr_delta (8 = fast CAD, ends early when nothing is there)
CAD_PREAMBLE_ONLY, CAD_FAST = 0x10, 0x08
IRQ_PRE, IRQ_HDR, IRQ_CADDET, IRQ_HDR_CRC = 1 << 5, 1 << 6, 1 << 7, 1 << 9
IRQ_RXDONE, IRQ_TXDONE, IRQ_CADDONE, IRQ_TO, IRQ_CRC = 1 << 18, 1 << 19, 1 << 20, 1 << 21, 1 << 22
SCAN_IRQS = IRQ_PRE | IRQ_HDR | IRQ_CADDET | IRQ_RXDONE | IRQ_CADDONE | IRQ_TO | IRQ_CRC | IRQ_HDR_CRC
IRQ_DIO = {'xiao': 8, 'nicerf': 9}


def be32(v):
    return struct.pack('>I', v & 0xFFFFFFFF)


def ok(c, op, params=b'', nread=0):
    st, d = c.rc(op, nread, params)
    if st != 0:
        raise RuntimeError(f'rc {op:04X} {params.hex()}: st={st}')
    return d


def irq(c):
    return struct.unpack('>I', ok(c, 0x0117, nread=4))[0]


def parse_channels(spec, sf, bw):
    out = []
    for part in spec.split(','):
        f = part.split(':')
        if len(f) == 3 and float(f[2]) < 5:          # start:stop:step
            a, b, s = map(float, f)
            n = int(round((b - a) / s)) + 1
            out += [(round(a + i * s, 6), sf, bw) for i in range(n)]
        else:
            out.append((float(f[0]), int(f[1]) if len(f) > 1 else sf, float(f[2]) if len(f) > 2 else bw))
    if not 1 <= len(out) <= MAX_CH:
        raise SystemExit(f'1..{MAX_CH} channels, got {len(out)}')
    return out


def console_init(c, mhz, bw, sf, dbm=-9):
    for cmd in (f'set radio {mhz},{bw:g},{sf},5', f'set tx {dbm}'):
        if 'OK' not in c.cmd(cmd, '->', 10):
            raise RuntimeError(cmd)
    c.cmd('hold on', 'hold st')


def setup_scanner(c, name, chans, cad_sym=1):
    """Reset the radio, load the blob, write the channel table."""
    f0, sf0, bw0 = chans[0]
    console_init(c, f0, bw0, sf0)                    # set radio resets the chip (and clears PRAM)
    pram.load(c, BLOB)
    ok(c, 0x0128, b'\0')
    ok(c, 0x0206, b'\x03')                           # fallback FS: the synthesizer keeps running
    ok(c, 0x0209, b'\x01')                           # a detected preamble stops the RX timeout
    ok(c, 0x0115, bytes([IRQ_DIO[name]]) + be32(SCAN_IRQS))
    for i, (f, sf, bw) in enumerate(chans):
        ldro = 1 if (2 ** sf) / (bw * 1e3) > 16e-3 else 0
        pre = 32 if sf <= 8 else 16
        word1 = ((sf << 4) | BW_CODE[bw]) | ((1 << 4 | ldro) << 8) | (PEAKS[cad_sym][sf] << 16) | (pre << 24)
        for addr, val in ((TAB + 8 * i, int(round(f * 1e6))), (TAB + 8 * i + 4, word1)):
            if 'mw st=0' not in c.cmd(f'mw {addr:X} {val:X}', 'mw st'):
                raise RuntimeError('channel table write failed')
    if c.rc(0x012D, 4, b'\x02')[1] != b'SCN2':
        raise RuntimeError('scanner blob not active')


def scan_rounds(chans, cad_sym, budget_ms=600):
    """Rounds that keep one command well under RadioLib's 1 s BUSY limit."""
    cycle = sum(cad_sym * 2 ** sf / bw for _, sf, bw in chans) + 0.07 * len(chans)
    return max(1, int(budget_ms / cycle))


def scan_start(c, n, rounds, cad_sym=1, wait_sym=8, cad_flags=0):
    p = bytes([1, n, cad_sym, 1, wait_sym, cad_flags, 0, rounds >> 8, rounds & 0xFF])
    c.s.reset_input_buffer()
    c.s.write(('rc 012D 8 ' + ' '.join(f'{b:02X}' for b in p) + '\r').encode())


def scan_result(c, timeout=3.0):
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        buf += c.s.read(4096)
        if b'rc st=' in buf and buf.endswith(b'\n'):
            break
    m = re.search(r'rc st=(-?\d+)((?: [0-9A-F]{2})*)', buf.decode(errors='replace'))
    if not m:
        raise RuntimeError(f'no scan reply: {buf!r}')
    d = bytes(int(b, 16) for b in m[2].split())
    if int(m[1]) != 0 or len(d) < 6:
        return dict(st=int(m[1]), hit=0xFE, cads=0, falses=0)
    return dict(st=0, hit=d[0], rounds=d[1] << 8 | d[2], cads=d[3] << 8 | d[4], falses=d[5])


def read_packet(c, timeout=2.0):
    """After a hit the chip is already receiving: wait for RxDone, then read with `rxread`."""
    acc, t = 0, time.time()
    while time.time() - t < timeout:
        acc |= irq(c)
        if acc & (IRQ_RXDONE | IRQ_TO):
            break
    if not acc & IRQ_RXDONE:
        return acc, None
    m = re.search(r'len=(\d+) st=(-?\d+) snr=(-?[\d.]+) rssi=(-?[\d.]+) data=([0-9A-F]*)',
                  c.cmd('rxread', '-> >', 3))
    if not m:
        raise RuntimeError('rxread failed (console firmware too old?)')
    pkt = dict(data=bytes.fromhex(m[5]), snr=float(m[3]), rssi=float(m[4]), crc=not acc & IRQ_CRC)
    return acc, pkt


BG_STATE = ['off', 'cad', 'wait', 'rx', 'parked']
BG_KEYS = ['word0', 'cads', 'falses', 'rx_lost', 'packets', 'rounds', 'events', 'lost_irq', 'irq', 'irq_gate']


def bg(c, op, extra=b''):
    """Entry 4: 1 start (n, CAD symbols, preamble wait symbols), 3 resume, 2 status, 0 stop."""
    d = dict(zip(BG_KEYS, struct.unpack('<10I', ok(c, 0x012D, bytes([4, op]) + extra, 40))))
    w = d.pop('word0')
    d.update(state=BG_STATE[w & 0xFF] if (w & 0xFF) < len(BG_STATE) else w & 0xFF, last_hit=(w >> 8) & 0xFF,
             channel=(w >> 16) & 0xFF, hooked=bool(w >> 24))
    return d


def bg_start(c, name, chans, cad_sym, wait_sym, cad_flags=0):
    ok(c, 0x0128, b'\0')
    ok(c, 0x0115, bytes([IRQ_DIO[name]]) + be32(IRQ_RXDONE))    # only RxDone wakes the MCU
    irq(c)
    return bg(c, 1, bytes([len(chans), cad_sym, wait_sym, cad_flags]))


def bg_stop(c):
    s = bg(c, 0)
    ok(c, 0x0128, b'\0')
    return s


def scanwait(c, seconds, on_packet):
    """Console `scanwait`: the MCU sleeps until RxDone, reads the packet and resumes the scan."""
    c.s.reset_input_buffer()
    c.s.write(f'scanwait {int(seconds * 1000)}\r'.encode())   # 0: until 'x'
    buf, end = b'', time.time() + (seconds or 600) + 3
    while time.time() < end:
        buf += c.s.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.decode(errors='replace').strip()
            m = re.match(r'SCAN (\d+) ch=(\d+) len=(\d+) st=(-?\d+) snr=(-?[\d.]+) rssi=(-?[\d.]+) data=([0-9A-F]*)', line)
            if m:
                on_packet(dict(t=int(m[1]) / 1e3, ch=int(m[2]), st=int(m[4]), snr=float(m[5]), rssi=float(m[6]),
                               data=bytes.fromhex(m[7])))
            elif line.startswith('scanwait done'):
                return line
    raise RuntimeError('scanwait did not finish')


def bg_summary(s, seconds):
    return (f"{s['cads']} CADs in the background ({s['cads'] / seconds:.0f}/s), {s['rounds']} passes, "
            f"{s['falses']} false alarms recovered on-chip, {s['rx_lost']} lost after a preamble, "
            f"{s['packets']} packets, final state {s['state']} (last hit ch{s['last_hit']})")


def ports():
    return {'xiao': default_port('XIAO', '/dev/ttyACM0'), 'nicerf': default_port('RAK3401', '/dev/ttyACM1')}


def cmd_listen(args):
    chans = parse_channels(args.ch, args.sf, args.bw)
    name = args.board
    c = pram.Console(ports()[name])
    setup_scanner(c, name, chans, args.cad_sym)
    if not args.blocking:
        return listen_bg(c, name, chans, args)
    rounds = scan_rounds(chans, args.cad_sym)
    print(f'scanning {len(chans)} channels, {rounds} rounds per command; ctrl-c to stop')
    t_end = time.time() + args.seconds if args.seconds else None
    n_pkt = n_false = n_cmd = 0
    try:
        while t_end is None or time.time() < t_end:
            irq(c)
            scan_start(c, len(chans), rounds, args.cad_sym, args.wait_sym, args.cad_flags)
            r = scan_result(c)
            n_cmd += 1
            n_false += r['falses']
            if r['hit'] >= len(chans):
                continue
            acc, pkt = read_packet(c)
            f, sf, bw = chans[r['hit']]
            when = time.strftime('%H:%M:%S')
            if pkt is None:
                print(f'{when} ch{r["hit"]:2} {f:.3f} MHz SF{sf}: preamble, no packet (irq {acc:08X})')
                continue
            n_pkt += 1
            print(f'{when} ch{r["hit"]:2} {f:.3f} MHz SF{sf}/{bw:g}: {len(pkt["data"])} B rssi {pkt["rssi"]} '
                  f'snr {pkt["snr"]} crc {"ok" if pkt["crc"] else "BAD"} {pkt["data"].hex()}', flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        print(f'{n_pkt} packets, {n_false} false CAD alarms recovered on-chip, {n_cmd} scan commands')
        console_init(c, *chans[0][:1], chans[0][2], chans[0][1])


def listen_bg(c, name, chans, args):
    secs = args.seconds or 3600
    bg_start(c, name, chans, args.cad_sym, args.wait_sym, args.cad_flags)
    print(f'scanning {len(chans)} channels in the background for {secs:.0f} s; the MCU waits for RxDone')

    def show(p):
        f, sf, bw = chans[p['ch']] if p['ch'] < len(chans) else (0, 0, 0)
        print(f"{p['t']:8.2f} ch{p['ch']:2} {f:.3f} MHz SF{sf}/{bw:g}: {len(p['data'])} B rssi {p['rssi']} "
              f"snr {p['snr']} st {p['st']} {p['data'].hex()}", flush=True)
    try:
        print(scanwait(c, secs, show))
    finally:
        print(bg_summary(bg_stop(c), secs))
        console_init(c, chans[0][0], chans[0][2], chans[0][1])


def bench_bg(sc, tx, rx_name, chans, args):
    """Background scan: start it once, then the other board sends on random channels at random moments."""
    pre = 32 if args.sf <= 8 else 16
    random.seed(args.seed)
    plan = []
    for trial in range(args.trials):
        k = random.randrange(len(chans))
        plan.append((k, f'scan {trial:03d} ch{k:02d}'.encode(), random.uniform(0, 0.15)))
    period = 0.45
    got, res = [], {}
    bg_start(sc, rx_name, chans, args.cad_sym, args.wait_sym, args.cad_flags)
    t_start = time.time()
    th = threading.Thread(target=lambda: res.setdefault('line', scanwait(sc, 0, got.append)))
    try:
        th.start()
        time.sleep(0.5)
        t0 = time.time()
        for i, (k, payload, jitter) in enumerate(plan):
            ok(tx, 0x0128, b'\0')
            ok(tx, 0x0200, be32(int(chans[k][0] * 1e6)))
            ok(tx, 0x0221, bytes([0, pre, len(payload), 0x02]))
            ok(tx, 0x011F)
            ok(tx, 0x0002, payload)
            irq(tx)
            while time.time() < t0 + i * period + jitter:
                time.sleep(0.002)
            ok(tx, 0x020D, b'\0\0\0')
            t1 = time.time()
            while time.time() - t1 < 1 and not irq(tx) & IRQ_TXDONE:
                pass
        time.sleep(1)
        sc.s.write(b'x')                             # end scanwait
        th.join()
        if args.verbose:
            print('before stop:', bg(sc, 2))
    finally:
        secs = time.time() - t_start
        s = bg_stop(sc)
        for c in (sc, tx):
            console_init(c, chans[0][0], args.bw, args.sf)
    good = sum(1 for k, payload, _ in plan if any(p['data'] == payload and p['ch'] == k and p['st'] == 0 for p in got))
    other = [p for p in got if not any(p['data'] == payload for _, payload, _ in plan)]
    print(res.get('line'))
    print(bg_summary(s, secs))
    print(f'{len(chans)} channels, CAD {args.cad_sym} symbol(s), background: {good}/{len(plan)} packets caught on the '
          f'right channel and decoded, {len(other)} other')
    if got:
        r = sorted(p['rssi'] for p in got)
        print(f'RSSI {r[0]:.0f} to {r[-1]:.0f} dBm (median {r[len(r) // 2]:.0f}), CAD flags {args.cad_flags:#04x}')
    missed = [(k, payload.decode()) for k, payload, _ in plan
              if not any(p['data'] == payload and p['ch'] == k and p['st'] == 0 for p in got)]
    if missed:
        print('missed:', missed, 'received:', [(p['ch'], p['st'], p['data']) for p in got if p['data'] in [m[1].encode() for m in missed]])


def cmd_bench(args):
    chans = parse_channels(','.join(f'{902.5 + 0.25 * i}' for i in range(args.channels)), args.sf, args.bw)
    p = ports()
    rx_name = args.board
    tx_name = 'xiao' if rx_name == 'nicerf' else 'nicerf'
    sc, tx = pram.Console(p[rx_name]), pram.Console(p[tx_name])
    setup_scanner(sc, rx_name, chans, args.cad_sym)
    console_init(tx, chans[0][0], args.bw, args.sf)
    if not args.blocking:
        return bench_bg(sc, tx, rx_name, chans, args)
    rounds = scan_rounds(chans, args.cad_sym)
    pre = 32 if args.sf <= 8 else 16
    random.seed(args.seed)
    good = falses = 0
    misses = []
    try:
        for trial in range(args.trials):
            k = random.randrange(len(chans))
            payload = f'scan {trial:03d} ch{k:02d}'.encode()
            ok(tx, 0x0128, b'\0')                    # prepare the packet first, so only SetTx is
            ok(tx, 0x0200, be32(int(chans[k][0] * 1e6)))   # left to do once the scan runs
            ok(tx, 0x0221, bytes([0, pre, len(payload), 0x02]))
            ok(tx, 0x011F)
            ok(tx, 0x0002, payload)
            irq(tx)
            ok(sc, 0x0128, b'\0')
            irq(sc)
            scan_start(sc, len(chans), rounds, args.cad_sym, args.wait_sym, args.cad_flags)
            time.sleep(random.uniform(0, 0.15))      # random phase against the scan cycle
            ok(tx, 0x020D, b'\0\0\0')
            r = scan_result(sc)
            acc, pkt = read_packet(sc) if r['hit'] < len(chans) else (0, None)
            t0 = time.time()
            while time.time() - t0 < 3 and not irq(tx) & IRQ_TXDONE:
                pass
            falses += r['falses']
            if r['hit'] == k and pkt and pkt['data'] == payload and pkt['crc']:
                good += 1
            else:
                misses.append((k, r['hit'], f'{acc:08X}'))
    finally:
        for c in (sc, tx):
            console_init(c, chans[0][0], args.bw, args.sf)
    print(f'{len(chans)} channels, CAD {args.cad_sym} symbol(s): {good}/{args.trials} packets caught on the '
          f'right channel and decoded, {falses} false alarms recovered on-chip')
    if misses:
        print('misses (tx channel, scan result, irq):', misses)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--board', default='nicerf', choices=IRQ_DIO, help='scanning board')
    ap.add_argument('--sf', type=int, default=7)
    ap.add_argument('--bw', type=float, default=62.5, choices=BW_CODE)
    ap.add_argument('--cad-sym', type=int, default=1, choices=PEAKS, help='CAD length in symbols')
    ap.add_argument('--cad-flags', type=lambda v: int(v, 0), default=CAD_PREAMBLE_ONLY,
                    help='SetLoraCadParams byte 1: 0x10 preamble only (default), 0x08 fast CAD, 0x18 both, 0 neither')
    ap.add_argument('--wait-sym', type=int, default=8, help='symbols to wait for a preamble after a CAD hit')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--blocking', action='store_true', help='re-issued scan command (entry 1) instead of the background scan')
    sub = ap.add_subparsers(dest='cmd', required=True)
    lp = sub.add_parser('listen')
    lp.add_argument('--ch', required=True, help='MHz[:SF[:BW]],... or start:stop:step')
    lp.add_argument('--seconds', type=float, default=0, help='0 = until ctrl-c')
    bp = sub.add_parser('bench')
    bp.add_argument('--channels', type=int, default=20)
    bp.add_argument('--trials', type=int, default=30)
    bp.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    {'listen': cmd_listen, 'bench': cmd_bench}[args.cmd](args)


if __name__ == '__main__':
    main()
