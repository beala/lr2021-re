#!/usr/bin/env python3
"""Write text on a waterfall with the LR2021's CW carrier, drawn by a PRAM blob
(firmware/pram/text.bin, docs/pram-waterfall-text.md), and record it with an RTL-SDR.

  python3 tools/waterfall_text.py "AB Labs LLC" --png wf.png
  python3 tools/waterfall_text.py "HELLO" --spacing 3 --row-ms 600 --mhz 915

The chip hops one carrier between pixel positions: each font row is one call, the text appears
top row first, one pixel column every --spacing kHz starting --offset kHz above the carrier (the
carrier itself is the park frequency for unlit pixels, kept outside the picture). Transmits at
--dbm on --mhz + --offset: use a frequency and power you are allowed to use.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pram  # noqa: E402
from radio_ports import default_port  # noqa: E402

BLOB = os.path.join(HERE, '..', 'firmware', 'pram', 'text.bin')
TEXT = 0x807800
CLK = 32.0e6                       # CPU clock in TX [HW]
FS = 1.024e6                       # RTL-SDR sample rate
DC_GAP = 150e3                     # SDR tuned this far below the text: its DC spike stays out of the picture
CALL_MAX = 0.85                    # s per call (host BUSY limit ~1 s)


def word(hz):
    return int(round(hz * 1048576 / 1e6))


def ok(c, op, params=b'', nread=0):
    st, d = c.rc(op, nread, params)
    if st != 0:
        raise RuntimeError(f'rc {op:04X} {params.hex()} st={st}')
    return d


def store(c, data):
    data = bytes(data) + bytes(-len(data) % 4)
    for i in range(0, len(data), 4):
        if 'mw st=0' not in c.cmd(f'mw {TEXT + i:X} {struct.unpack("<I", data[i:i + 4])[0]:X}', 'mw st'):
            raise RuntimeError('RAM write failed')


def waterfall(path, fs, f_lo, f_hi, pass_s, dur):
    """Waterfall lines: 4 ms Hann FFTs (250 Hz bins) averaged over one pass, cropped to
    [f_lo, f_hi] Hz (baseband) and to dur s from the first line with signal (the PA's switch-off
    click comes later)."""
    z = np.fromfile(path, np.uint8).astype(np.float32)
    z = (z[0::2] - 127.4) + 1j * (z[1::2] - 127.4)
    z = z[int(0.1 * fs):]                       # tuner settling
    nfft = int(fs / 250)
    m = len(z) // nfft
    S = np.abs(np.fft.fftshift(np.fft.fft(z[:m * nfft].reshape(m, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2
    k = max(1, int(round(pass_s * fs / nfft)))
    S = S[:len(S) // k * k].reshape(-1, k, nfft).mean(axis=1)
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / fs))
    sel = (f >= f_lo) & (f <= f_hi)
    img = 10 * np.log10(S[:, sel] + 1e-3)
    on = np.flatnonzero(img.max(axis=1) > np.median(img) + 20)
    if len(on):
        img = img[max(0, on[0] - 1):on[0] + int(np.ceil(dur * fs / (k * nfft))) + 1]
    return img, f[sel], k * nfft / fs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('text')
    ap.add_argument('--board', choices=['xiao', 'nicerf'], default='xiao')
    ap.add_argument('--mhz', type=float, default=908.0, help='carrier (park) frequency')
    ap.add_argument('--offset', type=float, default=300, help='kHz from the carrier to the first column')
    ap.add_argument('--spacing', type=float, default=2.0, help='kHz between pixel columns')
    ap.add_argument('--dwell', type=float, default=2.0, help='ms per pixel visit')
    ap.add_argument('--row-ms', type=float, default=400, help='time per font row')
    ap.add_argument('--dbm', type=int, default=-9)
    ap.add_argument('--mirror', action='store_true', help='mirror left-right (for an inverted spectrum)')
    ap.add_argument('--no-sdr', action='store_true')
    ap.add_argument('--png', default='waterfall_text.png')
    a = ap.parse_args()
    port = default_port('XIAO', '/dev/ttyACM0') if a.board == 'xiao' else default_port('RAK3401', '/dev/ttyACM1')

    txt = a.text.encode('ascii', 'replace')
    if not 0 < len(txt) <= 64:
        raise SystemExit('1-64 characters')
    cols = 5 * len(txt)
    dwell = a.dwell / 1e3
    if cols * dwell > CALL_MAX:
        dwell = CALL_MAX / cols
        print(f'dwell shortened to {dwell * 1e3:.2f} ms so a pass fits in one call')
    pass_s = cols * dwell
    passes = max(1, min(int(CALL_MAX / pass_s), int(round(a.row_ms / 1e3 / pass_s))))
    width = (6 * len(txt) - 2) * a.spacing
    print(f'"{a.text}": {6 * len(txt) - 1} columns, {width:.0f} kHz wide at {a.mhz + a.offset / 1e3:.3f} MHz, '
          f'{passes} x {pass_s * 1e3:.0f} ms per row')

    c = pram.Console(port)
    if 'OK' not in c.cmd(f'set radio {a.mhz},62.5,7,5', '->', 10):
        raise SystemExit('set radio failed')
    c.cmd(f'set tx {a.dbm}', '->')
    c.cmd('hold on', 'hold st')
    pram.load(c, BLOB)
    if c.rc(0x012D, 4, b'\x02')[1] != b'TXT1':
        raise SystemExit('text blob not active')
    store(c, txt)

    sdr = None
    if not a.no_sdr:
        path = tempfile.mktemp(suffix='.u8')
        centre = a.mhz * 1e6 + a.offset * 1e3 - DC_GAP
        secs = 7 * (passes * pass_s + 0.05) + 1.2
        sdr = subprocess.Popen(['rtl_sdr', '-f', str(int(centre)), '-s', str(int(FS)), '-g', '30',
                                '-n', str(int(FS * secs)), path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.8)
    p = struct.pack('>BBHIIIB', len(txt), 0, passes, int(round(dwell * CLK)), word(a.spacing * 1e3),
                    word(a.offset * 1e3), int(a.mirror))
    ok(c, 0x0128, b'\0')
    ok(c, 0x0200, struct.pack('>I', int(round(a.mhz * 1e6))))
    ok(c, 0x020E, b'\x02')                          # CW carrier at the park frequency
    try:
        for row in range(7):
            ok(c, 0x012D, b'\x01' + p[:1] + bytes([row]) + p[2:], 4)
    finally:
        ok(c, 0x0128, b'\0')
    print('sent')
    if not sdr:
        return
    sdr.wait()
    img, f, dt = waterfall(path, FS, DC_GAP - 8e3, DC_GAP + width * 1e3 + 8e3, pass_s,
                          7 * passes * pass_s)
    os.unlink(path)
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    lo = np.percentile(img, 50)
    fig, ax = plt.subplots(figsize=(10, 4.5))
    ax.imshow(img, aspect='auto', cmap='inferno', vmin=lo, vmax=lo + 30, origin='upper',
              extent=[0, (f[-1] - f[0]) / 1e3, len(img) * dt, 0])
    ax.set_xlabel(f'kHz above {(centre + f[0]) / 1e6:.4f} MHz (RTL-SDR, uncorrected)')
    ax.set_ylabel('time, s')
    ax.set_title(f'"{a.text}" from the LR2021 CPU, received by an RTL-SDR')
    fig.tight_layout()
    fig.savefig(a.png, dpi=120)
    print(f'waterfall: {a.png}')


if __name__ == '__main__':
    main()
