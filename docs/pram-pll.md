# FM from the chip: writing the synthesizer directly [HW]

The CPU can't feed the TX FIFO ([raw-iq.md](raw-iq.md#raw-tx-0x0293-and-the-tx-fifo-hw)), but it
can write the synthesizer. Put the radio in a CW carrier, then have PRAM code write the PLL
frequency word in a loop: the carrier follows each write within a couple of microseconds. With
nothing streamed from the host, the chip then generates any FM signal up to ~100 kHz of
modulation bandwidth: FSK, AFSK, a sine tone, a sweep. The blob `firmware/pram/pll.bin` (source
`pll.c`) does it, and it sends an APRS frame that decodes from an RTL-SDR on both boards.

```
python3 tools/pll.py aprs --sdr                          # AFSK 1200 APRS frame made on the chip, decoded
python3 tools/pll.py fsk --dev 5000 --us 1000 --n 400    # +-5 kHz square wave, 1 ms symbols
python3 tools/pll.py tone --khz 1 --dev 3000             # sine FM from a table
python3 tools/pll.py sweep --span 200 --ms 300           # linear sweep over 200 kHz
```

All measurements: 908 MHz, −9 dBm drive, into dummy loads, received with an RTL-SDR Blog V4 tuned
50 kHz below the carrier (it reads about 150 Hz low on its own). The APRS frame uses the N0CALL
placeholder.

## The register

| Address | Contents |
|---|---|
| 0xF40144 | Synthesizer frequency word = Hz × 2²⁰ / 10⁶ (0.95 Hz per LSB). 908 MHz = 0x38C00000 |
| 0xF40140 | Bit 31: strobe pulsed by the ROM after a retune; not needed for a write to take effect. [13:10]: band bits |
| 0xF40100 | [11:8]: band bits (same value as 0xF40140) |

SetRfFrequency (0x0200, handler 0xFC00) converts Hz to the word (0x6AF0: `(f << 14) / 15625`) and
calls 0xFAE8. In standby that writes 0xF40144 and the band bits directly. When the synthesizer is
already running it calls 0xC098 instead: clear 0xF40114 bit 7, write 0xF40144, pulse 0xF40140 bit
31, and either wait (jumps under 0x500000, about 5 MHz) or run a recalibration (larger jumps).

A plain write of 0xF40144 during CW is enough: the carrier moved exactly +10 kHz, with or without
the strobe. Large jumps (over ~5 MHz, where the ROM recalibrates) weren't tried.

## Measurements

| What | Result |
|---|---|
| Step response | follows within ~2 µs (the SDR's resolution); ~25 % overshoot, settled in 15–20 µs. Same shape for ±25 kHz and ±100 kHz steps |
| Large steps | ±500 kHz and ±900 kHz square waves land on frequency; output level unchanged |
| Symbol timing | 400 symbols of 32,000 + 32 cycles: 1.0012 ms each, as computed at 32.0 MHz |
| Fastest update | ~32 cycles per write in the C loop, so ~1 M updates/s. A ±100 kHz toggle every 1 µs averages out to the centre frequency |

FM response, sine table played by the chip, β = 0.5, deviation recovered from the carrier and
first-sideband levels (J₁/J₀):

| Modulating tone | Delivered deviation |
|---|---|
| 1 kHz | −0.3 dB |
| 5 kHz | +0.3 dB |
| 20 kHz | +0.9 dB |
| 50 kHz | +1.2 dB |
| 100 kHz | −0.3 dB |
| 200 kHz | −5.4 dB (4 samples per cycle) |
| 400 kHz | −16.8 dB (2 samples per cycle) |

Flat within ±1 dB to 100 kHz, with the slight peak the overshoot predicts. That covers AFSK and
APRS, POCSAG, narrowband FM voice, RTTY and GFSK up to ~100 kbps.

## APRS from the chip

Entry 3 generates AFSK 1200 (Bell 202): mark 1200 Hz, space 2200 Hz, phase continuous, through a
256-entry sine table, 38,400 PLL updates per second (32 per bit), ±3 kHz deviation. The host
builds the AX.25 frame with `tools/bench/aprs.py` (CRC, bit stuffing, flags, NRZI), writes the
tone bits to RAM at 0x807800 and calls the entry. It reuses the same decoder as the 2 m raw-IQ
test ([rate-bench.md](rate-bench.md)).

- Calibration: with 808 delay cycles per sample the mark tone measured 1200.2 Hz, so each sample
  costs ~25 cycles of loop on top of the delay.
- `N0CALL>APZ001:>LR2021 PLL AFSK test` (585 tone bits, 0.49 s with 30 flags of preamble):
  decoded with a valid CRC 3/3 on the XIAO and 2/2 on the NiceRF board, plus once more each through
  `tools/pll.py`.

Unlike the raw-IQ path, nothing is streamed: the whole packet is 74 bytes of tone bits in RAM.

## Entries

| Entry | Params (big-endian) | What it does |
|---|---|---|
| 1 | n (2), cycles per symbol (4), mode (1), step (4, signed, word units), table length (2, optional) | For each symbol write base + v × step, then wait. Mode 0: v = ±1 alternating. Mode 1: v = int8 table at 0x807800, wrapping at the table length (≤ 2048). Mode 2: v = i − n/2 (sweep) |
| 2 | – | Returns `PLL1` |
| 3 | nbits (2), cycles per sample (4), samples per bit (2), step (4), mark increment (4), space increment (4) | AFSK from tone bits at 0x807800 (LSB first, 1 = mark). Increment = f_tone / f_sample × 2³² |

Both return the base word (the value of 0xF40144 on entry) and restore it at the end.

## Limits

- **No timers:** timing is cycle-counted with interrupts off. The CPU ran at 32.0 MHz in TX (the
  symbol periods above), and the loop overhead per update is fixed. `tools/pll.py` subtracts it:
  ~32 cycles in mode 0, ~34.5 in mode 1, ~25 per AFSK sample. Mode 2 has a little more.
- **Interrupts are off for the whole command,** so the ROM handles nothing meanwhile. That is fine
  for a CW carrier, but the command must still finish within the host's ~1 s BUSY limit. A longer
  transmission needs several commands (the carrier stays up between them) or a version that
  `yield()`s like the scanner ([pram-scanner.md](pram-scanner.md)).
- **2 KB of table:** about 13 s of AFSK tone bits, or a 2048-sample FM waveform. Longer or
  non-repeating signals would be computed on the chip, as the AFSK entry does.
- The carrier is the console's normal TX path (PA, FEM and power settings), so the PA ramps up at
  SetTxTestMode and down at standby. There's no amplitude control: this is FM only.
- Only sub-GHz (908 MHz) was tested; the 2.4 GHz synthesizer path wasn't.
