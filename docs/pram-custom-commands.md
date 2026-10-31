# Custom commands in PRAM [HW]

The LR2021 runs its own code from the 4 KB Patch RAM. Semtech's patch normally lives there, but
RadioLib (and this repo's firmware) never loads it, so on these boards all of PRAM is free. A
blob with the right header becomes a set of new host commands, reached through ActivatePram
(0x012D). Source: `firmware/pram/` (`sweep.S`: header, entry table, sweep; `fft.c`: FFT). Host
tool: `tools/pram.py`.

## Interface

From the ROM (`pram_check` 0x24FC, ActivatePram handler 0x10AC4):

| Address | Contents |
|---|---|
| 0x101000 | magic 0x600DB002 |
| 0x101004 | low byte = number of entries |
| 0x101008 | entry table: one code address per entry |

- **Activation:** `0x012D 00`. `pram_check` finds the magic, stores `[0x8007F0]` = 0x101008
  (the entry table) and `[0x8007F4]` = 0x101000, and calls entry 0. Entry 0 must return 2 for
  the PRAM to count as active.
- **Calling:** `0x012D k <params>`, with 1 ≤ k < entry count, calls entry k with
  - `r0` = parameter length (without the k byte);
  - `r1` = pointer to the parameters;
  - `r2` = pointer to a response struct `{u32 length; u8 *buffer}`.

  The return value is the command status: 2 = OK, 1 = parameter error, 0 = fail, **3 = data**
  (fill the buffer and set the length, as the ROM's getters do, e.g. 0x0291 at 0x5B44).
- **Loading:** WriteRegMem32 to host address 0x801000 (write-only), then activate. Only after
  activation can the code be read back at CPU 0x101000; reading it earlier stalls the chip
  (`pram_check` sets the bank mapping in aux 0x80000008).
- Handlers can call ROM routines directly, e.g. the SetRfFrequency handler 0xFC00
  (`r0 = 4`, `r1` = big-endian Hz) and the instant-RSSI routine 0x5C4C (returns 9 bits,
  dBm = −value/2).

## Things that hang the chip

- **Registers r16–r25 don't exist.** The CPU has the reduced 16-entry register file
  (RF_BUILD 0x4B02, bit 9): only r0–r3, r10–r15 and r26–r31. The ROM sticks to those.
  `enter_s [r13-r19, ...]` faults. Keep state on the stack.
- **RAM banks owned by a DMA.** In LoRa RX the packet engine's DMA owns the bank at 0x804000
  (its boot-default target); a CPU access from inside a command stalls the chip. The sweep
  stores its results in PRAM after the code (0x101C00–0x101FFF) instead. The FFT runs with the
  chip in standby after the capture is stopped, so it can use that bank.
- **Long commands.** BUSY stays high while a handler runs. RadioLib gives up after 1 s
  (−705), so the host splits long sweeps into chunks.
- **Reading PRAM before activation** (above).

A hang needs a chip reset (`set radio …` or `hold off` in the console re-initialises it).

## Spectrum sweep (entry 1)

Parameters, big-endian: start Hz (4), step Hz (4, signed), points (2, ≤ 512), dwell (2),
readings to average (2). For each point the chip retunes (the radio stays in continuous RX),
waits dwell × 16 loop iterations, sums the instant-RSSI readings and stores the u16 sum at
0x101C00 + 2·i. It returns the number of points as 2 bytes. Retuning in RX works: SetRfFrequency
is accepted mid-RX and RSSI follows the new frequency.

- **Settling:** the RSSI lags a retune. With 62.5 kHz RX bandwidth a dwell below ~1000
  (≈ 1.4 ms) still reads the previous frequency.
- **Speed:** 902–928 MHz in 25 kHz steps (1,041 points), dwell 1000, 4 readings averaged:
  **2.2 s including the readback**; on the chip 100 points take 203 ms (~2 ms per point).
  Floor about −121 dBm.

```
python3 tools/pram.py load                       # after "set radio 908,62.5,7,5" in the console
python3 tools/pram.py sweep 902 928 25 --out band.csv --plot band.png
```

![On-chip sweep and waterfall](figures/pram_sweep_waterfall.png)

The waterfall (30 sweeps, ~70 s) shows the test carrier at 912 MHz from the other board
(switched off half-way), bursts on the US MeshCore channel at 910.525 MHz (other people's
packets, energy only, nothing decoded), one unidentified burst at 912.2 MHz, and a rise at
928 MHz present in every sweep.

## On-chip FFT and live waterfall (entries 7–10, `fft.c`, `stage.S`)

The chip captures raw IQ (source 0, int8 I/Q at 250 kS/s with the 62.5 kHz LoRa setting, 1–10
segments of 256 samples into 0x804000), computes windowed 256-point FFTs, and returns the
spectrum. The console's `wfrun` repeats that and streams binary frames to the PC.

| Entry | Does |
|---|---|
| 7 | FFT of a capture the host already ran; params `nseg skip`; 256 × u64 power at 0x805400 (used by `fft --check`) |
| 8 | capture only, driven from inside the command (debug) |
| 9 | **capture + FFT + dB**; param `nseg`; 256 × u8 half-dB at 0x805400 (bin 0 = DC, 128–255 negative) |
| 10 | timing benchmarks (below) |

### Results

| Segments averaged | Chip time per frame | Streamed frames/s (`wfrun`) |
|---|---|---|
| 1 | 2.82 ms | **306** |
| 2 | 5.47 ms | 169 |
| 4 | 10.3 ms | 93 |
| 10 | 25.0 ms | 39 |

Per frame outside the chip: SPI readback 0.34 ms, USB 0.09 ms. Against numpy on the same capture
(`tools/pram.py fft --check`): same peak bin, correlation 0.9999, 0.14 dB rms difference.

![On-chip FFT vs numpy](figures/onchip_fft.png)

![Live waterfall computed on the chip](figures/onchip_waterfall.png)

(The waterfall was recorded with the first version at 6.6 frames/s: a CW carrier at +30 kHz, then
back-to-back LoRa SF7 / 62.5 kHz packets, the ±50 kHz channel-filter edges and a steady internal
spur at −21 kHz.)

```
python3 tools/pram.py waterfall --seconds 20 --nseg 4 --save wf.npz --plot wf.png
python3 tools/pram.py fft --frames 5 --check --plot spectrum.png
```

### How it got from 6.6 to 306 frames/s

| Step | Effect |
|---|---|
| Capture driven from inside the command (ROM handlers 0x0290 0xF9A0, SetRx 0xFCFC, pointer 0x5B44, SetFs 0xEB40) | one command per frame. **The capture DMA runs while our code holds the CPU** |
| End each capture in FS instead of standby | SetRx from FS takes 0.19 ms, from standby 1.27 ms (synthesizer lock) |
| Capture only the segments used | 1 segment = 1.02 ms of samples instead of 10.2 ms |
| `wfrun`: frames looped on the MCU and streamed as binary | no host round trips or hex text |
| Raw 16 MHz SPI for the command and the readback | readback 1.73 → 0.34 ms |
| 16-bit block-floating-point FFT (no 64-bit maths) | 3.8 → 2.3 ms per FFT |
| Butterfly stages 3–8 in hand-written assembly | GCC spilled registers to the stack (only 16 exist) |
| Load, window and stages 1–2 fused into one radix-4 pass | saves a pass over memory and most of the bit reversal |
| Twiddles built in RAM from a 65-entry quarter-wave table | frees ~380 B of PRAM; RAM reads are also faster than PRAM reads |
| dB straight from 32-bit power for 1 segment | skips the u64 accumulators |

**FFT precision:** block floating point. Windowed input is ≤ 2¹⁴ per component, a radix-2 stage
at most doubles the magnitude, so the plan (from the input's largest raw sample) runs stages
unscaled while the worst-case bound stays ≤ 8192 per component and halves them after that.
Magnitudes stay ≤ 2^14.5, so s16 storage, Q15 twiddle products and their sums fit in 32 bits;
the scale exponent is folded back into the power.

### CPU timing (entry 10)

`rc 012D 0 0A <mode>` runs a benchmark; time it with the console's `us=`. Mode 0 is 10⁶ iterations
of a one-NOP zero-overhead loop: 31.3 ms, so **the CPU runs at 32.1 MHz**. Modes 16–22 loop one
instruction type:

| Loop body | Cycles per iteration |
|---|---|
| `nop_s` | 1.0 |
| 2 × `nop_s` | 3.0 |
| 2 × 32-bit `add` with a 32-bit constant | 4.0 |
| `ldh.x` from 0x805C00 | 2.0 |
| `sth` to 0x805C00 | 1.0 |
| `mpyw` (16×16) | 3.0 |
| `mpy` (32×32) | 3.0 |

A radix-2 butterfly (4 multiplies, 4 loads, 4 stores, ~12 ALU ops) costs 36–40 cycles. A full
256-point FFT is ~41k cycles (1.29 ms): ~2.6k for the max pre-pass, ~5k for the fused
load/window/radix-4 pass, the rest in stages 3–8.

### RAM used

| Range | Use |
|---|---|
| 0x804000–0x8053FF | capture (up to 10 segments) |
| 0x805400–0x805BFF | result: u64 power sums (entry 7) or u8 dB (entry 9) |
| 0x805C00–0x805FFF | FFT work buffer, 256 × {s16 re, s16 im} |
| 0x807800–0x807A03 | twiddle table (built on first use) and its marker word |

0x807800–0x807FFF sits above the stack (top 0x8077F8). Over a receive, a capture and FFTs it kept
its random power-up contents, while only 0x8076F8–0x8077BC changed, so nothing else seems to use
it. The FFT checks the marker and rebuilds the table if anything overwrote it.

## Other entries

| k | Name | What it does |
|---|---|---|
| 2 | hello | returns `LRC1`, the parameter length and the first parameter byte |
| 3–6 | (retired) | debug entries used while building the sweep; now return 1 |
| 7–10 | fft, capture, capfft, bench | see above |

## Build

`firmware/pram/build.sh` builds `pram.bin` with ARC GCC 13 (`docker build -t arc-gcc -f
docker/Dockerfile.gcc docker`) and the linker script `pram.ld`, which packs `.text`, `.rodata` and
`.data` from 0x101000. Flags: `-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding`, with the hot
FFT functions at `O2`. The build fails if the image reaches the sweep buffer at 0x101C00 or if
anything lands in `.bss`. `pram.bin` is checked in, so `tools/pram.py` works without Docker.

Pitfalls met along the way:
- `-mrf16` matches the 16-register CPU. Assembly files need `.arc_attribute Tag_ARC_ABI_rf16, 1`
  too, or the linker refuses to mix them with the C objects.
- `-mno-sdata`: gp (r26) belongs to the ROM.
- No libc or libgcc: stack arrays with initialisers become `memcpy` calls, and variable 64-bit
  shifts become `__ashldi3`. Use `static const` tables and 32-bit halves.
- `.bss` isn't uploaded: give globals an initial value in `.data`.
- `bl` targets must be 4-byte aligned (`.align 4`): an unaligned target is silently rounded down.
- Don't use fp as a scratch register with `enter_s [... fp ...]`: `leave_s` restores sp from it.
- On the nRF52 side, SPI buffers must be in RAM (EasyDMA can't read flash): a `const` zero buffer
  for a response frame sends garbage, and the chip's next command reports CMD_PERR.

The console's `rc` prints `us=` (command time including BUSY), which is how the timings were
measured.
