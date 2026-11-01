"""LoRa PHY encoder: payload bytes -> symbol values, the reverse of tools/lora_decode.py.

Reference for the on-chip modulator in firmware/pram/rpt.c (docs/pram-repeater.md). Explicit
header, CRC on, no LDRO, sync word 0x12. The bit-level steps are lora_decode's in reverse:
whitening, nibbles, Hamming, diagonal interleave, Gray demapping, symbol + 1.

  encode(payload, sf=7, cr=1) -> header + payload symbols (after the sync downchirps)
  modulate(payload, ...)      -> complex baseband at one sample per chip x os, with preamble
"""
import numpy as np

from lora_decode import _crc16, _whitening


def _inv_gray(g):
    v = 0
    while g:
        v ^= g
        g >>= 1
    return v


def _hamming_enc(n, cr):
    b0, b1, b2, b3 = (n >> 0) & 1, (n >> 1) & 1, (n >> 2) & 1, (n >> 3) & 1
    if cr == 1:
        return (b0 << 4) | (b1 << 3) | (b2 << 2) | (b3 << 1) | (b0 ^ b1 ^ b2 ^ b3)
    p0 = b0 ^ b1 ^ b2
    p1 = b1 ^ b2 ^ b3
    p2 = b0 ^ b1 ^ b3
    p3 = b0 ^ b2 ^ b3
    full = (b0 << 7) | (b1 << 6) | (b2 << 5) | (b3 << 4) | (p0 << 3) | (p1 << 2) | (p2 << 1) | p3
    return full >> (4 - cr)


def _interleave(cws, sf_app, cw_len):
    """sf_app codewords of cw_len bits -> cw_len symbols of sf_app bits (inverse of _deinterleave)."""
    out = []
    for i in range(cw_len):
        v = 0
        for j in range(sf_app):
            bit = (cws[(i - j - 1) % sf_app] >> (cw_len - 1 - i)) & 1
            v = (v << 1) | bit
        out.append(v)
    return out


def header_nibbles(plen, cr, has_crc=True):
    h = [plen >> 4, plen & 0xF, (cr << 1) | int(has_crc)]
    b = lambda v, k: (v >> k) & 1
    c4 = b(h[0], 3) ^ b(h[0], 2) ^ b(h[0], 1) ^ b(h[0], 0)
    c3 = b(h[0], 3) ^ b(h[1], 3) ^ b(h[1], 2) ^ b(h[1], 1) ^ b(h[2], 0)
    c2 = b(h[0], 2) ^ b(h[1], 3) ^ b(h[1], 0) ^ b(h[2], 3) ^ b(h[2], 1)
    c1 = b(h[0], 1) ^ b(h[1], 2) ^ b(h[1], 0) ^ b(h[2], 2) ^ b(h[2], 1) ^ b(h[2], 0)
    c0 = b(h[0], 0) ^ b(h[1], 1) ^ b(h[2], 3) ^ b(h[2], 2) ^ b(h[2], 1) ^ b(h[2], 0)
    return h + [c4, (c3 << 3) | (c2 << 2) | (c1 << 1) | c0]


def encode(payload, sf=7, cr=1):
    N = 1 << sf
    plen = len(payload)
    ws = _whitening(plen)
    nib = []
    for j, byte in enumerate(payload):
        w = byte ^ ws[j]
        nib += [w & 0xF, w >> 4]
    crc = _crc16(payload[:-2]) ^ payload[-1] ^ (payload[-2] << 8) if plen >= 2 else _crc16(payload)
    nib += [crc & 0xF, (crc >> 4) & 0xF, (crc >> 8) & 0xF, (crc >> 12) & 0xF]
    hdr = header_nibbles(plen, cr)
    # header block: sf-2 codewords at CR 4/8; for SF > 7 the rest of it carries payload nibbles
    first = hdr + nib[:sf - 2 - 5]
    nib = nib[sf - 2 - 5:]
    first += [0] * (sf - 2 - len(first))
    syms = [((_inv_gray(v) << 2) + 1) % N for v in _interleave([_hamming_enc(x, 4) for x in first], sf - 2, 8)]
    while nib:
        blk, nib = nib[:sf], nib[sf:]
        blk += [0] * (sf - len(blk))
        syms += [(_inv_gray(v) + 1) % N for v in _interleave([_hamming_enc(x, cr) for x in blk], sf, cr + 4)]
    return syms


def modulate(payload, sf=7, cr=1, preamble=8, sync=(8, 16), os=4, cfo=0.0):
    N = 1 << sf
    n = np.arange(N * os) / os

    def up(s):
        k = (n + s) % N
        return np.exp(2j * np.pi * np.cumsum(k / N - 0.5) / os)

    down = np.conj(up(0))
    parts = [up(0)] * preamble + [up(s) for s in sync] + [down, down, down[:N * os // 4]]
    parts += [up(s) for s in encode(payload, sf, cr)]
    x = np.concatenate(parts)
    return x * np.exp(2j * np.pi * cfo * np.arange(len(x)) / (N * os))


if __name__ == '__main__':
    import sys
    from lora_decode import decode
    rng = np.random.default_rng(2)
    for plen in (2, 5, 12, 37, 100, 255):
        p = bytes(rng.integers(0, 256, plen, dtype=np.uint8))
        x = modulate(p, os=4, cfo=0.3)
        x = np.concatenate([np.zeros(1600), x, np.zeros(1600)])
        x = x + 0.1 * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
        r = decode(x, 250000.0)
        ok = r.get('payload') == p and r.get('crc_ok')
        print(plen, 'OK' if ok else f'FAIL {r}')
        if not ok:
            sys.exit(1)
