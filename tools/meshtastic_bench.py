#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bench test of the chip's Meshtastic node (docs/pram-meshtastic.md): one board runs it
(meshtastic_node.py run), the other plays the rest of the mesh, sending packets with `loratx` and
checking what the chip relays.

  python3 tools/meshtastic_bench.py --node-port <XIAO> --mesh-port <NiceRF>
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pram  # noqa: E402
import meshtastic_node as m  # noqa: E402
from radio_ports import default_port  # noqa: E402

ME, SRC = 0xC0FFEE42, 0xA1B2C3D4


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--node-port', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--mesh-port', default=default_port('RAK', '/dev/ttyACM1'))
    ap.add_argument('--dbm', type=int, default=-9)
    a = ap.parse_args()

    mesh = pram.Console(a.mesh_port)

    class R:
        preset, mhz, dbm = 'LongFast', None, a.dbm
    m.radio_setup(mesh, R)
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'meshtastic_node.py')
    node = subprocess.Popen([sys.executable, '-u', tool, '--port', a.node_port, '--dbm', str(a.dbm), 'run',
                             '--node', f'{ME:08x}', '--name', 'LR2021 PRAM', '--short', 'PRAM', '--seconds', '75'],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in node.stdout:
        print('node:', line.rstrip(), flush=True)
        if 'SPI pins released' in line:
            break
    t0 = time.time()
    base = struct.unpack('<I', os.urandom(4))[0] & 0xFFFF0000 | 0x100

    def heard(sec):
        out, buf, end = [], b'', time.time() + sec
        while time.time() < end:
            buf += mesh.read_for(0.3)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                line = line.decode(errors='replace')
                if line.startswith('RXSTAT') and 'crc=1' in line:
                    f = dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv)
                    d = m.decode(bytes.fromhex(f['data']))
                    if d:
                        print(f'  {time.time() - t0:5.1f}s heard id {d["id"]:08x} hop {d["hop_limit"]}/{d["hop_start"]} '
                              f'relay {d["relay"]:02x} {d.get("text") or d.get("user")}', flush=True)
                        out.append(d)
        return out

    def send(n, text, hops=3, hop_start=3, to=0xFFFFFFFF, nh=0, relay=None):
        p = bytearray(m.packet(SRC, to, base + n, m.PORT_TEXT, text.encode(), hops))
        p[12] = hops | hop_start << 5
        p[14] = nh
        if relay is not None:
            p[15] = relay
        mesh.cmd(f'loratx {bytes(p).hex()}', '->', 10)
        print(f'{time.time() - t0:5.1f}s sent id {base + n:08x} hop {hops}/{hop_start} nh {nh:02x} '
              f'to {to:08x} {text!r}', flush=True)

    def relays(h, n):
        return [d for d in h if d['sender'] == SRC and d['id'] == base + n and d['relay'] == ME & 0xFF]

    results = []

    def check(name, ok):
        results.append(ok)
        print(f'{"PASS" if ok else "FAIL"}  {name}', flush=True)

    h = heard(8)
    check('NodeInfo sent, decrypts', any(d.get('user', {}).get('long') == 'LR2021 PRAM' for d in h))
    send(1, 'one')
    h = heard(7)
    check('new broadcast relayed once, hop 2, text intact',
          [(d['hop_limit'], d.get('text')) for d in relays(h, 1)] == [(2, 'one')])
    send(2, 'two')
    send(2, 'two', hops=2, relay=0x77)
    h = heard(7)
    check('relay dropped after another node relayed it', not relays(h, 2))
    send(3, 'three', nh=0x55)
    h = heard(6)
    send(4, 'four', nh=ME & 0xFF)
    h += heard(7)
    check('next hop: someone else not relayed, ours relayed', not relays(h, 3) and len(relays(h, 4)) == 1)
    send(5, 'five', to=ME)
    send(6, 'six', hops=0)
    h = heard(6)
    check('to us and hop 0 not relayed', not relays(h, 5) and not relays(h, 6))
    send(7, 'seven', hops=1, relay=0x66)
    send(7, 'seven', hops=3)
    h = heard(7)
    check('upgrade: one relay, with hop 2', [d['hop_limit'] for d in relays(h, 7)] == [2])
    send(7, 'seven', hops=3)
    h = heard(7)
    check('originator repeat relayed again', [d['hop_limit'] for d in relays(h, 7)] == [2])
    out = node.communicate()[0]
    print(out)
    print(f'{sum(results)}/{len(results)} passed')


if __name__ == '__main__':
    main()
