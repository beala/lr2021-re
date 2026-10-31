# APRS / AX.25 over AFSK 1200 (Bell 202): frame builder and IQ decoder.
#   python3 aprs.py encode SRC DEST "info" [path,...]   -> prints firmware "afsk" commands
#   python3 aprs.py decode iq8.bin [harmonic]           -> FM-demod an 83.33 kS/s int8 IQ stream, print frames
#   python3 aprs.py selftest
import sys, numpy as np

def crc16_x25(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF

def addr(call, last):
    c, _, ssid = call.upper().partition('-')
    a = bytes((ord(ch) << 1) for ch in c.ljust(6)[:6])
    return a + bytes([0x60 | ((int(ssid or 0) & 15) << 1) | (1 if last else 0)])

def frame(src, dest, info, path=()):
    calls = [dest, src] + list(path)
    body = b''.join(addr(c, i == len(calls) - 1) for i, c in enumerate(calls)) + b'\x03\xf0' + info.encode()
    f = crc16_x25(body)
    return body + bytes([f & 0xFF, f >> 8])

def tones(fr, preamble_flags=40, tail_flags=4):
    """HDLC bit stuffing + flags + NRZI. Returns tone bits (1 = mark 1200 Hz)."""
    bits = []
    for b in fr:
        bits += [(b >> i) & 1 for i in range(8)]       # AX.25 sends LSB first
    stuffed, ones = [], 0
    for b in bits:
        stuffed.append(b)
        ones = ones + 1 if b else 0
        if ones == 5: stuffed.append(0); ones = 0
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    raw = flag * preamble_flags + stuffed + flag * tail_flags
    out, level = [], 1
    for b in raw:                                       # NRZI: 0 = change tone, 1 = keep
        if b == 0: level ^= 1
        out.append(level)
    return out

def fw_commands(tb):
    cmds = ['afsk clear']
    for i in range(0, len(tb), 512):
        chunk = tb[i:i + 512]
        by = bytearray((len(chunk) + 7) // 8)
        for j, b in enumerate(chunk):
            if b: by[j >> 3] |= 1 << (j & 7)
        cmds.append(f'afsk {len(chunk)} {by.hex()}')
    return cmds

# ---------------------------------------------------------------- decode

def fm_audio(z, fs, harmonic=1, chan_hz=None):
    if chan_hz:                                           # channel filter before the discriminator
        from scipy.signal import firwin, lfilter
        z = lfilter(firwin(401, chan_hz, fs=fs), 1, z - z.mean())
    d = np.angle(z[1:] * np.conj(z[:-1])) * fs / (2 * np.pi) / harmonic   # instantaneous frequency, Hz
    return d - np.median(d)

def afsk_bits(audio, fs):
    from scipy.signal import firwin, lfilter
    a = lfilter(firwin(255, [600, 3000], pass_zero=False, fs=fs), 1, audio)
    t = np.arange(len(a)) / fs
    n = int(round(fs / 1200))
    k = np.ones(n) / n
    m = np.abs(np.convolve(a * np.exp(-2j * np.pi * 1200 * t), k, 'same'))
    s = np.abs(np.convolve(a * np.exp(-2j * np.pi * 2200 * t), k, 'same'))
    d = m - s                                            # >0 mark, <0 space
    # clock recovery: sample mid-bit, nudge toward observed transitions
    spb = fs / 1200.0
    out, pos = [], spb / 2
    sign = np.sign(d)
    while pos < len(d) - 1:
        i = int(pos)
        out.append(1 if d[i] > 0 else 0)
        lo, hi = int(pos - spb / 2), int(pos + spb / 2)
        lo, hi = max(lo, 1), min(hi, len(d))
        tr = np.nonzero(sign[lo:hi] != sign[lo - 1:hi - 1])[0]
        if len(tr):
            err = (lo + tr[0]) - (pos - spb / 2)         # transition should sit at the cell edge
            if abs(err) < spb / 2: pos += 0.25 * err
        pos += spb
    return out

def hdlc_frames(tone_bits):
    bits = [1 if tone_bits[i] == tone_bits[i - 1] else 0 for i in range(1, len(tone_bits))]   # NRZI decode
    frames, cur, ones, inframe = [], [], 0, False
    i = 0
    s = ''.join(map(str, bits))
    for seg in s.split('01111110'):
        if len(seg) < 100: continue
        data, ones, ok = [], 0, True
        for ch in seg:
            b = ch == '1'
            if ones == 5:
                if b: ok = False; break
                ones = 0; continue
            data.append(b); ones = ones + 1 if b else 0
        if not ok or len(data) % 8: continue
        by = bytes(sum(data[j + k] << k for k in range(8)) for j in range(0, len(data), 8))
        if len(by) >= 18 and crc16_x25(by[:-2]) == (by[-2] | by[-1] << 8):
            frames.append(by[:-2])
    return frames

def show(fr):
    calls, i = [], 0
    while True:
        a = fr[i:i + 7]
        c = ''.join(chr(x >> 1) for x in a[:6]).strip(); ssid = (a[6] >> 1) & 15
        calls.append(c + (f'-{ssid}' if ssid else ''))
        i += 7
        if a[6] & 1: break
    dest, src, path = calls[0], calls[1], calls[2:]
    return f"{src}>{dest}{''.join(',' + p for p in path)}:{fr[i + 2:].decode(errors='replace')}"

def decode_iq(z, fs, harmonic=1, chan_hz=None):
    if chan_hz is None: chan_hz = harmonic * 3000 + 3500    # deviation x harmonic + audio + margin
    return [show(f) for f in hdlc_frames(afsk_bits(fm_audio(z, fs, harmonic, chan_hz), fs))]

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'encode':
        path = sys.argv[5].split(',') if len(sys.argv) > 5 else []
        tb = tones(frame(sys.argv[2], sys.argv[3], sys.argv[4], path))
        print(len(tb))
        for c in fw_commands(tb): print(c)
    elif cmd == 'decode':
        x = np.fromfile(sys.argv[2], dtype=np.int8).astype(float)
        z = x[0::2] + 1j * x[1::2]
        h = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        res = decode_iq(z, 250000 / 3, h)
        print(f'{len(res)} frame(s) with valid CRC')
        for r in res: print('  ', r)
    elif cmd == 'selftest':
        tb = tones(frame('N0CALL', 'APZ001', '>LR2021 raw IQ APRS test'))
        fs_tx, fs = 38400, 250000 / 3
        n = len(tb) * 32
        tone = np.repeat([1200.0 if b else 2200.0 for b in tb], 32)
        aph = np.cumsum(2 * np.pi * tone / fs_tx)
        ph = np.cumsum(2 * np.pi * 3000 * 3 * np.sin(aph) / fs_tx)       # 3rd harmonic: 9 kHz deviation
        zt = np.exp(1j * ph)
        tt = np.arange(int(n * fs / fs_tx)) / fs
        z = np.interp(tt, np.arange(n) / fs_tx, zt.real) + 1j * np.interp(tt, np.arange(n) / fs_tx, zt.imag)
        z = z * np.exp(2j * np.pi * 400 * tt)                             # carrier offset
        rng = np.random.default_rng(1)
        z = np.concatenate([np.zeros(20000), z, np.zeros(20000)]) + (rng.normal(size=len(z) + 40000) + 1j * rng.normal(size=len(z) + 40000)) * 0.3
        print(len(tb), 'tone bits;', decode_iq(z, fs, 3))

def load_rtl(path, fs, offset_hz):
    """rtl_sdr uint8 IQ; shift the signal at +offset_hz to 0 Hz."""
    x = np.fromfile(path, dtype=np.uint8).astype(float) - 127.5
    z = x[0::2] + 1j * x[1::2]
    return z * np.exp(-2j * np.pi * offset_hz * np.arange(len(z)) / fs)

if __name__ == '__main__' and sys.argv[1] == 'decode_rtl':
    fs, off = float(sys.argv[3]), float(sys.argv[4])
    h = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    res = decode_iq(load_rtl(sys.argv[2], fs, off), fs, h)
    print(f'{len(res)} frame(s) with valid CRC')
    for r in res: print('  ', r)
