# Raw IQ from the LR2021

> **Credit:** the LR2021's hidden capture engine was originally discovered by
> **[SAMS0N1TE/lr2021-sdr](https://github.com/SAMS0N1TE/lr2021-sdr)** (MIT). That covers
> command 0x0290 and its raw routes, the RAM bank mapping through aux 0x80000008, FIFO IQ
> streaming, and TX through 0x0293. They built it into a full SDR: SoapySDR,
> 100 MHz–2.74 GHz, 500 kS/s 16-bit over a 26.67 MHz SPI on an ESP32-P4.
>
> The FIFO streaming recipe below comes from their documentation (`docs/capture-engine.md`,
> `docs/custom-firmware.md`). The ROM analysis in this repository reached the same code paths
> and agrees with their description: the same bank map and aux mapping, bytes 1–4 of 0x0290
> as the output rate, and the last byte as FIFO enable. What this page adds:
>
> - the ROM-side view;
> - hidden packet type 0x0B, and its interaction with Semtech's PRAM patch;
> - hardware AGC behavior in raw mode;
> - a checked continuous transport on an nRF52840.

Tags: **[HW]** = verified on hardware, **[RE]** = read from code.

## Hidden packet type 0x0B [HW]

The datasheet lists SetPacketType value 0x0B as "RFU". It is a raw/capture mode:

- 0x0290 only fully starts the capture DMA when the packet type (RAM 0x80002C) is 0x0B.
  In LoRa (0) it arms but writes nothing, and the LoRa demodulator stops receiving while it's
  armed.
- **With Semtech's PRAM patch loaded, the FIFO route only produces data in packet type 0x0B.**
  In LoRa the FIFO level stays 0. lr2021-sdr runs without the patch and stays in LoRa.
- **Hardware AGC still works in 0x0B.** Sampling 0xF30154 during a stream showed G13 when
  quiet, G1 during a nearby transmitter, then G13 again.

## One-shot capture into RAM [HW]

While a capture is armed, its RAM bank belongs to the DMA (aux 0x80000008 bank mapping).
**A CPU (or host) read of that bank stalls the bus and hangs the chip.** Stop the capture first
(0x0290 with mode 0), then read the RAM.

Recipe for 8 KB at 0x804000 (cfg 0xD0 = size 6, mode 2, source 0):

```
SetStandby(0)
SetPacketType(0x0B)
0x0290  FF 00000000 D0 00 00 08 00
SetRx(FFFFFF)
poll 0x0291 until 0x2000            (~17 ms, one-shot)
0x0290  FF 00 00 00 00 00           (mode 0: stop, give the bank back)
SetStandby(0)
ReadRegMem32 0x804000 ...
```

Source 0 is **interleaved int8 I, int8 Q at 250 kS/s** (4 × the 62.5 kHz LoRa bandwidth),
so 8 KB = 4096 samples = 16.4 ms. The channel filter edge is visible at about ±50 kHz.
This was verified with LoRa preamble upchirps: a -31..+31 kHz sweep with a 2.05 ms period
(SF7 at 62.5 kHz = 2.048 ms). Source 2 hung the chip once in LoRa packet type; in packet type
0x0B every source captured cleanly.

### All capture sources [HW]

Sources 0–6 have 2, 2, 4, 4, 4, 4, 2 bytes per sample (ROM table 0x13948). Each was captured
one-shot (8 KB, cfg `0xD0 | src`, packet type 0x0B) on the XIAO at 908 MHz, LoRa BW 62.5 kHz,
with no signal and with a CW carrier from the other board at −50, +10, +50 and +100 kHz offsets.
The rate comes from the 0x0291 write pointer and from where the carrier lands.

| Src | Format | Rate | Signal | Notes |
|---|---|---|---|---|
| 0 | int8 I, int8 Q | 250 kS/s | complex baseband after the channel filter (−3 dB ≈ ±35–40 kHz, −20 dB by 60 kHz) | the source used for streaming |
| 1 | int8 I, int8 Q | 250 kS/s | same filter and level as source 0 | a different tap at the same scale; not told apart yet |
| 2 | int16 I, int16 Q (LE) | 250 kS/s | same filter, deeper stopband (≈ −34 dB) | the IQ16 transport |
| 3 | int16 I, int16 Q (LE) | 250 kS/s | **before the final channel filter**: flat to ~60 kHz, −9 dB at 80 kHz, −17 dB at 120 kHz | ~2× source 2's level |
| 4 | 11-bit I, Q, left-aligned in int16 (LE) | **~8 MS/s** (at BW 62.5 kHz) | **wideband complex baseband**: the carrier lands at its offset (+50 kHz → +50 kHz); flat noise to about ±2 MHz | residual line at −332 kHz (IF leakage); passband tilted toward positive offsets |
| 5 | 11-bit I, Q, left-aligned in int16 (LE) | **~8 MS/s** | **low-IF signal before the final mixer**: a carrier at offset *f* appears at ≈ 330 kHz + *f* | strong fixed lines with no signal |
| 6 | int8 I, int8 Q | 250 kS/s | same filter as source 0, **4× the amplitude** (+12 dB) | a tap before a ÷4 scaling, or after extra gain |

At the 62.5 kHz setting, sources 4 and 5 run at about 8 MS/s (probably 32 MHz / 4), so one
8 KB capture is a 256 µs snapshot. That's too fast to stream over SPI, but useful as a
spectrum snapshot around the RX frequency
([figure](figures/source4_snapshot.png): idle, a CW carrier and an SF5 / 1000 kHz LoRa packet):

- The noise floor is flat out to about ±2 MHz. Beyond that, noise rises 30–40 dB, which looks
  like the ADC's noise shaping, not signals.
- The passband isn't symmetric. An SF5 / 1000 kHz LoRa packet comes through with the upper half
  of each chirp (0 to +0.5 MHz) about 20 dB stronger than the lower half. That fits an analog IF
  filter centred above the RX frequency (low IF ≈ +330 kHz).
- A fixed line at −332 kHz (IF leakage) is always there.
- In the LoRa snapshot the signal drops after ~70 µs, probably the AGC reducing gain during the
  capture.

At the 1000 kHz setting, source 4 fills 8 KB much faster and looks different: flat noise and one
strong line at −0.083 × the sample rate, with no LoRa visible. Its rate and IF there weren't
worked out. The ~330 kHz IF is for BW 62.5 kHz.

### Image rejection [HW]

The low IF (~330 kHz at BW 62.5 kHz) puts the image **about 660 kHz below** the RX frequency.
It was measured on the XIAO at 908 MHz with source 2 (int16, after the channel filter) and the
receive gain fixed at step 13 (`rc 021A 0 0D`) so the AGC couldn't skew the comparison:

1. The other board sends a CW carrier at the wanted offset (+10 kHz) and the tone level is
   measured.
2. The same carrier at the image (−670 or −680 kHz, same drive) leaks in as a mirror tone at
   +8.3 / +20.4 kHz. The mirror moves opposite to the carrier, which confirms it's the image
   (and gives IF ≈ 330 kHz). An image at −660 kHz isn't usable: its mirror lands near DC.
3. Image rejection = wanted tone − mirror tone, at the same drive and gain.

| Condition | Image rejection |
|---|---|
| After normal start-up (RadioLib runs CalibFE at 908 MHz) | **33 dB** (32.0–34.6 over 5 runs) |
| CalibFE again, run in LoRa packet type (standby RC / XOSC) | 34.2 / 31.5 dB: no improvement |
| Calibrate with reserved bit 7 (0x80) or bit 4 (0x10) | accepted, no error, no measurable change |
| **CalibFE run while in packet type 0x0B** | **24–25 dB, and the received level drops 18 dB** until the radio is re-initialised |

For comparison, datasheet Rev 2.2 gives sub-GHz image attenuation of 35 dB typical without image
calibration and 45 dB with it (BW < 500 kHz). The measured 33 dB is close to the uncalibrated
figure even though CalibFE had run. In practice, a strong signal ~660 kHz below the channel
leaks in about 33 dB down: a −60 dBm neighbour there is roughly a −93 dBm interferer.

Notes:
- **Don't run CalibFE (0x0123) in packet type 0x0B.** It miscalibrates the front end: −18 dB
  of signal and worse image rejection, until the next full init. Switch to LoRa (`rc 0207 0 00`)
  first.
- Calibrate bits 4 and 7 (RFU) don't affect image rejection, so bit 7 isn't image calibration,
  despite GetErrors bit 7 being called IMG_CALIB_ERR.
- At the IF tap (source 5) the mirror is only a few dB above that source's noise floor, so it
  only shows ≥ ~21 dB before the final mixer. That's not enough to say how much the digital
  correction adds.
- One chip, one frequency, maximum gain, CW tones, and the raw capture path. Whether the LoRa
  demodulator path does better wasn't tested.

Debug console: `cap <cfg> <words> <ms>` and `capwait <cfg> <ratio> <ms>`
([firmware/README.md](../firmware/README.md)).

## Streaming through the RX FIFO [HW]

Route (from lr2021-sdr):

```
SetStandby(0)
SetPacketType(0x0B)                                    (needed with the PRAM patch)
0x0290  FF rate(4) 08|src 00 00 00 00 01               (mode 1 + FIFO enable)
WriteRegMem32 0xF30028 = 0x804400, 0xF30030 = 0x3E8    (RX FIFO at the documented 1 KB region)
ClearFifoIrqFlags(0x3F, 0x3F)
SetRx(FFFFFF)
ClearRxFifo
loop: GetRxFifoLevel (0x011C) + ReadRxFifo (0x0001)
      GetFifoIrqFlags (0x011B) bits 0x30 = data lost
```

Source 0 at 250 kS/s is 500 KB/s across SPI. RadioLib's 2 MHz SPI is too slow, so the
firmware uses its own SPI transactions. The output-rate field had no effect for source 0
with byte 0 = 0xFF. lr2021-sdr sets a filter code in byte 0 together with an explicit rate.

Measured on an nRF52840:

| Setup | Result |
|---|---|
| 8 MHz SPI, triggered snapshots | lossless; FIFO peak 136–152 of 1024 bytes; ~67 µs per level poll |
| 16 MHz SPI, continuous, 3× MCU averaging (83.333 kS/s out) | **60 s: 4,999,818 IQ pairs, 9,766 frames, no FIFO or queue loss**. A 250 ms reader pause was absorbed; a 2 s stall ended with an explicit queue-full error |
| 8 MHz SPI, continuous | completed 5 s, but built 116 KB of queue backlog |
| 32 MHz SPI (beyond the 16 MHz spec) | **failed**, including all eight MISO sampling delays. Impossible status bits and wrong sample counts (`analysis/spi32_delay_sweep.json`) |
| Native IQ16, source 2, LoRa packet type, BW 31.25 kHz (125 kpair/s), 2× averaging | 60 s: 7,499,749 raw pairs, no reported loss |
| Native IQ16, BW 62.5 kHz (1 MB/s on SPI), 4× averaging | FIFO overflow within 5 ms. MCU averaging can't reduce what crosses SPI first |

Source 2 ("route byte 0x0A") is native little-endian signed int16 I, int16 Q, at four complex
pairs per configured LoRa bandwidth Hz. A 16-bit container doesn't establish 16 effective
ADC bits. Design notes and USB/SPI budgets are in `analysis/continuous_streaming.md`.

### Snapshot trigger and AGC

The snapshot streamer (`stream`) triggers when a 4 KB block's power exceeds a multiple of
the quiet baseline. With hardware AGC on, a strong packet only raised output power to about
1.6× baseline, because the AGC was doing its job. The firmware therefore also triggers when the
automatic gain step (0xF30154[23:20], sampled once per block) drops below the highest quiet
step it has seen.

## LRIQ wire format (continuous stream)

`iqstream` sends framed binary over USB CDC. All fields are little-endian, with a 48-byte
header, magic `LRIQ`. Version 1 = IQ8, version 2 = IQ16.

| Offset | Field |
|---:|---|
| 0 | magic (4), version u8, type u8, header size u16 |
| 8 | generation u32, sequence u32 |
| 16 | first output sample index u64 |
| 24 | sample count u32, nominal raw rate u32 |
| 32 | decimation u16, flags u16 |
| 36 | payload bytes u32, payload CRC32 u32, header CRC32 u32 |

- The header CRC32 covers the first 44 bytes. CRC32 is the standard reflected 0xEDB88320
  (same as Python `zlib.crc32`).
- Frame types:
  - 1 = START, with a 64-byte status payload;
  - 2 = DATA, with ≤ 1024 IQ bytes;
  - 3 = END, with a 64-byte status payload.
- Every frame advances the sequence number. DATA advances the sample index without gaps.
- Status payload:
  - four u64 values: raw samples, produced output pairs, sent pairs, elapsed µs;
  - u32 values: max FIFO level, sticky FIFO flags, max service gap µs, max queue bytes,
    queue overflows, BUSY timeouts, SPI Hz;
  - an i32 configuration error.
- Flags:
  - 0x01 FIFO loss;
  - 0x02 queue full;
  - 0x04 clean host stop;
  - 0x08 duration complete;
  - 0x10 BUSY timeout;
  - 0x20 reserved;
  - 0x40 configuration error.

  A missing END is a failed run. Footer totals must agree with the host and with
  floor(raw samples / decimation).
- The host stops the stream with ETX (0x03).

`tools/iq_stream.py` parses and validates the format, and `tools/continuous_capture.py`
records `iq.bin` + `frames.jsonl` + `summary.json`.

Two firmware details mattered:

- One 64-byte USB write per FIFO service was too slow; up to four bounded writes per service
  works.
- Status-read SPI transfers must clock out zero bytes. The nRF SPIM's default over-read byte
  is 0xFF, which the radio later rejected with -706.

## TX from RAM

0x0293 / 0x0294 switch the modulator input (0xF30904 bits 4/5) and packet-engine source
(0xF30800 bits 10/11), with a length and start time [RE]. lr2021-sdr uses this path to
transmit. It was not tested here.
