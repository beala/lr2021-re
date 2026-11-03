#!/usr/bin/env python3
"""The figure explaining how the raycaster sends a frame (docs/figures/raycaster_encoding.png).

One real frame from the chip, as the RTL-SDR received it: the demodulated frequency against time
with every slot's decoded value, a close-up of the first slots, and the picture the points draw.

  python3 tools/raycaster.py ...                 # (the chip and SDR setup live there)
  python3 tools/raycaster_figure.py --capture capture.npy     # record a frame from the chip
  python3 tools/raycaster_figure.py --iq capture.npy --out docs/figures/raycaster_encoding.png
"""
import argparse
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import raycaster as r  # noqa: E402

POSE = (4.0, 6.5, 768)          # cells, cells, angle: a corridor straight ahead (3 wall faces, 15 points)
INK, INK2, MUTED, GRID = '#0b0b0b', '#52514e', '#8a8984', '#e4e3df'
SURFACE = '#fcfcfb'
C_X, C_Y, C_PEN, C_FRAME = '#2a78d6', '#eb6834', '#1baf7a', '#8a8984'


def capture(path, port=None):
    class A:
        pass
    a = A()
    a.port, a.mhz, a.dbm, a.gain = port, 908.0, -9, 40.2
    chip, sdr = r.setup(a, record=True)
    x, y, ang = POSE
    chip.ok(0x012D, b'\x01\x00' + struct.pack('>HHH', int(x * 256), int(y * 256), ang), 10)
    time.sleep(0.5)
    sdr.raw.clear()
    t = time.time()
    while time.time() - t < 0.6:
        chip.play(0, 4)
    chip.close()
    time.sleep(0.2)
    raw = list(sdr.raw)
    sdr.close()
    np.save(path, np.concatenate(raw))
    print(f'saved {path}')


def slots_of(f, t0, fs):
    """The frame starting at sample t0: slot length, calibration, and each slot's kind and value."""
    lv = f / r.STEP_HZ
    dw = (r.DWELL_US * 1e-6 + r.EMIT_CYCLES / r.CLK) * fs
    sy = np.median(lv[int(t0 - 3 * dw):int(t0 - 0.5 * dw)])
    mk = np.median(lv[int(t0 + 0.45 * dw):int(t0 + 0.95 * dw)])
    mid, scale = (mk + sy) / 2, (r.MARK_L - r.SYNC_L) / (mk - sy)
    cal = (lv - mid) * scale                    # levels; 1 level = 1 kHz nominal
    out, k = [], 1
    while True:
        a = int(t0 + (k + 0.55) * dw)
        v = np.median(cal[a:a + int(0.4 * dw)])
        if v > (r.MARK_L + 80) / 2:
            out.append(('MARK', v, k))
            break
        out.append((None, v, k))
        k += 1
    kinds, expect = [], 'X'
    for _, v, k in out[:-1]:
        if v < (r.PEN_L - 80) / 2:
            kinds.append(('PEN', v, k))
            continue
        kinds.append((expect, v, k))
        expect = 'Y' if expect == 'X' else 'X'
    return dw, cal, kinds + [out[-1]]


