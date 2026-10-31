"""Load and call the custom PRAM commands in firmware/pram (console firmware on the board).

  python3 tools/pram.py [--port P] load [blob.bin]          upload, verify, activate
  python3 tools/pram.py [--port P] hello
  python3 tools/pram.py [--port P] sweep START_MHZ STOP_MHZ STEP_KHZ [--dwell N] [--avg N] [--out file.csv] [--plot file.png]

The sweep keeps the radio in LoRa RX and retunes on the chip (ROM SetRfFrequency handler)
between instant-RSSI readings; results come back through RAM 0x804000. Receive only.
"""
import argparse
import os
import re
import struct
import sys
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radio_ports import default_port

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BLOB = os.path.join(HERE, '..', 'firmware', 'pram', 'sweep.bin')
PRAM_HOST = 0x801000      # write-only window into PRAM
PRAM_CPU = 0x101000       # readable copy
OUT_BUF = 0x101400        # results live in PRAM (0x804000 is DMA-owned during RX)


class Console:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.05)
        time.sleep(0.2)
        self.s.write(b'\r')
        time.sleep(0.2)
        self.s.read(1 << 16)

    def cmd(self, text, until, timeout=5.0):
        self.s.reset_input_buffer()
        self.s.write((text + '\r').encode())
        buf, end = b'', time.time() + timeout
        while time.time() < end:
            buf += self.s.read(4096)
            if until.encode() in buf and buf.endswith(b'\n'):
                break
        return buf.decode(errors='replace')

    def rc(self, op, nread, params=b'', timeout=5.0):
        text = f'rc {op:04X} {nread}' + ''.join(f' {b:02X}' for b in params)
        out = self.cmd(text, 'rc st=', timeout)
        m = re.search(r'rc st=(-?\d+)((?: [0-9A-F]{2})*)', out)
        if not m:
            raise RuntimeError(f'no reply to {text!r}: {out!r}')
        return int(m[1]), bytes(int(b, 16) for b in m[2].split())

    def mr(self, addr, words):
        out = self.cmd(f'mr {addr:X} {words}', 'mr done', 10)
        vals = []
        for line in out.splitlines():
            m = re.match(r'^([0-9A-F]{6}):((?: [0-9A-F]{8})+)', line)
            if m:
                vals += [int(w, 16) for w in m[2].split()]
        return vals[:words]


def load(con, path):
    blob = open(path, 'rb').read()
    blob += b'\0' * (-len(blob) % 4)
    words = struct.unpack(f'<{len(blob) // 4}I', blob)
    print(f'{path}: {len(words)} words')
    con.cmd('hold on', 'hold st')
    con.rc(0x0128, 0, b'\0')                      # standby RC
    for i, w in enumerate(words):
        out = con.cmd(f'mw {PRAM_HOST + 4 * i:X} {w:X}', 'mw st=', 2)
        if 'mw st=0' not in out:
            raise RuntimeError(f'write {i} failed: {out!r}')
    # Activate first: pram_check switches the bank mapping (aux 0x80000008); reading 0x101000
    # before that stalls the chip.
    st, _ = con.rc(0x012D, 0, b'\0')
    print(f'ActivatePram st={st}')
    back = con.mr(PRAM_CPU, len(words))
    if list(back) != list(words):
        bad = [i for i, (a, b) in enumerate(zip(back, words)) if a != b]
        raise RuntimeError(f'readback mismatch at words {bad[:8]}: got {[hex(v) for v in back[:4]]}')
    print('verified')
    return st == 0


def sweep(con, start_hz, step_hz, n, dwell, avg):
    params = struct.pack('>IiHHH', start_hz, step_hz, n, dwell, avg)
    t0 = time.time()
    st, data = con.rc(0x012D, 2, b'\x01' + params, timeout=30)
    dt = time.time() - t0
    if st != 0:
        raise RuntimeError(f'sweep failed: st={st}')
    done = struct.unpack('>H', data)[0]
    raw = con.mr(OUT_BUF, (done + 1) // 2)
    vals = []
    for w in raw:  # little-endian u16 pairs inside each word
        vals += [w & 0xFFFF, w >> 16]
    vals = vals[:done]
    dbm = [-v / (2 * avg) for v in vals]
    return [start_hz + i * step_hz for i in range(done)], dbm, dt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default=default_port('XIAO', '/dev/ttyACM0'))
    sub = ap.add_subparsers(dest='what', required=True)
    p = sub.add_parser('load'); p.add_argument('blob', nargs='?', default=DEFAULT_BLOB)
    sub.add_parser('hello')
    p = sub.add_parser('sweep')
    p.add_argument('start', type=float); p.add_argument('stop', type=float); p.add_argument('step_khz', type=float)
    p.add_argument('--dwell', type=int, default=1000, help='settle time after each retune, x16 loop iterations (1000 ~ 1.4 ms)')
    p.add_argument('--avg', type=int, default=4, help='RSSI readings summed per point')
    p.add_argument('--out'); p.add_argument('--plot')
    args = ap.parse_args()
    con = Console(args.port)
    if args.what == 'load':
        sys.exit(0 if load(con, args.blob) else 1)
    if args.what == 'hello':
        st, data = con.rc(0x012D, 6, b'\x02\xAB')
        print(f'st={st} data={data.hex()} ({data[:4]!r}, param len {data[4] if len(data) > 4 else "?"})')
        return
    con.cmd('hold on', 'hold st')
    con.rc(0x020C, 0, b'\xff\xff\xff')          # continuous RX (an error just means it already is)
    start, stop, step = int(args.start * 1e6), int(args.stop * 1e6), int(args.step_khz * 1e3)
    n = (stop - start) // step + 1
    freqs, dbm, dt = [], [], 0.0
    for chunk in range(0, n, 400):                   # keep each command under RadioLib's 1 s BUSY timeout
        f, d, t = sweep(con, start + chunk * step, step, min(400, n - chunk), args.dwell, args.avg)
        freqs += f; dbm += d; dt += t
    print(f'{len(freqs)} points in {dt:.2f} s ({dt / max(len(freqs), 1) * 1e3:.2f} ms/point incl. readback)')
    top = sorted(zip(dbm, freqs), reverse=True)[:5]
    print('strongest:', ', '.join(f'{f / 1e6:.4f} MHz {d:.1f} dBm' for d, f in top))
    if args.out:
        with open(args.out, 'w') as fh:
            fh.write('freq_hz,rssi_dbm\n' + ''.join(f'{f},{d}\n' for f, d in zip(freqs, dbm)))
    if args.plot:
        import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
        plt.figure(figsize=(11, 4)); plt.plot([f / 1e6 for f in freqs], dbm, lw=0.8)
        plt.xlabel('MHz'); plt.ylabel('RSSI (dBm)'); plt.grid(alpha=.3); plt.tight_layout(); plt.savefig(args.plot, dpi=100)


if __name__ == '__main__':
    main()
