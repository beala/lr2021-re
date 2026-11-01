#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Meshtastic node running on the LR2021's own CPU (firmware/pram/meshtastic/mt.bin,
docs/pram-meshtastic.md). Packet layout, channel hash, key, nonce and slot/frequency rules follow the
Meshtastic firmware (GPL-3.0), so this script is GPL-3.0 too.

  python3 tools/meshtastic_node.py run --seconds 600                 # relay on US LongFast
  python3 tools/meshtastic_node.py run --name "LR2021 PRAM" --dbm 22 --seconds 3600
  python3 tools/meshtastic_node.py send --port <other board> "hello"  # a test text message
  python3 tools/meshtastic_node.py listen --port <other board> --seconds 60

`run` loads the blob, starts it and lets go of the SPI pins (the chip sends through its own SPI
pads, as the MeshCore repeater does). With --name the host also encrypts a few NodeInfo packets
with the channel key, the chip sends one every --nodeinfo-every minutes, and phones list the node.
The chip can't decrypt, so it relays from the outer header only and can't be messaged; its NodeInfo
says so (is_unmessagable). Only transmit where you're allowed to.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pram  # noqa: E402
from radio_ports import default_port  # noqa: E402

BLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'pram', 'meshtastic', 'mt.bin')
BW_CODE = {62.5: 3, 125: 4, 250: 5, 500: 6}
# name: (BW kHz, SF, CR 4/x), RadioInterface.cpp modemPresetToParams
PRESETS = {'ShortTurbo': (500, 7, 5), 'ShortFast': (250, 7, 5), 'ShortSlow': (250, 8, 5),
           'MediumFast': (250, 9, 5), 'MediumSlow': (250, 10, 5), 'LongFast': (250, 11, 5),
           'LongModerate': (125, 11, 8), 'LongSlow': (125, 12, 8)}
DEFAULT_PSK = bytes.fromhex('d4f1bb3a20290759f0bcffabcf4e6901')   # Channels.h defaultpsk ("AQ==")
SYNC = 0x2B
NAMES = ['state', 'received', 'crc_errors', 'header_errors', 'relayed', 'duplicates', 'relays_dropped',
         'upgrades', 'ignored', 'busy', 'tx_done', 'spi_write_failures', 'events', 'last_len',
         'queue_bytes', 'next_tx_ticks', 'irq', 'uptime_s', 'nodeinfo_sent', 'last_snr_x4']
NI_ADDR, NI_MAX = 0x807D00, 256
PORT_TEXT, PORT_NODEINFO = 1, 4
HW_PRIVATE = 255


def djb2(s):
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def us_freq(preset):
    """Default US frequency for a preset's primary channel (RadioInterface.cpp: slot = hash(name) %
    slots, 902 MHz + BW/2 + slot x BW)."""
    bw = PRESETS[preset][0] / 1000
    slots = round(26 / bw)
    return 902 + bw / 2 + (djb2(preset) % slots) * bw


def channel_hash(name, psk=DEFAULT_PSK):
    h = 0
    for b in name.encode() + psk:
        h ^= b
    return h