def figure(iq_path, out):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyArrowPatch

    iq = np.load(iq_path)
    fs = r.FS
    f = r.fm_demod(iq, fs, r.TUNE_BELOW)
    frames, _, starts = r.decode_frames(f, fs, times=True)
    pick = len(frames) // 2
    pts, t0 = frames[pick], starts[pick]
    dw, cal, slots = slots_of(f, t0, fs)

    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 9, 'axes.edgecolor': MUTED,
                         'axes.labelcolor': INK2, 'xtick.color': INK2, 'ytick.color': INK2,
                         'axes.spines.top': False, 'axes.spines.right': False})
    fig = plt.figure(figsize=(12, 8.2), dpi=160, facecolor=SURFACE)
    gs = fig.add_gridspec(2, 2, height_ratios=[1.05, 1], width_ratios=[1, 1.05], hspace=0.42, wspace=0.18,
                          left=0.06, right=0.98, top=0.82, bottom=0.07)
    fig.text(0.06, 0.962, 'How the LR2021 sends one frame: every slot is one frequency', fontsize=15,
             color=INK, weight='bold')
    fig.text(0.06, 0.93, 'One real frame of the raycaster, received by an RTL-SDR: 15 beam points, '
             f'{(len(slots) + 1) * dw / fs * 1e3:.1f} ms on air. The chip renders during SYNC, then hops '
             'its carrier once per 80 µs slot.', fontsize=10, color=INK2)

    # ---- A: the whole frame
    ax = fig.add_subplot(gs[0, :])
    a0, a1 = int(t0 - 6 * dw), int(t0 + (len(slots) + 3) * dw)
    t = (np.arange(a0, a1) - t0) / fs * 1e3
    ax.plot(t, cal[a0:a1], color=INK2, lw=0.6, alpha=0.55, zorder=1)
    colors = {'X': C_X, 'Y': C_Y, 'PEN': C_PEN, 'MARK': C_FRAME}
    for kind, v, k in slots:
        ts, te = k * dw / fs * 1e3, (k + 1) * dw / fs * 1e3
        ax.plot([ts + 0.08 * dw / fs * 1e3, te - 0.08 * dw / fs * 1e3], [v, v], color=colors[kind],
                lw=3, solid_capstyle='round', zorder=3)
        if kind in ('X', 'Y', 'PEN'):
            lab = {'X': 'X', 'Y': 'Y', 'PEN': 'P'}[kind]
            ax.text((ts + te) / 2, v + (14 if v >= 0 or kind != 'PEN' else -26), lab, ha='center', fontsize=7,
                    color=INK2, zorder=4)
    # MARK at the start and SYNC
    ax.plot([0.08 * dw / fs * 1e3, 0.92 * dw / fs * 1e3], [r.MARK_L] * 2, color=C_FRAME, lw=3, zorder=3)
    ax.plot([-5.9 * dw / fs * 1e3, -0.1 * dw / fs * 1e3], [r.SYNC_L] * 2, color=C_FRAME, lw=3, zorder=3)
    lab = dict(boxstyle='round,pad=0.2', fc=SURFACE, ec='none', alpha=0.9)
    ax.text(-3 * dw / fs * 1e3, r.SYNC_L + 30, 'SYNC (the chip renders meanwhile)', ha='center', fontsize=8,
            color=INK, bbox=lab, zorder=5)
    ax.text(0.5 * dw / fs * 1e3, r.MARK_L + 14, 'MARK', ha='center', fontsize=8, color=INK2)
    km = slots[-1][2]
    ax.text((km + 0.5) * dw / fs * 1e3, r.MARK_L + 14, 'MARK', ha='center', fontsize=8, color=INK2)
    ax.text((km + 2.5) * dw / fs * 1e3, r.SYNC_L + 30, 'SYNC', ha='center', fontsize=8, color=INK, bbox=lab, zorder=5)
    # point numbers under each X/Y pair
    n, i = 0, 0
    while i < len(slots) - 1:
        kind, v, k = slots[i]
        if kind == 'PEN':
            i += 1
            continue
        n += 1
        ax.text((k + 1) * dw / fs * 1e3, -262, str(n), ha='center', fontsize=7, color=MUTED)
        i += 2
    ax.text(0.12, -262, 'points', ha='left', fontsize=7, color=MUTED)
    for y, lab in ((r.MARK_L, 'MARK +220'), (80, 'X max'), (-80, 'X min / Y range'), (r.PEN_L, 'PEN −130'),
                   (r.SYNC_L, 'SYNC −220')):
        ax.axhline(y, color=GRID, lw=0.8, zorder=0)
    ax.set_ylim(-285, 265)
    ax.set_xlim(t[0], t[-1])
    ax.set_ylabel('carrier offset (kHz)')
    ax.set_xlabel('time from MARK (ms)')
    ax.set_yticks([-220, -130, -80, 0, 80, 220])
    ax.grid(False)
    handles = [plt.Line2D([], [], color=c, lw=3) for c in (C_X, C_Y, C_PEN, C_FRAME)]
    ax.legend(handles, ['X: x − 80 kHz per half unit', 'Y: y kHz per half unit', 'PEN: next point is a move',
                        'SYNC / MARK: framing'],
              loc='upper center', bbox_to_anchor=(0.5, 1.13), ncol=4, frameon=False, fontsize=8.5,
              labelcolor=INK2)
    ax.set_title('A  The whole frame', loc='left', fontsize=10.5, color=INK, weight='bold', pad=30)

    # ---- B: close-up of the first slots
    bx = fig.add_subplot(gs[1, 0])
    b0, b1 = int(t0 - 0.6 * dw), int(t0 + 6.2 * dw)
    tb = (np.arange(b0, b1) - t0) / fs * 1e6
    bx.plot(tb, cal[b0:b1], color=INK2, lw=0.9, zorder=2)
    for kind, v, k in [('MARK', r.MARK_L, 0)] + slots[:5]:
        ts = k * dw / fs * 1e6
        bx.axvline(ts, color=GRID, lw=0.8, zorder=0)
        a = (k + 0.55) * dw / fs * 1e6
        b = (k + 0.95) * dw / fs * 1e6
        bx.axvspan(a, b, color=colors[kind], alpha=0.13, lw=0, zorder=1)
        if kind != 'MARK':
            bx.plot([a, b], [v, v], color=colors[kind], lw=3, solid_capstyle='round', zorder=3)
            name = {'X': 'x', 'Y': 'y'}.get(kind)
            txt = 'PEN' if kind == 'PEN' else f'{kind}\n{name} = {(v + 80) / 2 if kind == "X" else v / 2:+.1f}'
            bx.text((k + 0.5) * dw / fs * 1e6, v + 48, txt, ha='center', va='bottom', fontsize=8, color=INK,
                    bbox=dict(boxstyle='round,pad=0.2', fc=SURFACE, ec='none', alpha=0.9), zorder=5)
        else:
            bx.text((k + 0.5) * dw / fs * 1e6, v - 30, 'MARK', ha='center', va='top', fontsize=8, color=INK,
                    bbox=dict(boxstyle='round,pad=0.2', fc=SURFACE, ec='none', alpha=0.9), zorder=5)
    bx.annotate('the carrier jumps\nand settles in ~15 µs', xy=(1.08 * dw / fs * 1e6, -60), xytext=(2.0 * dw / fs * 1e6, -230),
                fontsize=8, color=INK2, arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    bx.text(6.1 * dw / fs * 1e6, -275, 'shaded: the part of each slot\nthe receiver reads\n(median of the last 40 %)',
            ha='right', va='bottom', fontsize=8, color=INK2)
    bx.set_xlim(tb[0], tb[-1])
    bx.set_ylim(-285, 265)
    bx.set_yticks([-220, -130, -80, 0, 80, 220])
    bx.set_xlabel('time from MARK (µs)')
    bx.set_ylabel('carrier offset (kHz)')
    bx.set_title('B  The first slots, close up: a move (PEN), then two points', loc='left', fontsize=10.5, color=INK,
                 weight='bold')

    # ---- C: the picture
    cx = fig.add_subplot(gs[1, 1])
    for i in range(1, len(pts)):
        x0, y0, x1, y1 = pts[i - 1, 0], pts[i - 1, 1], pts[i, 0], pts[i, 1]
        if pts[i, 2]:
            cx.plot([x0, x1], [y0, y1], color=INK, lw=2, solid_capstyle='round', zorder=2)
        else:
            cx.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle='-|>', mutation_scale=10, color=C_PEN,
                                         lw=1.2, linestyle=(0, (3, 2)), zorder=1))
    groups = []                                 # points within a unit of each other share a label
    for i, (x, y, on) in enumerate(pts):
        for g in groups:
            if abs(g[0] - x) < 2.5 and abs(g[1] - y) < 2.5:
                g[2].append(i + 1)
                break
        else:
            groups.append([x, y, [i + 1]])
    for x, y, ids in groups:
        cx.plot(x, y, 'o', ms=4.5, color=SURFACE, mec=INK, mew=1.2, zorder=3)
        inner = 0 < x < 79 and abs(x - 40) < 30
        dx = (-1.2 if x < 40 else 1.2) if inner else (1.2 if x < 40 else -1.2)
        cx.text(x + dx, y + (2.2 if y >= 0 else -2.2), '/'.join(map(str, ids)), fontsize=7.5, color=INK,
                ha='left' if dx > 0 else 'right', va='bottom' if y >= 0 else 'top', zorder=4)
    cx.set_xlim(-2, 82)
    cx.set_ylim(-40, 34)
    cx.set_aspect('equal')
    cx.set_xticks([0, 20, 40, 60, 80])
    cx.set_yticks([-31, 0, 31])
    cx.set_xlabel('x (screen units)')
    cx.set_ylabel('y')
    cx.grid(color=GRID, lw=0.6)
    cx.set_axisbelow(True)
    cx.set_title('C  What those 15 points draw', loc='left', fontsize=10.5,
                 color=INK, weight='bold')
    cx.text(42, -38.5, '5 points per wall face, the last back on the first\n'
            'solid: beam on   dashed: move after PEN', ha='center', va='bottom', fontsize=7.5, color=INK2)
    fig.savefig(out, facecolor=SURFACE)
    print(f'saved {out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture', help='record a frame from the chip into this .npy')
    ap.add_argument('--port')
    ap.add_argument('--iq', help='a recording made with --capture')
    ap.add_argument('--out', default='raycaster_encoding.png')
    a = ap.parse_args()
    if a.capture:
        capture(a.capture, a.port)
    if a.iq:
        figure(a.iq, a.out)


if __name__ == '__main__':
    main()
