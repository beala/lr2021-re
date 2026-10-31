# iq2wav.py <iq8.bin> <rx MHz> <dial MHz> <out.wav> [start s]: USB-demodulate the 83.33 kS/s IQ stream
# to 12 kHz audio for WSJT-X (jt9). The file starts at <start> s into the recording.
import sys, numpy as np, wave
from scipy.signal import resample_poly, firwin, lfilter
x = np.fromfile(sys.argv[1], dtype=np.int8).astype(float); z = x[0::2] + 1j*x[1::2]
fs = 250000/3; rx, dial, out = float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
start = float(sys.argv[5]) if len(sys.argv) > 5 else 0
z = z[int(start*fs):]; z = z - z.mean()
t = np.arange(len(z))/fs
z = z*np.exp(2j*np.pi*(rx - dial)*1e6*t)                      # dial frequency -> 0 Hz
z = z*np.exp(-2j*np.pi*1500*t)                                # centre the 0-3 kHz USB passband
z = lfilter(firwin(801, 1500, fs=fs), 1, z)                   # keep 0..3 kHz only
z = z*np.exp(2j*np.pi*1500*t)
a = np.real(resample_poly(z, 18, 125))                        # 83333.33 -> 12000 S/s
a = a[:12000*15]; a = a/np.max(np.abs(a))*0.5
with wave.open(out, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(12000); w.writeframes((a*32767).astype('<i2').tobytes())
print(f"wrote {out}: {len(a)/12000:.1f} s")