def ctr(psk, sender, pid, data):
    """Meshtastic channel encryption: AES-CTR, nonce = id (8 bytes LE) | from (4 bytes LE) | 0."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    nonce = struct.pack('<QI', pid, sender) + bytes(4)
    c = Cipher(algorithms.AES(psk), modes.CTR(nonce)).encryptor()
    return c.update(data) + c.finalize()


def varint(v):
    out = b''
    while True:
        b = v & 0x7F
        v >>= 7
        out += bytes([b | (0x80 if v else 0)])
        if not v:
            return out


def field(n, v):
    if isinstance(v, int):
        return varint(n << 3) + varint(v)
    if isinstance(v, str):
        v = v.encode()
    return varint(n << 3 | 2) + varint(len(v)) + v


def parse_pb(b):
    """{field: value} of a protobuf message (varints and length-delimited only, last one wins)."""
    out, i = {}, 0
    while i < len(b):
        key, i = _rv(b, i)
        n, t = key >> 3, key & 7
        if t == 0:
            out[n], i = _rv(b, i)
        elif t == 2:
            ln, i = _rv(b, i)
            out[n], i = b[i:i + ln], i + ln
        elif t == 5:
            out[n], i = struct.unpack_from('<I', b, i)[0], i + 4
        elif t == 1:
            out[n], i = struct.unpack_from('<Q', b, i)[0], i + 8
        else:
            raise ValueError('bad wire type')
    return out


def _rv(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, i


def packet(sender, to, pid, port, payload, hops=3, channel='LongFast', psk=DEFAULT_PSK, relay=None):
    """A Meshtastic LoRa packet: 16-byte header (RadioInterface.h PacketHeader) + encrypted Data."""
    data = field(1, port) + field(2, payload)
    flags = hops | hops << 5
    relay = sender & 0xFF if relay is None else relay
    hdr = struct.pack('<IIIBBBB', to, sender, pid, flags, channel_hash(channel, psk), 0, relay)
    return hdr + ctr(psk, sender, pid, data)


def nodeinfo(sender, long_name, short_name, pid, hops=3, channel='LongFast'):
    user = (field(1, f'!{sender:08x}') + field(2, long_name) + field(3, short_name) + field(5, HW_PRIVATE)
            + field(9, 1))                        # is_unmessagable: the chip can't decrypt
    return packet(sender, 0xFFFFFFFF, pid, PORT_NODEINFO, user, hops, channel)


def decode(p, psk=DEFAULT_PSK):
    """Header fields, and the Data message when the default key decrypts it."""
    if len(p) < 16:
        return None
    to, sender, pid, flags, ch, nh, relay = struct.unpack_from('<IIIBBBB', p)
    d = dict(to=to, sender=sender, id=pid, hop_limit=flags & 7, want_ack=flags >> 3 & 1,
             hop_start=flags >> 5, channel=ch, next_hop=nh, relay=relay)
    try:
        data = parse_pb(ctr(psk, sender, pid, p[16:]))
        if 1 in data and isinstance(data[1], int) and 0 < data[1] < 512:
            d['port'] = data[1]
            pl = data.get(2, b'')
            if data[1] == PORT_TEXT:
                d['text'] = pl.decode(errors='replace')
            elif data[1] == PORT_NODEINFO:
                u = parse_pb(pl)
                d['user'] = {k: u.get(n, b'').decode(errors='replace') for k, n in (('id', 1), ('long', 2), ('short', 3))}
    except (ValueError, IndexError, KeyError):
        pass
    return d


def node_cmd(c, op, params=b''):
    st, d = c.rc(0x012D, 80, bytes([1, op]) + params)
    if st != 0:
        raise RuntimeError(f'node op {op}: st={st}')
    v = dict(zip(NAMES, struct.unpack('<20I', d)))
    v['last_snr_x4'] = struct.unpack('<i', struct.pack('<I', v['last_snr_x4']))[0]
    return v


def store(c, addr, data):
    data = data + bytes(-len(data) % 4)
    for i in range(0, len(data), 4):
        if 'mw st=0' not in c.cmd(f'mw {addr + i:X} {struct.unpack("<I", data[i:i + 4])[0]:X}', 'mw st'):
            raise RuntimeError('RAM write failed')


def node_number(path):
    if not os.path.exists(path):
        n = struct.unpack('<I', os.urandom(4))[0] | 1
        with open(path, 'w') as f:
            f.write(f'{n:08x}\n')
    return int(open(path).read().strip(), 16)


def radio_setup(c, a):
    bw, sf, cr = PRESETS[a.preset]
    mhz = a.mhz or us_freq(a.preset)
    for cmd in (f'set radio {mhz:.4f},{bw:g},{sf},{cr}', f'set sync {SYNC:02X}', f'set tx {a.dbm}'):
        if 'OK' not in c.cmd(cmd, '->', 10):
            raise SystemExit(f'{cmd} failed')
    return mhz, bw, sf


def cmd_run(a):
    c = pram.Console(a.port)
    mhz, bw, sf = radio_setup(c, a)
    me = int(a.node, 16) if a.node else node_number(a.node_file)
    c.cmd('hold on', 'hold st')
    pram.load(c, BLOB)
    if c.rc(0x012D, 4, b'\x02')[1] != b'MTS1':
        raise SystemExit('Meshtastic blob not active')
    params = struct.pack('>I', me) + bytes([16, 16, a.half_bit, sf, BW_CODE[bw], 1 if a.router else 0])
    node_cmd(c, 1, params)
    if a.name:
        short = a.short or f'{me & 0xFFFF:04x}'
        pkts, blob = [], b''
        while len(pkts) < a.nodeinfo_copies:
            p = nodeinfo(me, a.name, short, struct.unpack('<I', os.urandom(4))[0] | 1, channel=a.preset)
            if len(blob) + 1 + len(p) > NI_MAX:
                break
            pkts.append(p)
            blob += bytes([len(p)]) + p
        if not pkts:
            raise SystemExit('NodeInfo too long: shorten --name')
        store(c, NI_ADDR, blob)
        node_cmd(c, 4, bytes([len(pkts)]) + struct.pack('>HH', a.nodeinfo_every, 3))
        print(f'NodeInfo "{a.name}" ({short}, {len(pkts)} copies of {len(pkts[0])} bytes) in 3 s, '
              f'then every {a.nodeinfo_every} min')
    print(f'node !{me:08x} on {mhz:.3f} MHz {a.preset} for {a.seconds} s; SPI pins released', flush=True)
    out = pram.spifree(c, f'np {"s " if a.sleep else ""}w{a.seconds * 1000} 01 2D 01 02', a.seconds + 10)
    line = next((ln for ln in out.splitlines() if ln.startswith('spifree st=')), '')
    if not line.startswith('spifree st=0'):
        print(f'  MCU: {line or "no spifree reply"}')
    c.rc(0x0100, 2)
    c.rc(0x0100, 2)                   # resynchronise the SPI after the pins come back
    for k, v in node_cmd(c, 2).items():
        if k not in ('state', 'irq', 'queue_bytes', 'next_tx_ticks', 'events'):
            print(f'  {k:20s} {v}')
    if not a.keep:
        node_cmd(c, 0)
        c.rc(0x0128, 0, b'\0')


def cmd_send(a):
    c = pram.Console(a.port)
    radio_setup(c, a)
    sender = int(a.sender, 16)
    pid = int(a.id, 16) if a.id else struct.unpack('<I', os.urandom(4))[0] | 1
    p = packet(sender, int(a.to, 16), pid, PORT_TEXT, a.text.encode(), a.hops, a.preset)
    out = c.cmd(f'loratx {p.hex()}', '->', 10)
    print(f'sent !{sender:08x} id {pid:08x} hops {a.hops}: {out.strip()}')


def cmd_listen(a):
    c = pram.Console(a.port)
    radio_setup(c, a)
    end, buf = time.time() + a.seconds, b''
    while time.time() < end:
        buf += c.read_for(min(1, end - time.time()))
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.decode(errors='replace').strip()
            if not line.startswith('RXSTAT') or 'crc=1' not in line:
                continue
            f = dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv)
            d = decode(bytes.fromhex(f['data']))
            if not d:
                continue
            extra = d.get('text') and f' text={d["text"]!r}' or d.get('user') and f' user={d["user"]}' or \
                (f' port={d["port"]}' if 'port' in d else ' (other channel)')
            print(f'{time.strftime("%H:%M:%S")} snr={f["snr"]:>6} !{d["sender"]:08x} -> {d["to"]:08x} id {d["id"]:08x} '
                  f'hop {d["hop_limit"]}/{d["hop_start"]} ch {d["channel"]:02x} nh {d["next_hop"]:02x} '
                  f'relay {d["relay"]:02x}{extra}', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--preset', default='LongFast', choices=PRESETS)
    ap.add_argument('--mhz', type=float, help='default: the preset\'s US frequency (LongFast 906.875)')
    ap.add_argument('--dbm', type=int, default=-9)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--node', help='node number, hex (default: kept in --node-file)')
    r.add_argument('--node-file', default=os.path.expanduser('~/.lr2021_meshtastic_node'))
    r.add_argument('--name', help='send NodeInfo with this long name')
    r.add_argument('--short', help='short name (<= 4 characters; default: last 4 hex digits)')
    r.add_argument('--nodeinfo-every', type=int, default=180, help='minutes (Meshtastic: 3 h)')
    r.add_argument('--nodeinfo-copies', type=int, default=4, help='different packet ids to rotate through')
    r.add_argument('--router', action='store_true', help='ROUTER timing: relay early, never drop a queued relay')
    r.add_argument('--half-bit', type=int, default=32, help='SPI pad half-bit delay, CPU cycles')
    r.add_argument('--seconds', type=int, default=60)
    r.add_argument('--sleep', action='store_true', help='the MCU sleeps instead of watching the SPI lines')
    r.add_argument('--keep', action='store_true', help='leave the node running on the chip afterwards')
    s = sub.add_parser('send')
    s.add_argument('text')
    s.add_argument('--sender', default='a1b2c3d4', help='hex node number')
    s.add_argument('--to', default='ffffffff')
    s.add_argument('--id', help='hex packet id (default random)')
    s.add_argument('--hops', type=int, default=3)
    li = sub.add_parser('listen')
    li.add_argument('--seconds', type=int, default=60)
    a = ap.parse_args()
    {'run': cmd_run, 'send': cmd_send, 'listen': cmd_listen}[a.cmd](a)


if __name__ == '__main__':
    main()
