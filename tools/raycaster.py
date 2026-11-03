#!/usr/bin/env python3
"""A first-person raycaster running on the LR2021's own CPU, drawn as vector graphics on a
simulated XY oscilloscope from what an RTL-SDR receives (firmware/pram/ray.c,
docs/pram-raycaster.md).

The chip holds the map and the player, moves the player with the keys the host passes in, casts
the rays, turns the walls into a vector display list and sends it by hopping its synthesizer during
CW: alternating X and Y beam positions, one per 80 us slot. This script FM-demodulates the RTL-SDR
stream, finds the frames and feeds the X/Y pairs to a phosphor-scope simulation. It renders nothing
itself: every line on the screen is a beam move the chip sent.

  python3 tools/raycaster.py play                  # window: arrows move/turn, , . strafe, q quits
  python3 tools/raycaster.py demo --gif out.gif    # a scripted walk, recorded and decoded
  python3 tools/raycaster.py sim --gif sim.gif     # no radio: ray.c built for the host, simulated RF
"""
import argparse
import os
import struct
import subprocess
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HERE = os.path.dirname(os.path.abspath(__file__))
BLOB = os.path.join(HERE, '..', 'firmware', 'pram', 'ray.bin')
W = 80                          # screen width (X 0..80, sent in half units)
CLK = 32.0e6                    # CPU clock in TX [HW]
FS = 2.048e6                    # RTL-SDR sample rate
TUNE_BELOW = 400e3              # SDR tuned this far below the carrier: its DC spike stays out of the way
STEP_HZ = 1000.0                # one level: half a screen unit (data +-80 kHz, PEN -130, SYNC/MARK +-220 kHz)
DWELL_US = 80.0                 # one slot (the PLL settles in 15-20 us)
EMIT_CYCLES = 31                # each slot also takes ~31 CPU cycles of loop and register writes [HW]
SYNC_L, MARK_L, PEN_L = -220, 220, -130   # levels: f = L x step; X = x - 80, Y = y (half units)

# The map lives in the blob (firmware/pram/ray.c, loosely after the start of E1M1); the demo's
# route below is just a player who knows the way.
START = (2.5, 12.5, 0)          # cells, cells, angle (0-1023, 0 = east, 256 = south)
# the demo's route: along the bottom corridor, north up the long hall, west, into the start room
ROUTE = [(10.5, 12.5), (10.5, 3.5), (3.5, 3.5), (3.5, 6.5), (6.0, 6.5), (6.0, 9.0), (1.5, 9.0)]


def word(hz):
    return int(round(hz * 1048576 / 1e6))


# ---------------------------------------------------------------- receiver

def fm_demod(iq, fs, shift):
    """Instantaneous frequency (Hz) of iq after shifting it down by `shift` Hz."""
    n = np.arange(len(iq))
    x = iq * np.exp(-2j * np.pi * shift / fs * n)
    x = np.convolve(x, np.ones(4) / 4, mode='same')   # light smoothing; first null at fs/4
    return np.angle(x[1:] * np.conj(x[:-1])) * fs / (2 * np.pi)


