# Custom commands in PRAM [HW]

The LR2021 runs its own code from the 4 KB Patch RAM. Semtech's patch normally lives there, but
RadioLib (and this repo's firmware) never loads it, so on these boards all of PRAM is free. A
blob with the right header becomes a set of new host commands, reached through ActivatePram
(0x012D). Source: `firmware/pram/sweep.S`. Host tool: `tools/pram.py`.

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
  stores its results in PRAM after the code (0x101400–0x101FFF) instead.
- **Long commands.** BUSY stays high while a handler runs. RadioLib gives up after 1 s
  (−705), so the host splits long sweeps into chunks.
- **Reading PRAM before activation** (above).

A hang needs a chip reset (`set radio …` or `hold off` in the console re-initialises it).

## Spectrum sweep (entry 1)

Parameters, big-endian: start Hz (4), step Hz (4, signed), points (2, ≤ 1536), dwell (2),
readings to average (2). For each point the chip retunes (the radio stays in continuous RX),
waits dwell × 16 loop iterations, sums the instant-RSSI readings and stores the u16 sum at
0x101400 + 2·i. It returns the number of points as 2 bytes. Retuning in RX works: SetRfFrequency
is accepted mid-RX and RSSI follows the new frequency.

- **Settling:** the RSSI lags a retune. With 62.5 kHz RX bandwidth a dwell below ~1000
  (≈ 1.4 ms) still reads the previous frequency.
- **Speed:** 902–928 MHz in 25 kHz steps (1,041 points), dwell 1000, 4 readings averaged:
  **2.2 s including the readback**, about 2.1 ms per point. Floor about −121 dBm.

```
python3 tools/pram.py load                       # after "set radio 908,62.5,7,5" in the console
python3 tools/pram.py sweep 902 928 25 --out band.csv --plot band.png
```

![On-chip sweep and waterfall](figures/pram_sweep_waterfall.png)

The waterfall (30 sweeps, ~70 s) shows the test carrier at 912 MHz from the other board
(switched off half-way), bursts on the US MeshCore channel at 910.525 MHz (other people's
packets, energy only, nothing decoded), one unidentified burst at 912.2 MHz, and a rise at
928 MHz present in every sweep.

## Other entries

| k | Name | What it does |
|---|---|---|
| 2 | hello | returns `LRC1`, the parameter length and the first parameter byte |
| 3 | setfreq | one call to the ROM SetRfFrequency handler; returns its status byte |
| 4 | rssi1 | one instant-RSSI read (2 bytes, raw) |
| 5 | storetest | writes and reads back one halfword in the PRAM result buffer |
| 6 | onepoint | one sweep step without storing (debug) |

## Build

`firmware/pram/build.sh` assembles with the ARC binutils image (`docker build -t arc-binutils
docker`): `-mcpu=em4_dmips`, linked at 0x101000. `sweep.bin` is checked in, so `tools/pram.py`
works without Docker.
