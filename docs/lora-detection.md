# LoRa sync word and detection commands [HW]

Four undocumented commands write the LoRa modem's sync and detection registers. None of them is
in the datasheet, and Semtech's `usp` v1.2.1 driver doesn't use them either: it lists the LoRa
opcodes 0x021E–0x022C but skips 0x021F and 0x0226. This page decodes them from the ROM and
measures them between the two bench boards. All transmissions: 908 MHz, −9 dBm drive, dummy
loads, XIAO → NiceRF. Reproduce with `tools/lora_per.py`:

```
python3 tools/lora_per.py --bw 62.5 --sf 7 --tx-rc 0223:42 --rx-rc 0223:12    # 0x42 into a 0x12 receiver
python3 tools/lora_per.py --tx-rc 022D:0205 --rx-rc 022D:0205                 # raw (odd) sync symbols
```

| Opcode | Params | What it does | Status |
|---|---|---|---|
| 0x022D | `s1 s2` | **Raw LoRa sync word.** Writes two 5-bit sync symbol values to 0xF30A68 [4:0] and [12:8], shadow at RAM 0x800306 | [HW] |
| 0x022E | up to 3 × `s1 s2` | **Raw side-detector sync words.** Same encoding into 0xF30B00/04/08 [19:15] and [14:10], shadow at 0x800430. Default 0x0204 each | [RE] |
| 0x021F | `p0 p1` | Writes 13 bits of 0xF30A30 [28:16], shadow at RAM 0x8003E0. Bits [27:24] act in the payload demodulator | [HW] partly |
| 0x0226 | ≥ 6 bytes | Fills the configuration block next to SetLoraCadParams' and sets enable bit 0xF30A1C[17]. No effect on ordinary packets | [RE] |

## 0x022D: raw sync word

SetLoraSyncword (0x0223) takes one byte and calls the same ROM function (0x95EC) with each nibble
doubled: 0x12 becomes symbol values (2, 4), 0x34 becomes (6, 8). The register read back at boot is
0x48A1**04****02**, i.e. (2, 4). 0x022D takes the two values directly, so it also accepts odd
values: 32 × 32 combinations instead of 16 × 16.

- (2, 4) through 0x022D behaves exactly like 0x0223 0x12.
- Odd values work: TX and RX both on (2, 5) received 20/20.
- The setting survives RadioLib's transmit and receive restarts (it only rewrites the register on
  SetLoraSyncword).

## The sync word filters less than you'd think

The surprise from the 0x022D sweep was that the receiver hardly checks the sync word. Measured with
the documented command, receiver on 0x12, BW 62.5 kHz SF7 (MeshCore's settings), SNR +5 dB:

| TX sync word | Received |
|---|---|
| 0x12 | 100/100 |
| **0x22, 0x32, 0x72, 0xF2** | **100/100, 50/50, 50/50, 50/50** |
| 0xA2 | 49/50 |
| 0x02 | 0/50 |
| 0x11, 0x13, 0x14, 0x15, 0x18 | 58 %, 65 %, 57 %, 88 %, 82 % |
| 0x10, 0x1F | 12 %, 6 % |
| 0x34, 0x2B, 0xF5 | 2 % each |
| 0x56 | 6 % |
| 0x21 | 0/100 |

- **The first nibble is ignored**, as long as it isn't 0. A first symbol of 0 is the same chirp as
  the preamble, so the receiver probably counts it as one more preamble symbol and loses the frame.
  The raw sweep agrees: at BW 125 kHz every first symbol from 1 to 31 was received 10/10 with the
  second symbol at 4.
- **The second nibble filters only partly**, and not smoothly: values near 2 get through 57–88 % of
  the time, far ones 2–12 %.
- The rejections are silent: no header or CRC errors, the packet is just not reported.
- It depends on the setting. At BW 125 kHz SF7 (SNR +2.6 dB), 0x34 into 0x12 got through 9/20, and
  the raw second-symbol sweep was received 50–100 % for every value from 1 to 31.

So on this chip the sync word is a weak filter. Two networks on the same frequency, bandwidth and SF
are separated only if their sync words differ in the second nibble, and even then only partly.
Different SF, bandwidth or IQ inversion separate them; the sync word mostly doesn't.

None of the other bits in 0xF30A68 tightens the check. Bits 5–7 and 13–15 aren't implemented (they
read back 0), and toggling each of bits 16–31 changed nothing: 0x42 still reached a 0x12 receiver
15/15 for every bit.