def decode_frames(f, fs, step=STEP_HZ, dwell_us=DWELL_US, times=False):
    """Frames in an instantaneous-frequency series. Returns (frames, consumed[, starts]): each
    frame is an (n, 3) array of beam points x, y, beam on (analog values, noise and all).
    One pass: the SYNC/MARK masks and their runs are found once for the whole buffer, and each
    frame only looks at its own samples (about 0.2 s of CPU per second of 2 MS/s signal)."""
    from scipy.ndimage import median_filter
    lv = f / step
    med = median_filter(lv, 9, mode='nearest')  # FM clicks don't break SYNC/MARK runs; centred, no delay
    sync = med < (SYNC_L + PEN_L) / 2           # halfway between SYNC and PEN, the lowest other level
    mark = med > (MARK_L + 80) / 2              # halfway between the highest data level and MARK
    dw = (dwell_us * 1e-6 + EMIT_CYCLES / CLK) * fs
    n = len(lv)
    # SYNC runs: [start, end) pairs
    d = np.diff(np.concatenate([[0], sync.astype(np.int8), [0]]))
    rs, re_ = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    # a frame opens after a run of at least 2 slots; it closes at the next run of at least half a slot
    # (the synthesizer overshoots on big steps, and a short dip below the threshold isn't SYNC)
    held = rs[(re_ - rs) >= max(1, int(0.5 * dw))]
    opens = re_[(re_ - rs) >= 2 * dw]
    look_n, edge_n = int(30e-6 * fs), int(40e-6 * fs)
    frames, starts, used = [], [], 0
    for k2 in opens:
        if k2 < used:
            continue
        look = mark[k2:k2 + look_n]
        if not look.any():
            continue
        # Edge timing: the SYNC -> MARK step crosses 50 % and 89 % of its swing 0.69 and 2.2 time
        # constants after it starts (first-order settling), which gives the PLL's time constant; the
        # closing MARK -> SYNC step crosses its own 89 % point equally late.
        r0 = max(0, k2 - int(20e-6 * fs))
        seg = med[r0:k2 + edge_n]
        thr89 = SYNC_L + 0.89 * (MARK_L - SYNC_L)
        up50, up89 = int(np.argmax(seg > (SYNC_L + MARK_L) / 2)), int(np.argmax(seg > thr89))
        if not seg[up89] > thr89:
            continue
        tau = float(np.clip((up89 - up50) / 1.51, 0.5, 30))
        t0 = r0 + up50 - 0.69 * tau             # the first MARK starts
        h = np.searchsorted(held, t0 + dw)
        if h >= len(held):
            break                               # the frame isn't complete yet
        e = int(held[h])
        dn = med[max(0, e - edge_n):e + 1]
        e89 = e - len(dn) + 1 + int(np.argmax(dn < MARK_L - 0.89 * (MARK_L - SYNC_L)))
        end = e89 - 2.2 * tau                   # the closing MARK ends
        slots = int(round((end - t0) / dw)) - 2 # MARK, data, MARK
        if slots < 2:
            used = e
            continue
        slot = (end - t0) / (slots + 2)
        # Calibrate on this frame's own SYNC and MARK: the carrier drifts and the synthesizer's step
        # isn't exactly the nominal one, so map the measured SYNC..MARK span onto -150..+150 steps.
        sy = np.median(lv[max(0, int(t0 - 3 * slot)):int(t0 - 0.5 * slot)])
        mk = np.median(lv[int(t0 + 0.45 * slot):int(t0 + 0.95 * slot)])
        if not mk - sy > 0.5 * (MARK_L - SYNC_L):
            used = e
            continue
        mid, scale = (mk + sy) / 2, (MARK_L - SYNC_L) / (mk - sy)   # (SYNC and MARK are symmetric)
        # each slot's settled part (55-95 % of it), all slots at once
        m = max(1, int(0.4 * slot))
        a0 = (t0 + (1 + np.arange(slots) + 0.55) * slot).astype(np.int64)
        L = (np.median(lv[a0[:, None] + np.arange(m)], axis=1) - mid) * scale
        pts = parse_points(L)
        if pts is None:
            used = e
            continue
        frames.append(pts)
        starts.append(t0)
        used = e
    return (frames, used, starts) if times else (frames, used)


def parse_points(L):
    """Slot levels -> (n, 3) points x, y (screen units), beam on. [PEN] X Y per point."""
    out, pen, i = [], False, 0
    while i < len(L):
        if L[i] < (PEN_L - 80) / 2:             # halfway between PEN and the lowest X
            pen = True
            i += 1
            continue
        if i + 1 >= len(L):
            return None                         # a dangling X: the slot count was off
        out.append(((L[i] + 80) / 2, L[i + 1] / 2, not pen))
        pen, i = False, i + 2
    return np.array(out) if out else None


# ---------------------------------------------------------------- the scope

