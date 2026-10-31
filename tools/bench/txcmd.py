# txcmd.py <cmd...>: send rate_bench commands to a board (PORT=..., default the XIAO; one per arg), keep USB alive, print output until each "# ready"
import os, sys, serial, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from radio_ports import default_port
s = serial.Serial(os.environ.get('PORT') or default_port('XIAO', '/dev/ttyACM0'), 115200, timeout=.2)
for c in sys.argv[1:]:
    s.write((c + '\n').encode()); buf = b''; t0 = time.time()
    while b'# ready' not in buf and time.time() - t0 < 60:
        buf += s.read(4096); s.write(b'\n')
    print('\n'.join(l for l in buf.decode(errors='replace').splitlines() if l.strip() and not l.startswith('H,')))