## 0x022E: raw side-detector sync words

SetLoraSideDetSyncword (0x0225) doubles each nibble the same way and calls ROM 0x8B54, which writes
the three side detectors' registers 0xF30B00/04/08. 0x022E is its raw form: up to three
big-endian u16 values, `s1` in the high byte. Not run on hardware, since it needs side detectors
configured; by the same code path it should behave like 0x022D.

## 0x021F: 0xF30A30 [28:16]

The two parameter bytes are scattered into the register: `p0[1:0]` → [23:22], `p0[7:6]` → [21:20],
`p0[5:2]` → [19:16], `p1[3:0]` → [27:24], `p1[4]` → [28]. The field is 0 after reset. No documented
command touches it. The wake-up restore (ROM 0x8916) rewrites it from the RAM shadow.

Setting one field at a time on the receiver:

| Field | Effect |
|---|---|
| [27:24] = 1 | BW 125 SF7: 3/20 received, 15 CRC errors, reported SNR −3.8 dB instead of +2.6 |
| [27:24] = 2 | 19/20, 1 CRC error, SNR −0.9 dB |
| [27:24] = 3, 4 | BW 500 SF5 at the sensitivity edge: 56 % and 73 % against 88 % |
| [27:24] ≥ 6 | same as 0: BW 500 SF5, 400 packets each, 0 / 7 / 8 / 10 → 90 / 88 / 90 / 87 % |
| [23:22], [21:20], [19:16], [28] | no change at BW 125 SF7 or BW 500 SF5 |

The header always decodes and the CRC errors come with a lower SNR estimate, so [27:24] acts in
the payload demodulator, maybe as a tracking-loop constant. 0 is as good as any value: there is
nothing to gain from this command for normal reception. It also had no effect on sync word
filtering (0x34 into 0x12 was the same for every field).

## 0x0226: an extra CAD-style configuration

The handler needs at least 6 parameter bytes and accepts 8 or 10:

| Bytes | Meaning (from the ROM) |
|---|---|
| p0 | bit 7 → enable bit 0xF30A1C[17]; bits 2:0 a mode; bit 7 also selects the constant 0xC3FC for one field |
| p1 p2 | u16 |
| p3 p4 p5 | 24-bit value (big-endian, like the CAD timeout) |
| p6, p7, p8, p9 | optional small parameters (p7 as two nibbles); with only 6 bytes the ROM uses 25, 6 and 5, 10, 25 |

It calls ROM 0x9558, which fills the block at RAM 0x8002F0, directly after SetLoraCadParams' block at
0x8002EC (`cad_symb_nb` at +0x1C, `pnr_delta`, exit mode, 24-bit timeout and detect peak). After
reset the 0x0226 block holds uninitialised RAM, so the ROM doesn't use it by default. On hardware,
enabling it (`80 00 00 00 00 00`, `81 00 10 00 10 00`) on either board set 0xF30A1C bit 17 and left
normal packets unchanged (20/20, same SNR). It probably belongs to CAD or listen-before-talk; not
tested further.

## Bench notes

- The console boards must have their receiver's serial port drained while packets arrive. The host
  tty buffer holds about 40 RXSTAT lines; after that the board drops output and the run looks like
  packet loss. `tools/lora_per.py` reads the receiver after every packet.
- The link is leakage between dummy loads: about −115 dBm at the NiceRF, SNR +5 dB at BW 62.5 kHz,
  +2.6 dB at 125 kHz and −3.4 dB at 500 kHz. BW 500 SF5 sits at the sensitivity edge (about 90 %
  delivery), which makes it the setting for spotting receiver changes.