class Scope:
    """A simulated XY oscilloscope: a P31-green phosphor that the beam excites along its path
    (dimmer where it moves fast), with glow, persistence and a graticule."""

    def __init__(self, size=(520, 640), persist=0.35):
        self.h, self.w = size
        self.persist = persist
        self.acc = np.zeros(size, np.float32)
        self.grat = self._graticule()

    def _graticule(self):
        g = np.zeros((self.h, self.w), np.float32)
        for i in range(11):
            g[:, min(self.w - 1, int(i * (self.w - 1) / 10))] = 1
        for i in range(9):
            g[min(self.h - 1, int(i * (self.h - 1) / 8)), :] = 1
        cy, cx = self.h // 2, self.w // 2
        for i in range(0, self.w, self.w // 50):
            g[cy - 3:cy + 4, i] = 1
        for i in range(0, self.h, self.h // 40):
            g[i, cx - 3:cx + 4] = 1
        return g

    def xy(self, x, y):                         # chip screen units -> pixels
        return (40 + x / W * (self.w - 80), self.h / 2 - y / 34 * (self.h / 2 - 20))

    def frame(self, pts):
        self.acc *= self.persist
        if pts is None or len(pts) == 0:
            return
        px, py = self.xy(pts[:, 0], pts[:, 1])
        for i in range(1, len(pts)):
            if not pts[i, 2]:
                continue                        # beam off: a move
            x0, y0, x1, y1 = px[i - 1], py[i - 1], px[i], py[i]
            ln = max(1.0, np.hypot(x1 - x0, y1 - y0))
            t = np.linspace(0, 1, int(ln * 2) + 2)
            xs = np.clip(np.rint(x0 + t * (x1 - x0)).astype(int), 0, self.w - 1)
            ys = np.clip(np.rint(y0 + t * (y1 - y0)).astype(int), 0, self.h - 1)
            # the beam spends one slot per segment: energy per pixel ~ 1 / length
            np.add.at(self.acc, (ys, xs), min(1.0, 90.0 / ln) * 0.75)
            self.acc[ys[[0, -1]], xs[[0, -1]]] += 0.6       # it dwells at each point: bright dots

    def image(self):
        from scipy.ndimage import gaussian_filter
        core = np.minimum(self.acc, 1.5)
        glow = gaussian_filter(core, 2.2) * 2.5 + gaussian_filter(core, 7) * 1.5
        v = np.clip(core * 0.9 + glow, 0, None)
        v = 1 - np.exp(-1.6 * v)                 # phosphor saturates
        rgb = np.empty((self.h, self.w, 3), np.float32)
        rgb[..., 0] = v * 0.35 + v ** 3 * 0.55
        rgb[..., 1] = v
        rgb[..., 2] = v * 0.45 + v ** 3 * 0.4
        rgb += self.grat[..., None] * np.array([0.07, 0.11, 0.09], np.float32)
        rgb += np.array([0.015, 0.025, 0.02], np.float32)  # the screen isn't black
        return (np.clip(rgb, 0, 1) * 255).astype(np.uint8)


def save_mp4(path, frames, fps=60, persist=0.5):
    """The scope as an H.264 video, one decoded frame per video frame."""
    sc = Scope(persist=persist)
    p = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                          '-s', f'{sc.w}x{sc.h}', '-r', str(fps), '-i', '-', '-c:v', 'libx264', '-preset', 'slow',
                          '-crf', '18', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', path], stdin=subprocess.PIPE)
    for f in frames:
        sc.frame(f)
        p.stdin.write(sc.image().tobytes())
    p.stdin.close()
    p.wait()
    print(f'saved {path} ({len(frames)} frames at {fps} fps)')


def save_gif(path, frames, fps=25, persist=0.35):
    from PIL import Image
    sc = Scope(persist=persist)
    imgs = []
    for p in frames:
        sc.frame(p)
        imgs.append(Image.fromarray(sc.image()))
    imgs[0].save(path, save_all=True, append_images=imgs[1:], duration=int(1000 / fps), loop=0)
    imgs[len(imgs) // 2].save(os.path.splitext(path)[0] + '.png')
    print(f'saved {path} ({len(imgs)} frames) and a still')


# ---------------------------------------------------------------- simulation (no radio)

def sim_iq(log, fs, snr_db=25, settle_us=6.0, rng=None):
    """RTL-SDR samples for a list of (synth word offset, cycles): FM with a first-order settle."""
    from scipy.signal import lfilter
    rng = rng or np.random.default_rng(1)
    hz = np.array([w * 1e6 / 1048576 for w, _ in log])
    t_end = np.cumsum([c / CLK for _, c in log])
    n = int(t_end[-1] * fs) + 1
    idx = np.searchsorted(t_end, np.arange(n) / fs, side='right')
    target = hz[np.minimum(idx, len(hz) - 1)]
    a = np.exp(-1 / (settle_us * 1e-6 * fs))
    f = lfilter([1 - a], [1, -a], target, zi=[a * target[0]])[0] + TUNE_BELOW
    iq = np.exp(1j * 2 * np.pi * np.cumsum(f) / fs)
    noise = (rng.standard_normal(n) + 1j * rng.standard_normal(n)) * 10 ** (-snr_db / 20) / np.sqrt(2)
    return (iq + noise).astype(np.complex64)


def resample(frames, times, fps):
    """The frame on screen at each video frame time (None where nothing new arrived)."""
    if not frames:
        return []
    t = np.asarray(times) - times[0]
    out, j = [], 0
    for k in range(int(t[-1] * fps) + 1):
        new = []
        while j < len(frames) and t[j] <= (k + 1) / fps:
            new.append(frames[j])
            j += 1
        out.append(new[-1] if new else None)
    return out


def cmd_sim(a):
    x, y, ang = START
    out = subprocess.run([a.sim_bin, str(int(x * 256)), str(int(y * 256)), str(ang), str(a.keys),
                          str(a.frames)] + (['title'] if a.title else []), capture_output=True, check=True)
    print(out.stderr.decode().strip())
    raw = np.frombuffer(out.stdout, '<u4').reshape(-1, 2)
    log = [(int(np.int32(np.uint32(w))), int(c) if c else 64) for w, c in raw]
    iq = sim_iq(log, FS, a.snr)
    frames, _ = decode_frames(fm_demod(iq, FS, TUNE_BELOW), FS)
    print(f'{len(frames)} frames decoded from {len(iq) / FS * 1e3:.0f} ms of simulated RF, '
          f'{np.mean([len(p) for p in frames]):.0f} points each')
    if a.gif and frames:
        save_gif(a.gif, frames)
    if a.mp4 and frames:
        save_mp4(a.mp4, frames)


# ---------------------------------------------------------------- the chip and the SDR

class Chip:
    """The LR2021 running ray.bin, sending CW at `mhz`."""

    def __init__(self, port, mhz, dbm, dwell_us=DWELL_US, step_hz=STEP_HZ):
        import pram
        self.c = c = pram.Console(port)
        self.dwell, self.step = int(round(dwell_us * 1e-6 * CLK)), word(step_hz)
        for cmd in (f'set radio {mhz:.3f},62.5,7,5', f'set tx {dbm}'):   # resets the chip, then the power
            if 'OK' not in c.cmd(cmd, '->', 10):
                raise SystemExit(f'{cmd} failed')
        c.cmd('hold on', 'hold st')
        pram.load(c, BLOB)                       # always: an older ray.bin answers RAY1 too
        if c.rc(0x012D, 4, b'\x02')[1] != b'RAY1':
            raise SystemExit('raycaster blob not active')
        self.ok(0x0128, b'\0')
        self.ok(0x0200, struct.pack('>I', int(round(mhz * 1e6))))
        self.ok(0x020E, b'\x02')                 # CW carrier
        x, y, ang = START
        self.ok(0x012D, b'\x01\x00' + struct.pack('>HHH', int(x * 256), int(y * 256), ang), 10)

    def ok(self, op, params=b'', nread=0):
        st, d = self.c.rc(op, nread, params)
        if st != 0:
            raise RuntimeError(f'rc {op:04X} {params.hex()} st={st}')
        return d

    def play(self, keys, frames):
        p = b'\x01\x01' + bytes([keys, frames]) + struct.pack('>II', self.dwell, self.step)
        return struct.unpack('>HHHHH', self.ok(0x012D, p, 10))

    def title(self, frames):
        p = b'\x01\x02\x00' + bytes([frames]) + struct.pack('>II', self.dwell, self.step)
        return struct.unpack('>HHHHH', self.ok(0x012D, p, 10))

    def close(self):
        self.ok(0x0128, b'\0')


class Sdr:
    """rtl_sdr streaming to a pipe, tuned TUNE_BELOW under the carrier, decoded as it comes."""

    def __init__(self, mhz, gain, record=False):
        self.record, self.raw = record, []        # record: keep the samples, decode later (no drops)
        self.p = subprocess.Popen(['rtl_sdr', '-f', str(int(mhz * 1e6 - TUNE_BELOW)), '-s', str(int(FS)),
                                   '-g', str(gain), '-'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  bufsize=0)
        self.stop = threading.Event()
        self.frames, self.times = [], []
        self.n = 0                              # samples read so far
        self.th = threading.Thread(target=self._run, daemon=True)
        self.th.start()

    def _read(self, n):
        b = b''
        while len(b) < 2 * n:
            c = self.p.stdout.read(2 * n - len(b))
            if not c:
                break
            b += c
        u = np.frombuffer(b, np.uint8).astype(np.float32) - 127.5
        return (u[0::2] + 1j * u[1::2]).astype(np.complex64) / 128

    def _run(self):
        buf = np.zeros(0, np.complex64)
        while not self.stop.is_set():
            iq = self._read(262144)
            if len(iq) == 0:
                break
            if self.record:
                self.raw.append(iq)
                continue
            start = self.n - len(buf)               # sample index of buf[0]
            self.n += len(iq)
            buf = np.concatenate([buf, iq])
            fr, used, at = decode_frames(fm_demod(buf, FS, TUNE_BELOW), FS, times=True)
            self.frames.extend(fr)
            self.times.extend((start + t) / FS for t in at)
            buf = buf[used:] if used else buf[-int(0.1 * FS):]

    def close(self):
        self.stop.set()
        self.p.terminate()
        self.th.join(timeout=2)
        if self.record and self.raw:
            iq = np.concatenate(self.raw)
            fr, _, at = decode_frames(fm_demod(iq, FS, TUNE_BELOW), FS, times=True)
            self.frames, self.times = fr, [t / FS for t in at]


def setup(a, record=False):
    from radio_ports import default_port
    port = a.port or default_port('RAK', '/dev/ttyACM0')
    sdr = Sdr(a.mhz, a.gain, record)
    chip = Chip(port, a.mhz, a.dbm)
    return chip, sdr


def cmd_demo(a):
    chip, sdr = setup(a, record=True)
    time.sleep(0.5)
    sdr.raw.clear()
    # Walk the route like a player would: turn toward the next waypoint (the chip reports where it
    # put us), walk when roughly facing it. The chip does the moving and the collisions.
    t0, sent, wp = time.time(), 0, 0
    while time.time() - t0 < a.title:           # the title screen first
        sent += chip.title(a.per_call)[3]
    st = chip.play(0, a.per_call)
    try:
        while wp < len(ROUTE) and time.time() - t0 < 30:
            x, y, ang = st[0] / 256, st[1] / 256, st[2]
            tx, ty = ROUTE[wp]
            if np.hypot(tx - x, ty - y) < 0.35:
                wp += 1
                continue
            want = int(round(np.arctan2(ty - y, tx - x) / (2 * np.pi) * 1024)) & 1023
            err = (want - ang + 512) % 1024 - 512
            keys = (8 if err > 0 else 4) if abs(err) > 12 else 0
            if abs(err) < 90:
                keys |= 1
            st = chip.play(keys, a.per_call)
            sent += st[3]
    finally:
        chip.close()
        dt = time.time() - t0
        time.sleep(0.3)
        sdr.close()
    print(f'chip sent {sent} frames in {dt:.2f} s ({sent / dt:.1f} fps), {st[4]} beam points in the last; '
          f'player at {st[0] / 256:.2f}, {st[1] / 256:.2f}')
    print(f'decoded {len(sdr.frames)} frames')
    if a.gif and sdr.frames:
        k = max(1, round(sent / dt / 25))          # real time at 25 fps
        save_gif(a.gif, sdr.frames[::k])
    if a.mp4 and sdr.frames:
        # every decoded frame, timed by when it arrived: a dropped frame leaves the last one glowing
        fps = 60
        save_mp4(a.mp4, resample(sdr.frames, sdr.times, fps), fps)


KEYS = {'up': 1, 'down': 2, 'left': 4, 'right': 8, ',': 16, '.': 32, 'w': 1, 's': 2, 'a': 4, 'd': 8}


def cmd_play(a):
    import matplotlib
    import matplotlib.pyplot as plt
    for k in list(matplotlib.rcParams):
        if k.startswith('keymap.') and k != 'keymap.quit':
            matplotlib.rcParams[k] = []
    chip, sdr = setup(a)
    held, done = set(), threading.Event()
    stats = {'fps': 0.0}

    def drive():
        t, n = time.time(), 0
        while not done.is_set():
            keys = 0
            for k in list(held):
                keys |= KEYS.get(k, 0)
            chip.play(keys, a.per_call)
            n += a.per_call
            if time.time() - t > 1:
                stats['fps'], t, n = n / (time.time() - t), time.time(), 0
    th = threading.Thread(target=drive, daemon=True)
    th.start()
    sc = Scope()
    fig = plt.figure(figsize=(8, 6.5), facecolor='black')
    ax = fig.add_axes([0, 0, 1, 1])
    ax.axis('off')
    im = ax.imshow(sc.image(), interpolation='bilinear')
    txt = ax.text(8, 16, '', color='#3c6', fontsize=9, family='monospace')
    fig.canvas.mpl_connect('key_press_event', lambda e: held.add(e.key))
    fig.canvas.mpl_connect('key_release_event', lambda e: held.discard(e.key))
    fig.canvas.manager.set_window_title('LR2021 raycaster: RTL-SDR -> XY scope')
    try:
        while plt.fignum_exists(fig.number):
            fr = sdr.frames[-3:]
            sdr.frames.clear()
            sdr.times.clear()
            for p in fr:
                sc.frame(p)
            if not fr:
                sc.frame(None)
            im.set_data(sc.image())
            txt.set_text(f'LR2021 908 MHz  chip {stats["fps"]:4.1f} fps  '
                         f'{len(fr[-1]) if fr else 0:3d} beam points')
            plt.pause(0.03)
    finally:
        done.set()
        th.join(timeout=2)
        chip.close()
        sdr.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('sim')
    s.add_argument('--sim-bin', default='ray_sim', help='ray_sim built from firmware/pram/ray_sim.c')
    s.add_argument('--keys', type=int, default=1 | 8)
    s.add_argument('--frames', type=int, default=40)
    s.add_argument('--snr', type=float, default=25)
    s.add_argument('--title', action='store_true', help='the title screen instead of the game')
    s.add_argument('--gif')
    s.add_argument('--mp4')
    for name in ('demo', 'play'):
        d = sub.add_parser(name)
        d.add_argument('--port', help='the transmitting board (default: the NiceRF)')
        d.add_argument('--mhz', type=float, default=908.0)
        d.add_argument('--dbm', type=int, default=-9, help='drive level (the NiceRF has a ~2 W PA after it)')
        d.add_argument('--gain', type=float, default=40.2, help='RTL-SDR gain, dB: 40-44 wobbles least; lower leaves the 8-bit ADC under-driven')
        d.add_argument('--per-call', type=int, default=4, help='frames per chip call')
        d.add_argument('--gif')
        d.add_argument('--mp4', help='60 fps video of the scope (needs ffmpeg)')
        d.add_argument('--title', type=float, default=2.5, help='seconds of title screen first')
    a = ap.parse_args()
    {'sim': cmd_sim, 'demo': cmd_demo, 'play': cmd_play}[a.cmd](a)


if __name__ == '__main__':
    main()
