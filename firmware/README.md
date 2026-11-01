# LR2021 test firmware

Standalone PlatformIO project: the Adafruit nRF52 Arduino core plus RadioLib (pinned to the
commit all results were taken with), and nothing else. Every program talks over USB serial at
115200, one command per line.

| Program | Source | What it's for |
|---|---|---|
| console | `src/console/` | Raw chip access (`mr`, `mw`, `rc`), the ROM dump, IQ capture and streaming, plus a hardware LoRa receiver/transmitter. Used by `tools/`. |
| rate_bench | `src/rate_bench/` | Bit-rate bench, raw IQ TX, APRS, reserved-code experiments ([docs/rate-bench.md](../docs/rate-bench.md)) |
| dualband_bench | `src/dualband_bench/` | 908 MHz / 2478 MHz CAD scan bench ([docs/dualband-bench.md](../docs/dualband-bench.md)) |

Each program's command list is in the comment block at the top of its `main.cpp`.

## Boards

| Env prefix | Board | Notes |
|---|---|---|
| `xiao_` | Seeed XIAO nRF52840 + Seeed Wio-LR2021 carrier | TCXO 1.6 V, IRQ on DIO8, SIMO regulator, OCP 52 |
| `nicerf_` | RAK3401 + RAK19007 base + NiceRF LoRa2021F33-2G4 carrier | TCXO 1.8 V, IRQ on DIO9, 2 W PA. **Needs a battery to transmit**: on USB alone the base board's charger can't supply the PA |

Pins, power-up and the radio setup (PA tables, RF switches) are in `lib/LR2021Board/`. The
Arduino pin maps are in `variants/` and keep their original Arduino/Adafruit LGPL-2.1 headers.
The rest of the project is MIT, like the repository.

## Build and flash

```bash
cd firmware
pio run                                                    # builds xiao_console and nicerf_console
pio run -e xiao_console -t upload --upload-port /dev/ttyACM0
pio run -e nicerf_rate_bench -t upload --upload-port /dev/ttyACM1
```

Envs: `{xiao,nicerf}_{console,rate_bench,dualband_bench}`. Uploads use the boards' UF2/DFU
bootloader over USB (`nrfutil`).

## Console commands

Debug commands (`src/console/LR2021DebugCLI.h`):

| Command | What it does |
|---|---|
| `mr <addr> [words]` | ReadRegMem32: read words at a hex address, printed 8 per line (`AAAAAA: w0 ... w7`) |
| `mw <addr> <value>` | WriteRegMem32: write one word |
| `rc <op> <nread> [bytes..]` | Raw command: hex opcode, number of reply bytes, hex parameter bytes. Prints `rc st=<status> <reply bytes> us=<time incl. BUSY>` |
| `err` | Print and clear the chip error register |
| `romprobe` | Read a few words at candidate ROM bases (how the ROM was located) |
| `hold on\|off` | Stop/start the hardware LoRa receiver. `hold off` re-initialises the radio first if a capture command changed it |
| `cap <cfg> <words> <ms> [trig]` | One-shot 0x0290 capture into RAM 0x804000, then dump. `cfg` = size[7:5]\|mode[4:3]\|src[2:0], e.g. `D0` |
| `capwait <cfg> <ratio> <ms>` | Repeat one-shot captures until the power exceeds ratio × the first capture's power, then dump (set packet type 0x0B first: `rc 207 0 0B`) |
| `stream <src> <x> <ms> <prekb> [mhz] [dec]` | FIFO IQ streaming into a 128 KB ring. Triggers on a power rise × `x` or an AGC gain drop, keeps `prekb` KB of history; `x` = 0 for a timed capture |
| `streamdump` | Print the summary, then `STREAMBIN <n>` and n raw bytes |
| `streamstats` | Per-4 KB-block power, actual / manual gain, int8 rail counts of the last stream |
| `iqstream <ms> [mhz] [dec] [bits] [rawrate] [rxdelay]` | Continuous LRIQ binary stream (see [docs/raw-iq.md](../docs/raw-iq.md)); `ms` = 0 runs until ETX (0x03). Defaults: 16 MHz, decimation 3, 8 bits, 250000 |

