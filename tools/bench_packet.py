"""Test transmissions for the bench tools.

The console firmware can't build MeshCore packets, so the bench tools replay one: a zero-hop
advert from the author's own test node ("RAK3401 1W Repeater", no location), the same packet as
the IQ test vectors in captures/iter/. Bench transmissions default to 908 MHz, away from the
US MeshCore channel (910.525 MHz), into dummy loads.
"""
TEST_FREQ_MHZ = 908.0
RADIO = '62.5,7,5'  # BW kHz, SF, CR: what the software decoder and IQ8 transport expect

TEST_ADVERT_HEX = (
    '1200aac8a2275ac71e9edcec1b60fc279836bd72f118fbe05bc2a538b5874e32fd3a4ba344661aa779d6c239d5dd55a3'
    'f26be5540bd3e1b7a63da76556e4113d382e7c73465f388a389512de8e2460d0e3e1402bb18fc0fc7212952782f2dc70'
    'bc2b1e28720792000000000000000052414b33343031203157205265706561746572'
)


def setup_pair(command, rx, tx, freq_mhz=TEST_FREQ_MHZ):
    """Put both console boards on the test channel, receiver held for IQ capture."""
    for port in (rx, tx):
        reply = command(port, f'set radio {freq_mhz:.3f},{RADIO}')
        if 'OK' not in reply:
            raise RuntimeError(f'{port.port}: set radio failed: {reply}')
    command(rx, 'hold on')
    command(tx, 'hold on')


def send_advert(command, tx):
    reply = command(tx, 'loratx ' + TEST_ADVERT_HEX)
    if 'OK' not in reply:
        raise RuntimeError(f'test transmission failed: {reply}')
    return reply
