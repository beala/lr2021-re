"""Receive-only host for the console firmware's hardware LoRa receiver (firmware/src/console).

The reference board runs the LR2021's own LoRa modem and prints one RXSTAT line per packet:
  RXSTAT len=<n> st=<status> rssi=<dBm> snr=<dB> raw=<10 status bytes> gain=<g> crc=<1|0> data=<hex>
Only configuration commands and blank keep-alive lines are sent; this module never sends
"loratx", so it never transmits.
"""
from collections import deque
import re
import threading
import time

RXSTAT = re.compile(r'RXSTAT len=(\d+) st=(-?\d+) rssi=(-?[\d.]+) snr=(-?[\d.]+) raw=((?:[0-9A-F]{2} ){10})'
                    r'gain=(\d+) crc=([01]) data=([0-9A-F]*)')
REPLY = '  -> '


def parse_rxstat(line):
    """RXSTAT line -> packet dict (without time), or None."""
    m = RXSTAT.search(line)
    if not m:
        return None
    return dict(payload=m[8].lower(), crc_ok=m[7] == '1', rssi_dbm=float(m[3]), snr_db=float(m[4]),
                gain=int(m[6]), status_raw=m[5].strip().replace(' ', '').lower())


class LineParser:
    def __init__(self):
        self.buffer = b''

    def feed(self, data):
        self.buffer += data
        lines = []
        while True:
            cut = min((i for i in (self.buffer.find(b'\n'), self.buffer.find(b'\r')) if i >= 0), default=-1)
            if cut < 0:
                break
            line, self.buffer = self.buffer[:cut], self.buffer[cut + 1:]
            if line.strip():
                lines.append(line.decode(errors='replace'))
        if len(self.buffer) > 4096:
            self.buffer = b''
        return lines


class HwReceiver:
    def __init__(self, serial_port, on_packet):
        self.serial = serial_port
        self.on_packet = on_packet
        self.parser = LineParser()
        self.condition = threading.Condition()
        self.replies = deque(maxlen=16)
        self.write_lock = threading.Lock()
        self.stop = threading.Event()
        self.error = None
        self.crc_errors = 0
        self.thread = threading.Thread(target=self._read, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)

    def _line(self, line, now):
        packet = parse_rxstat(line)
        if packet is not None:
            if packet['crc_ok']:
                self.on_packet(dict(packet, t=now))
            else:
                self.crc_errors += 1
        elif line.startswith(REPLY) or line.startswith('hold st='):
            with self.condition:
                self.replies.append(line)
                self.condition.notify_all()

    def _read(self):
        try:
            next_poll = 0
            while not self.stop.is_set():
                now = time.monotonic()
                if now >= next_poll:
                    # nRF52 USB CDC output can stall while the host is silent; blank lines keep it flowing.
                    with self.write_lock:
                        self.serial.write(b'\r')
                    next_poll = now + .01
                data = self.serial.read(4096)
                now = time.monotonic()
                for line in self.parser.feed(data):
                    self._line(line, now)
        except Exception as e:
            self.error = e
            with self.condition:
                self.condition.notify_all()

    def request(self, command, timeout=3):
        deadline = time.monotonic() + timeout
        with self.condition:
            self.replies.clear()
            with self.write_lock:
                self.serial.write((command + '\r').encode())
            while not self.replies:
                if self.error:
                    raise RuntimeError(f'receiver reader failed: {self.error}')
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f'no reply to {command!r}; check the console firmware/port')
                self.condition.wait(remaining)
            reply = self.replies.popleft()
        if 'ERR' in reply:
            raise RuntimeError(f'{command!r} failed: {reply.strip()}')
        return reply

    def configure(self, frequency, bandwidth, sf, cr):
        for attempt in range(3):
            try:
                self.request('get radio', timeout=1)  # allow USB/firmware startup to settle
                break
            except RuntimeError:
                if self.error or attempt == 2:
                    raise
        self.request(f'set radio {frequency / 1e6:.6f},{bandwidth / 1e3:g},{sf},{cr}')  # also starts RX
        reply = self.request('get radio')
        m = re.search(r'>\s+([\d.]+),([\d.]+),(\d+),(\d+)', reply)
        if not m or abs(float(m[1]) * 1e6 - frequency) > 1000 or abs(float(m[2]) * 1e3 - bandwidth) > 1 \
                or int(m[3]) != sf or int(m[4]) != cr:
            raise RuntimeError(f'reference radio configuration mismatch: {reply!r}')

    def stats(self):
        reply = self.request('rxstats')
        fields = dict(re.findall(r'(\w+)=(\d+)', reply))
        if not {'rx', 'crc_err', 'hdr_err', 'tx'} <= fields.keys():
            raise RuntimeError(f'invalid rxstats reply: {reply!r}')
        return dict(rx=int(fields['rx']), tx=int(fields['tx']),
                    read_errors=int(fields['crc_err']) + int(fields['hdr_err']))