Radio commands (replies start with `  -> `, which the host tools parse):

| Command | What it does |
|---|---|
| `get radio` | `  -> > <MHz>,<BW kHz>,<SF>,<CR>` |
| `set radio <MHz>,<BW kHz>,<SF>,<CR>` | Re-initialise the chip with these LoRa settings and start receiving |
| `get tx` / `set tx <dBm>` | TX power: the LR2021 drive level, −9..22. On the NiceRF board this drives the external 2 W PA (see the table in `lib/LR2021Board/LR2021Board.cpp`) |
| `loratx <hex>` | Send one LoRa packet: explicit header, CRC on, 16-symbol preamble, sync word 0x12 |
| `rxstats` | Hardware RX counters: `rx`, `crc_err`, `hdr_err`, `tx` |
| `wfrun <nseg> [frames] [ov]` | Stream the on-chip spectrum (PRAM entry 9, load it with `tools/pram.py load`) as binary frames until `x`: up to 317 frames/s, 464 with `ov` = 1 (nseg 1: next capture overlapped with the FFT). Used by `tools/pram.py waterfall` |
| `aprswait <ms>` | Print the frames the chip's background APRS receiver decodes (`firmware/pram/demod.bin`, started by `tools/aprs_rx.py`): waits for the IRQ line, clears RxDone, reads the frames from PRAM, prints `APRS <ms> <hex>`; `0` runs until `x`. See [docs/pram-demod.md](../docs/pram-demod.md) |
| `pinmon <ms>` | XIAO only: count edges (hardware counters) and the high fraction on the pads wired to the MCU (DIO7, DIO8, DIO11). Prints `PM,<pad>,<edges>,<toggle Hz>,<high fraction>,<level>` |
| `pinsnap <n>` | XIAO only: n ≤ 65536 raw samples of those pads at ~7.1 MS/s (bit0 DIO7, bit1 DIO8, bit2 DIO11), as `SNAPBIN <n> <ns per sample>` + bytes. See [docs/test-bus.md](../docs/test-bus.md) |

While the receiver runs, each packet prints:

```
RXSTAT len=20 st=0 rssi=-92.5 snr=14.75 raw=11 14 3B 5C 5A 07 FF FE 50 0D gain=13 crc=1 data=0001...
```

`raw` is the full 10-byte GetLoraPacketStatus reply (byte 9 is the AGC gain step, see
[docs/agc-gain.md](../docs/agc-gain.md)); `data` is the payload. `tools/hw_rx.py` and
`tools/listen.py` read these lines.

The console boots at 910.525 MHz / 62.5 kHz / SF7 / CR 4/5, −9 dBm, with the **receiver held**,
so it never logs other people's traffic or transmits until told to.

Examples:

```
mr 0 32768                 dump ROM + zeros to 0x1FFFF
mr F30154 1                AGC gain register; bits 23:20 = current step
rc 21A 0 05                SetAgcGainManual(5)
rc 22A 10                  GetLoraPacketStatus, all 10 bytes (byte 9 = gain step)
rc 103 4 00 00 00 04       read aux IDENTITY -> 00 00 00 46
set radio 908,62.5,7,5     test channel, starts receiving
```

Use `tools/sc.py` to script these from the host.

## Notes

- nRF52 USB CDC: output can stall while the host is silent. The host tools send periodic
  blank lines to keep it flowing. The dualband bench only acts on LF line endings.
- `rc`, `mw` and `mr` run alongside the receiver, so settings such as a manual gain
  (`rc 21A 0 05`) stay in effect while receiving. Capture commands (`cap`, `capwait`, `stream`,
  `iqstream`) hold the receiver and switch the chip to packet type 0x0B.
- Tested on the nRF52840 (Seeed XIAO and RAK3401, Adafruit core). `iqstream` uses SPIM3 at
  16 MHz; 32 MHz did not work.
- Undocumented commands can hang the chip until it is reset, and some overwrite PRAM or live RAM.
- Only transmit where you're licensed to, and test into a dummy load.
