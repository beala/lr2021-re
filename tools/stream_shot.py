# usage: stream_shot.py <out.bin> "<stream cmd>" [--rak]
# XIAO (receiver) and RAK3401 board (transmitter) both run the console firmware.
# Runs the capture, then fetches it with 'streamdump' (long blocking runs lose their own serial output).
# --rak: the RAK board replays the test advert on the test channel (tools/bench_packet.py, 908 MHz).
import serial, sys, time
from radio_ports import default_port
from bench_packet import TEST_FREQ_MHZ, RADIO, TEST_ADVERT_HEX
out, cmd = sys.argv[1], sys.argv[2]
ms = int(cmd.split()[3])
x = serial.Serial(default_port('XIAO', '/dev/ttyACM0'), 115200, timeout=0.2); time.sleep(0.3); x.read(1 << 20)
rak = None
if '--rak' in sys.argv:
    rak = serial.Serial(default_port('RAK3401', '/dev/ttyACM1'), 115200, timeout=0.3); rak.read(1 << 20)
    for p in (x, rak):
        p.write(f'set radio {TEST_FREQ_MHZ:.3f},{RADIO}\r'.encode()); time.sleep(0.6); p.read(1 << 20)
    rak.write(b'hold on\r'); time.sleep(0.3); rak.read(1 << 20)
x.write(b'hold on\r'); time.sleep(0.3); x.read(1 << 20)
x.write(cmd.encode() + b'\r')
if rak:
    time.sleep(1.0)
    rak.write(f'loratx {TEST_ADVERT_HEX}\r'.encode())
time.sleep(ms / 1000 + 1.5); x.read(1 << 20)
x.write(b'streamdump\r')
buf = b''; end = time.time() + 20
while time.time() < end and b'STREAMBIN' not in buf: buf += x.read(4096)
if b'STREAMBIN' not in buf: print('NO STREAMBIN, got:', buf[-300:]); sys.exit(1)
i = buf.index(b'STREAMBIN'); nl = buf.index(b'\n', i)
print(buf[:i].decode(errors='replace').strip().split('\n')[-1])
n = int(buf[i:nl].split()[1]); data = buf[nl + 1:]
x.timeout = 2
while len(data) < n:
    d = x.read(n - len(data))
    if not d: break
    data += d
open(out, 'wb').write(data[:n]); print('saved', min(len(data), n), 'of', n, 'bytes ->', out)
