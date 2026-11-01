#!/usr/bin/env python3
"""MeshCore repeater running on the LR2021's own CPU (firmware/pram/mesh.bin, docs/pram-repeater.md).

The chip receives, decides, waits and transmits by itself. The MCU only loads the blob, starts it
and then leaves its NSS/SCK/MOSI pins floating (the console's `spifree np w<ms>`), because the chip
writes its TX FIFO by driving its own SPI pads. Afterwards the MCU takes SPI back and reads the
chip's counters.

  python3 tools/mesh_rpt.py run --seconds 60                    # repeat on the console's channel
  python3 tools/mesh_rpt.py run --mhz 910.525 --id C7112233 --preamble 32 --seconds 600

MeshCore uses a 32-symbol preamble at SF7-8 (16 from SF9); the console's own `loratx` uses 16, which
is the default here so the bench boards can talk to it. Only transmit where you're allowed to.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pram  # noqa: E402
from radio_ports import default_port  # noqa: E402

BLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'pram', 'mesh.bin')
BW_CODE = {62.5: 3, 125: 4, 250: 5, 500: 6}
NAMES = ['state', 'received', 'crc_errors', 'header_errors', 'floods_forwarded', 'directs_forwarded',
         'duplicates', 'ignored', 'busy', 'tx_done', 'spi_write_failures', 'events', 'last_len',
         'waiting_len', 'waiting_delay_ticks', 'irq']


def mesh(c, op, params=b''):
    st, d = c.rc(0x012D, 64, bytes([1, op]) + params)
    if st != 0:
        raise RuntimeError(f'repeater op {op}: st={st}')
    return dict(zip(NAMES, struct.unpack('<16I', d)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default=default_port('XIAO', '/dev/ttyACM0'))
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--mhz', type=float, default=908.0)
    r.add_argument('--bw', type=float, default=62.5)
    r.add_argument('--sf', type=int, default=7)
    r.add_argument('--cr', type=int, default=5, help='coding rate 4/x, x = 5..8')
    r.add_argument('--dbm', type=int, default=-9)
    r.add_argument('--id', default='C7112233', help='4 bytes hex; the first 1-3 are the path hash')
    r.add_argument('--preamble', type=int, default=16)
    r.add_argument('--flood-factor', type=float, default=0.5, help='MeshCore tx_delay_factor')
    r.add_argument('--direct-factor', type=float, default=0.3, help='MeshCore direct_tx_delay_factor')
    r.add_argument('--hops', type=int, default=64, help='flood hop limit')
    r.add_argument('--half-bit', type=int, default=8, help='SPI pad half-bit delay, CPU cycles')
    r.add_argument('--seconds', type=int, default=60)
    a = ap.parse_args()

    c = pram.Console(a.port)
    for cmd in (f'set radio {a.mhz},{a.bw:g},{a.sf},{a.cr}', f'set tx {a.dbm}'):
        if 'OK' not in c.cmd(cmd, '->', 10):
            raise SystemExit(f'{cmd} failed')
    c.cmd('hold on', 'hold st')
    pram.load(c, BLOB)
    if c.rc(0x012D, 4, b'\x02')[1] != b'MSH1':
        raise SystemExit('repeater blob not active')
    params = bytes.fromhex(a.id) + bytes([a.preamble, a.preamble, a.half_bit, a.sf, BW_CODE[a.bw], a.cr - 4,
                                          round(a.flood_factor * 10), round(a.direct_factor * 10), a.hops])
    mesh(c, 1, params)
    print(f'repeating as {a.id[:2]} on {a.mhz} MHz for {a.seconds} s; SPI pins released')
    c.s.reset_input_buffer()
    c.s.write(f'spifree np w{a.seconds * 1000} 01 2D 01 02\r'.encode())
    end = time.time() + a.seconds + 10
    buf = b''
    while b'spifree bits' not in buf and time.time() < end:
        buf += c.s.read(4096)
    c.rc(0x0100, 2)
    c.rc(0x0100, 2)                   # resynchronise the SPI after the pins come back
    for k, v in mesh(c, 2).items():
        if k not in ('state', 'irq', 'waiting_len', 'waiting_delay_ticks'):
            print(f'  {k:20s} {v}')
    mesh(c, 0)
    c.rc(0x0128, 0, b'\0')


if __name__ == '__main__':
    main()
