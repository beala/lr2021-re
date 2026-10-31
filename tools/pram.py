"""Load and call the custom PRAM commands in firmware/pram (console firmware on the board).

  python3 tools/pram.py [--port P] load [blob.bin]          upload, verify, activate
  python3 tools/pram.py [--port P] hello
  python3 tools/pram.py [--port P] sweep START_MHZ STOP_MHZ STEP_KHZ [--dwell N] [--avg N] [--out file.csv] [--plot file.png]
  python3 tools/pram.py [--port P] fft [--frames N] [--check] [--plot file.png]   on-chip spectrum of the current channel
  python3 tools/pram.py [--port P] waterfall [--seconds S] [--nseg N] [--save f.npz] [--plot f.png]
                                                         capture + FFT in one on-chip command per frame

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
DEFAULT_BLOB = os.path.join(HERE, '..', 'firmware', 'pram', 'pram.bin')
PRAM_HOST = 0x801000      # write-only window into PRAM
PRAM_CPU = 0x101000       # readable copy
OUT_BUF = 0x101A00        # sweep results live in PRAM (0x804000 is DMA-owned during RX)
CAP_BUF = 0x804000        # raw capture (source 0, int8 I/Q)
FFT_ACC = 0x805400        # 256 x u32 power sums written by the FFT command
CAP_WORDS = 1280          # 5 KB = 10 segments of 256 samples


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


def capture(con, words=CAP_WORDS):
    """One-shot raw capture (source 0, int8 I/Q) into 0x804000, then stop: chip left in standby."""
    con.rc(0x0128, 0, b'\0')
    con.rc(0x0207, 0, b'\x0b')
    con.rc(0x0290, 0, bytes([0xFF, 0, 0, 0, 0, 0xD0, 0, 0, words >> 8, words & 0xFF]))
    con.rc(0x020C, 0, b'\xff\xff\xff')
    end = time.time() + 1
    while time.time() < end:
        st, d = con.rc(0x0291, 2)
        if st == 0 and int.from_bytes(d, 'big') >= 4 * words:
            break
    con.rc(0x0290, 0, bytes([0xFF, 0, 0, 0, 0, 0]))
    con.rc(0x0128, 0, b'\0')


def chip_fft(con, nseg=10, skip=0):
    t0 = time.time()
    st, d = con.rc(0x012D, 2, bytes([7, nseg, skip]))
    if st != 0:
        raise RuntimeError(f'fft failed: st={st}')
    t1 = time.time()
    acc = con.mr(FFT_ACC, 256)
    return acc, t1 - t0, time.time() - t1


def host_fft(raw, nseg=10, skip=0):
    """The same windowed 256-point FFT power sum, in floating point, from the raw capture bytes."""
    import numpy as np
    b = np.frombuffer(raw, np.int8).astype(float)
    z = b[0::2] + 1j * b[1::2]
    win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(256) / 256)
    acc = np.zeros(256)
    for s in range(skip, skip + nseg):
        acc += np.abs(np.fft.fft(z[s * 256:(s + 1) * 256] * win)) ** 2
    return acc


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
    p = sub.add_parser('waterfall')
    p.add_argument('--seconds', type=float, default=10)
    p.add_argument('--nseg', type=int, default=10, help='256-sample segments averaged per frame (1..10)')
    p.add_argument('--save'); p.add_argument('--plot')
    p = sub.add_parser('fft')
    p.add_argument('--frames', type=int, default=1)
    p.add_argument('--check', action='store_true', help='also read the raw capture and compare with numpy')
    p.add_argument('--plot')
    args = ap.parse_args()
    con = Console(args.port)
    if args.what == 'load':
        sys.exit(0 if load(con, args.blob) else 1)
    if args.what == 'waterfall':
        import numpy as np
        con.cmd('hold on', 'hold st')
        con.rc(0x0128, 0, b'\0')
        con.rc(0x0207, 0, b'\x0b')                  # raw packet type, once
        rows, times, chip_ms = [], [], []
        t0 = time.time()
        while time.time() - t0 < args.seconds:
            out = con.cmd(f'rc 012D 4 09 {args.nseg:02X} 00', 'us=', 5)
            m = re.search(r'rc st=(-?\d+)((?: [0-9A-F]{2})*) us=(\d+)', out)
            if not m or m[1] != '0':
                raise RuntimeError(f'capture+fft failed: {out!r}')
            rows.append(np.fft.fftshift(np.array(con.mr(FFT_ACC, 256), float)))
            times.append(time.time() - t0); chip_ms.append(int(m[3]) / 1000)
        dt = times[-1] / max(len(times) - 1, 1)
        print(f'{len(rows)} frames in {times[-1]:.1f} s = {1 / dt:.1f} frames/s; on-chip capture+FFT {np.median(chip_ms):.1f} ms per frame')
        freq = np.fft.fftshift(np.fft.fftfreq(256, 1 / 250e3))
        if args.save:
            np.savez(args.save, rows=np.array(rows), times=np.array(times), freq=freq)
        if args.plot:
            import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
            db = 10 * np.log10(np.array(rows) + 1)
            plt.figure(figsize=(9, 6)); plt.pcolormesh(freq / 1e3, times, db, shading='nearest', cmap='Blues')
            plt.gca().invert_yaxis(); plt.xlabel('offset (kHz)'); plt.ylabel('time (s)'); plt.colorbar(label='dB')
            plt.tight_layout(); plt.savefig(args.plot, dpi=100)
        return
    if args.what == 'fft':
        import numpy as np
        con.cmd('hold on', 'hold st')
        freq = np.fft.fftshift(np.fft.fftfreq(256, 1 / 250e3))
        frames = []
        for k in range(args.frames):
            t0 = time.time(); capture(con); tc = time.time() - t0
            acc, tf, tr = chip_fft(con)
            p = np.array(acc, float)
            frames.append(p)
            print(f'frame {k}: capture {tc * 1e3:.0f} ms, on-chip FFT {tf * 1e3:.0f} ms, readback {tr * 1e3:.0f} ms; '
                  f'peak {freq[np.argmax(np.fft.fftshift(p))] / 1e3:+.1f} kHz')
            if args.check:
                words = con.mr(CAP_BUF, CAP_WORDS)
                raw = b''.join(struct.pack('<I', w) for w in words)
                h = host_fft(raw)
                a, b2 = 10 * np.log10(np.fft.fftshift(p) + 1), 10 * np.log10(np.fft.fftshift(h) + 1)
                off = np.median(a - b2)
                print(f'  vs numpy on the same capture: peak bin chip {np.argmax(p)} / host {np.argmax(h)}, '
                      f'correlation {np.corrcoef(a, b2)[0, 1]:.4f}, rms dB difference {np.sqrt(np.mean((a - b2 - off) ** 2)):.2f}')
        if args.plot:
            import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
            plt.figure(figsize=(10, 4))
            for p in frames:
                plt.plot(freq / 1e3, 10 * np.log10(np.fft.fftshift(p) + 1), lw=.8)
            plt.xlabel('offset (kHz)'); plt.ylabel('power (dB, arbitrary)'); plt.grid(alpha=.3); plt.tight_layout()
            plt.savefig(args.plot, dpi=100)
        return
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
