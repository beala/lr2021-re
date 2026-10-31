from pathlib import Path
from collections import deque
import time
import unittest

from compare_rx import airtime, compare_packets, decode_capture
from hw_rx import HwReceiver, LineParser, parse_rxstat

LINE = ('RXSTAT len=4 st=0 rssi=-92.5 snr=14.75 raw=11 04 3B 5C 5A 07 FF FE 50 0D gain=13 crc=1 '
        'data=01C0DB00')


class HwRxTests(unittest.TestCase):
    def test_keepalive_never_transmits(self):
        class FakeSerial:
            def __init__(self):
                self.writes = []
                self.replies = deque()

            def write(self, data):
                self.writes.append(data)
                if data.strip():
                    self.replies.append(b'  -> > 910.525,62.5,7,5\r\n')

            def read(self, n):
                time.sleep(.002)
                return self.replies.popleft() if self.replies else b''

        port = FakeSerial()
        receiver = HwReceiver(port, lambda p: None)
        receiver.start()
        try:
            self.assertIn('910.525', receiver.request('get radio', timeout=.2))
            time.sleep(.025)
        finally:
            receiver.close()
        self.assertGreaterEqual(len(port.writes), 2)
        self.assertTrue(all(w in (b'\r', b'get radio\r') for w in port.writes))
        self.assertFalse(any(b'loratx' in w for w in port.writes))

    def test_fragmented_lines_and_startup_noise(self):
        parser = LineParser()
        lines = []
        for byte in b'# LR2021 console on X\r\n\r\n' + LINE.encode() + b'\r\n':
            lines += parser.feed(bytes([byte]))
        self.assertEqual(lines, ['# LR2021 console on X', LINE])
        packet = parse_rxstat(lines[1])
        self.assertEqual(packet['payload'], '01c0db00')
        self.assertEqual((packet['rssi_dbm'], packet['snr_db'], packet['gain']), (-92.5, 14.75, 13))

    def test_crc_failures_are_counted_not_reported(self):
        received = []
        receiver = HwReceiver(None, received.append)
        receiver._line(LINE.replace('crc=1', 'crc=0'), 1)
        receiver._line(LINE, 2)
        self.assertEqual(receiver.crc_errors, 1)
        self.assertEqual(len(received), 1)
        self.assertEqual(received[0]['t'], 2)
        self.assertTrue(received[0]['crc_ok'])

    def test_replies_are_separated_from_packets(self):
        receiver = HwReceiver(None, lambda p: None)
        receiver._line('  -> > rx=3 crc_err=1 hdr_err=2 tx=0', 1)
        receiver._line('hold st=0 on', 1)
        receiver._line(LINE, 1)
        self.assertEqual(list(receiver.replies), ['  -> > rx=3 crc_err=1 hdr_err=2 tx=0', 'hold st=0 on'])


class ComparisonTests(unittest.TestCase):
    def test_airtime_matches_saved_adverts(self):
        self.assertAlmostEqual(airtime(130), .479744)

    def test_duplicates_are_matched_once_and_capture_gaps_are_excluded(self):
        payload = '11' * 130
        rak = [dict(t=t, payload=payload) for t in (1, 2, 3, 4)]
        lr = [dict(t_end=1.01, payload=payload), dict(t_end=3.01, payload=payload),
              dict(t_end=5, payload='22' * 130)]
        captures = [dict(retained_start=0, retained_end=3.5)]
        result = compare_packets(rak, lr, captures)
        self.assertEqual(result['matched'], 2)
        self.assertEqual(result['rak_only_retained_iq'], 1)
        self.assertEqual(result['rak_only_outside_retained_iq'], 1)
        self.assertEqual(result['lr_only'], 1)
        self.assertAlmostEqual(result['lr_miss_fraction_relative_to_rak'], 1 / 3)

    def test_full_packet_not_just_rx_end_must_be_in_iq(self):
        payload = '11' * 130
        result = compare_packets([dict(t=1, payload=payload)], [],
                                 [dict(retained_start=.9, retained_end=1.5)])
        self.assertIsNone(result['lr_miss_fraction_relative_to_rak'])
        self.assertEqual(result['rak_only_outside_retained_iq'], 1)

    def test_timing_guard_excludes_boundary_ambiguity(self):
        payload = '11' * 130
        result = compare_packets([dict(t=1, payload=payload)],
                                 [dict(t_end=1.01, payload=payload)],
                                 [dict(retained_start=0, retained_end=1.01)])
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['matched_near_iq_boundary'], 1)
        self.assertIsNone(result['lr_miss_fraction_relative_to_rak'])

    def test_identical_payload_received_twice_is_not_deduplicated(self):
        payload = 'abcd'
        result = compare_packets([dict(t=1, payload=payload), dict(t=2, payload=payload)],
                                 [dict(t_end=2.01, payload=payload)],
                                 [dict(retained_start=0, retained_end=3)])
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['rak_only_retained_iq'], 1)

    def test_adjacent_identical_transmissions_are_not_false_matches(self):
        # A short packet can finish again ~130 ms later. Opposite receivers
        # hearing different copies must not appear to have received both.
        payload = '11' * 12
        result = compare_packets([dict(t=1, payload=payload)],
                                 [dict(t_end=1.14, payload=payload)],
                                 [dict(retained_start=0, retained_end=2)])
        self.assertEqual(result['matched'], 0)
        self.assertEqual(result['lr_only'], 1)
        self.assertEqual(result['rak_only_retained_iq'], 1)

    def test_multiple_packets_in_one_iq_buffer(self):
        root = Path(__file__).resolve().parents[1]
        data = b''.join((root / 'captures/iter' / name).read_bytes() for name in ('c00.bin', 'c01.bin'))
        attempts = decode_capture(data, 250000 / 3, 7, 62500)
        packets = [a for a in attempts if a.get('crc_ok')]
        self.assertEqual(len(packets), 2)
        self.assertNotEqual(packets[0]['payload'], packets[1]['payload'])
        self.assertLess(packets[0]['packet_end_s'], packets[1]['packet_start_s'])


if __name__ == '__main__':
    unittest.main()
