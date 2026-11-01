#!/usr/bin/env python3
"""MeshCore repeater running on the LR2021's own CPU (firmware/pram/mesh.bin, docs/pram-repeater.md).

The chip receives, decides, waits and transmits by itself. The MCU only loads the blob, starts it
and then leaves its NSS/SCK/MOSI pins floating (the console's `spifree np w<ms>`), because the chip
writes its TX FIFO by driving its own SPI pads. Afterwards the MCU takes SPI back and reads the
chip's counters.

  python3 tools/mesh_rpt.py run --seconds 60                    # repeat on the console's channel
  python3 tools/mesh_rpt.py run --mhz 910.525 --id C7112233 --preamble 32 --seconds 600
  python3 tools/mesh_rpt.py run --sleep --seconds 600     # the MCU sleeps in delay() meanwhile
  python3 tools/mesh_rpt.py run --sysoff                  # the MCU powers down (System OFF) for good;
                                                          # reset or replug it to get the console back

With --name the repeater also sends a MeshCore advert every --advert-every minutes, so companions
list it by name and show its hash in paths. Adverts are Ed25519-signed: this script signs one with a
key kept in --key (made on first use; --vanity 00 picks a key whose public key starts with 00) and
stores it in the chip's RAM, and the chip resends it. The repeater's path hash is then the first
byte of that public key.

  python3 tools/mesh_rpt.py run --name LR2021-rpt --vanity 00 --preamble 32 --dbm 22 --sysoff

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
         'waiting_len', 'waiting_delay_ticks', 'irq', 'uptime_s', 'adverts_sent', 'traces_forwarded', 'last_snr_x4']
ADV_ADDR = 0x807C00
ADV_TYPE_REPEATER, ADV_NAME_MASK = 2, 0x80


def mesh(c, op, params=b''):
    st, d = c.rc(0x012D, 80, bytes([1, op]) + params)
    if st != 0:
        raise RuntimeError(f'repeater op {op}: st={st}')
    v = dict(zip(NAMES, struct.unpack('<20I', d)))
    v['last_snr_x4'] = struct.unpack('<i', struct.pack('<I', v['last_snr_x4']))[0]
    return v


def load_key(path, vanity):
    """Ed25519 private key from path (raw 32-byte seed), made on first use."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization as ser
    if os.path.exists(path):
        return Ed25519PrivateKey.from_private_bytes(open(path, 'rb').read())
    want = bytes.fromhex(vanity) if vanity else b''
    while True:
        k = Ed25519PrivateKey.generate()
        if pub_of(k).startswith(want):
            break
    with open(path, 'wb') as f:
        f.write(k.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption()))
    os.chmod(path, 0o600)
    return k


def pub_of(k):
    from cryptography.hazmat.primitives import serialization as ser
    return k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)


def advert_packet(key, name):
    """A flood MeshCore advert for a repeater: header, path_len 0, pub key, timestamp, signature,
    app data (type | name flag, name), signed over pub key + timestamp + app data."""
    pub = pub_of(key)
    ts = struct.pack('<I', int(time.time()))
    app = bytes([ADV_TYPE_REPEATER | ADV_NAME_MASK]) + name.encode()[:32]
    sig = key.sign(pub + ts + app)
    return bytes([(4 << 2) | 1, 0]) + pub + ts + sig + app


def store(c, addr, data):
    data = data + bytes(-len(data) % 4)
    for i in range(0, len(data), 4):
        if 'mw st=0' not in c.cmd(f'mw {addr + i:X} {struct.unpack("<I", data[i:i + 4])[0]:X}', 'mw st'):
            raise RuntimeError('RAM write failed')


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
    r.add_argument('--half-bit', type=int, default=32, help='SPI pad half-bit delay, CPU cycles (8 fails on the NiceRF board)')
    r.add_argument('--seconds', type=int, default=60)
    r.add_argument('--sleep', action='store_true', help='the MCU sleeps instead of watching the SPI lines')
    r.add_argument('--sysoff', action='store_true', help='leave the chip repeating and put the MCU into System OFF')
    r.add_argument('--name', help='send a signed MeshCore advert with this name')
    r.add_argument('--key', default=os.path.expanduser('~/.lr2021_repeater.key'), help='Ed25519 seed file')
    r.add_argument('--vanity', default='', help='hex prefix for a new key, e.g. 00')
    r.add_argument('--advert-every', type=int, default=2880, help='minutes between adverts (48 h)')
    a = ap.parse_args()

    c = pram.Console(a.port)
    for cmd in (f'set radio {a.mhz},{a.bw:g},{a.sf},{a.cr}', f'set tx {a.dbm}'):
        if 'OK' not in c.cmd(cmd, '->', 10):
            raise SystemExit(f'{cmd} failed')
    c.cmd('hold on', 'hold st')
    pram.load(c, BLOB)
    if c.rc(0x012D, 4, b'\x02')[1] != b'MSH1':
        raise SystemExit('repeater blob not active')
    key = None
    if a.name:
        key = load_key(a.key, a.vanity)
        a.id = pub_of(key)[:4].hex()
    params = bytes.fromhex(a.id) + bytes([a.preamble, a.preamble, a.half_bit, a.sf, BW_CODE[a.bw], a.cr - 4,
                                          round(a.flood_factor * 10), round(a.direct_factor * 10), a.hops])
    mesh(c, 1, params)
    if key:
        adv = advert_packet(key, a.name)
        store(c, ADV_ADDR, adv)
        mesh(c, 4, bytes([len(adv)]) + struct.pack('>HH', a.advert_every, 3))
        print(f'advert "{a.name}" ({len(adv)} bytes, key {pub_of(key)[:8].hex()}...) in 3 s, then every {a.advert_every} min')
    if a.sysoff:
        print(f'repeating as {a.id[:2]} on {a.mhz} MHz; MCU going to System OFF (reset or replug to wake)')
        c.s.write(b'sysoff\r')
        time.sleep(1)
        return
    print(f'repeating as {a.id[:2]} on {a.mhz} MHz for {a.seconds} s; SPI pins released')
    c.s.reset_input_buffer()
    c.s.write(f'spifree np {"s " if a.sleep else ""}w{a.seconds * 1000} 01 2D 01 02\r'.encode())
    end = time.time() + a.seconds + 10
    buf = b''
    while b'spifree bits' not in buf and time.time() < end:
        buf += c.s.read(4096)
    c.rc(0x0100, 2)
    c.rc(0x0100, 2)                   # resynchronise the SPI after the pins come back
    for k, v in mesh(c, 2).items():
        if k not in ('state', 'irq', 'waiting_len', 'waiting_delay_ticks', 'events'):
            print(f'  {k:20s} {v}')
    mesh(c, 0)
    c.rc(0x0128, 0, b'\0')


if __name__ == '__main__':
    main()
