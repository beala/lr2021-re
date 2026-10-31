# rec.py <port> <seconds> <out.bin>: record a checked LRIQ stream from the iqstream command (rate_bench or console)
import sys, time, serial
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from iq_stream import FrameParser, StreamValidator, DATA
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
p, v = FrameParser(), StreamValidator()
with serial.Serial(port, 115200, timeout=.02, exclusive=True) as s, open(out, 'wb') as f:
    s.write(b'\n'); time.sleep(.3); s.read(65536)
    s.write(f'iqstream {int(secs*1000)} 16 3 8 250000\n'.encode())
    t0 = time.monotonic(); last = t0
    while not v.ended:
        if time.monotonic() - last > .1: s.write(b'\n'); last = time.monotonic()
        for fr in p.feed(s.read(16384)):
            v.accept(fr)
            if fr.kind == DATA: f.write(fr.payload)
        if time.monotonic() - t0 > secs + 10: raise SystemExit('stream did not end')
st = v.final_stats
print(f"recorded {v.samples} IQ pairs at {v.raw_rate/v.decimation:.1f} S/s ({v.samples*v.decimation/v.raw_rate:.2f} s), max FIFO {st['max_fifo']}, flags 0x{v.final_flags:02x}")
