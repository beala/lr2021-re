# LR2021 reverse engineering

Notes, tools and findings from reverse engineering the Semtech LR2021 LoRa transceiver's
internal firmware (ROM and Patch RAM). This covers how the ROM was read out over SPI and
disassembled, and what was learned from it:

- the full host command map, including 38 undocumented commands;
- the CPU memory map;
- named ROM functions;
- the hook points used by Semtech's PRAM patch;
- the AGC gain register;
- a hidden raw IQ capture mode.

It also includes a software LoRa receiver and MeshCore packet decoder that run on raw IQ
streamed out of the chip.

The work was done on two boards: an nRF52840 (Seeed XIAO) with a Seeed Wio-LR2021 module,
and a RAK3401 carrier with a NiceRF LoRa2021F33-2G4 module. The test firmware in `firmware/` is
a standalone PlatformIO project (Arduino core + RadioLib): a debug console, a bit-rate bench and
a dual-band bench.

## Credit

The LR2021's hidden capture engine was originally discovered by
**[SAMS0N1TE/lr2021-sdr](https://github.com/SAMS0N1TE/lr2021-sdr)**. That includes:

- undocumented command 0x0290;
- the raw IQ routes;
- RAM bank mapping through aux register 0x80000008;
- streaming IQ through the RX FIFO.

They turned it into a full SDR (SoapySDR, 100 MHz–2.74 GHz, TX through 0x0293). This
project's FIFO streaming recipe comes from their documentation
(`docs/capture-engine.md`, `docs/custom-firmware.md`). The ROM analysis here reached the same
code paths and agrees with their description. Please cite and star their work.

The software LoRa demodulator's bit-level steps (Gray mapping, interleaving, Hamming,
dewhitening) follow EPFL's [gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr)
(Tapparel et al.).

## What is not in this repository

The LR2021 ROM is Semtech's copyrighted firmware. This repository **does not contain** any of
the following:

- the ROM image or hex dumps of it;
- the disassembly listings;
- Semtech's datasheet.

It only documents facts about the chip: addresses, register fields, command formats and
argument layouts, plus a description of what each function does. The tools let you read
out and disassemble the ROM of a chip you own, and `.gitignore` keeps those outputs out of
git (`dump/`, `disasm/`, `datasheet/`).

Semtech's PRAM patch is published by Semtech under the Clear BSD license in
[Lora-net/usp](https://github.com/Lora-net/usp)
(`smtc_rac_lib/radio_drivers/lr20xx_driver/inc/lr20xx_pram_lr2021.h`). Its hooks are
described here; get the patch itself from that repository.

Captures of live mesh traffic were also left out, because they contain other people's
packets. The IQ captures in `captures/` are all adverts from the author's own test node,
which has no location set.

## Documentation

| Doc | Contents |
|---|---|
| [docs/dumping.md](docs/dumping.md) | How the ROM was read out and disassembled. Reproduce it on your own chip. |
| [docs/memory-map.md](docs/memory-map.md) | CPU/host address map, ROM layout, RAM variables, CPU build registers |
| [docs/commands.md](docs/commands.md) | Command routing, handler ABI, every undocumented opcode |
| [docs/rom-functions.md](docs/rom-functions.md) | Named ROM functions: address, JLI index, arguments, what they do |
| [docs/pram-patch.md](docs/pram-patch.md) | Semtech's PRAM patch: layout, install, the 21 JLI hooks, PRAM budget |
| [docs/agc-gain.md](docs/agc-gain.md) | AGC gain register, per-packet gain in GetLoraPacketStatus byte 9 |
| [docs/raw-iq.md](docs/raw-iq.md) | Hidden packet type 0x0B, 0x0290 capture, continuous IQ streaming, LRIQ wire format |
| [docs/software-receiver.md](docs/software-receiver.md) | Software LoRa decoder, live sniffer, hardware-vs-software comparison, offline SIC |
| [docs/rate-bench.md](docs/rate-bench.md) | Max LoRa/FLRC throughput, raw IQ TX/RX limits, 2 m APRS from the LR2021, reserved codes (SF0–4, ldro=2, packet type 0x01) |
| [docs/pram-custom-commands.md](docs/pram-custom-commands.md) | Running your own code on the LR2021: the PRAM command interface, pitfalls, an on-chip spectrum sweep and FFT |
| [docs/test-bus.md](docs/test-bus.md) | Undocumented test bus on the DIO pads (0x012F): selector map, packet flags; DIO function codes |
| [docs/dualband-bench.md](docs/dualband-bench.md) | 908 MHz / 2478 MHz dual-band CAD scanning: switch time, catch rates |
| [firmware/README.md](firmware/README.md) | The test firmware: console (`mr`, `mw`, `rc`, `stream`, `iqstream`, LoRa RX/TX), benches, boards |

Data tables:

- `analysis/opcode_table.txt`: every slot in both command tables. It gives the handler
  address, whether the command is documented, its datasheet name and its RadioLib name.
- `analysis/register_fingerprints.txt` / `analysis/undoc_fingerprints.txt`: for each
  documented / undocumented command handler, the parameter length check, the registers and
  RAM it touches, and the functions it calls.
- `analysis/response_lengths.txt`: response lengths per opcode.
- `analysis/jli_table.csv`: all 460 JLI indices mapped to ROM addresses, with known names
  and the PRAM hooks.

## Headline findings

- The ROM (80 KB, ARCv2 EM) is readable from the host with the documented ReadRegMem32
  command at 0x000000–0x013FFF. It can't be read out through any other path.
- **38 opcodes have working handlers but aren't in the datasheet.** They include:
  - auxiliary register read/write (0x0102/0x0103);
  - hardware actionpoints (0x012C);
  - a PRAM sub-command table (0x012D);
  - a raw capture DMA (0x0290–0x0294);
  - test multiplexers (0x012B, and 0x012F, which puts an internal test bus on the DIO pads);
  - lower-level packet-engine and AGC knobs.
- **GetLoraPacketStatus returns 10 bytes, not 6.** Byte 9 is the AGC gain step (G1..G13)
  that the ROM latches during each packet. The current gain step is in 0xF30154[23:20].
- Semtech's PRAM patch fills the 4 KB PRAM almost completely. It works by swapping 21
  entries in a RAM copy of the ROM's 460-entry JLI jump table.
- Hidden packet type 0x0B is a raw mode. Together with 0x0290 it streams int8 IQ at
  250 kS/s through the RX FIFO, and the hardware AGC still works in this mode.
- 0x0290 has seven capture sources: int8 and int16 IQ after the channel filter, int16 before
  it, and two ~8 MS/s 11-bit IQ taps (at BW 62.5 kHz), one at baseband (a ±2 MHz snapshot)
  and one at the ~330 kHz low IF
  ([raw-iq.md](docs/raw-iq.md)).
- **Reserved datasheet values that do something** ([docs/rate-bench.md](docs/rate-bench.md)):
  - LoRa `ldro = 2` is a stronger LDRO: SF−3 bits per chirp, and it receives;
  - SF1–4 send SF12-length chirps carrying 1–4 bits each;
  - SF0 sends a preamble with no payload;
  - LoRa bandwidth codes 0x0/0x1/0x8/0x9 are the narrow SX127x bandwidths (7.8–20.8 kHz);
  - packet type 0x01 is an empty slot that reuses the previous modem's setup.
- **Custom code runs on the chip:** a PRAM blob adds host commands through ActivatePram
  (0x012D). An on-chip sweep covers 902–928 MHz in 1,041 points in 2.2 s, and an on-chip
  capture + FFT gives a live waterfall at 6.6 frames/s ([pram-custom-commands.md](docs/pram-custom-commands.md)).
- Measured maxima: LoRa SF5/1000 kHz delivers 118 kbps, FLRC 2.08 Mbps. The chip's raw IQ TX
  path can transmit 2 m APRS that a Kenwood handheld decodes.

## Quick start

Python 3 with `numpy`, `scipy` and `pyserial` (`matplotlib` for plots).

```bash
python3 -m unittest discover -s tools -p 'test_*.py'      # 52 tests, uses captures/
python3 tools/lora_decode.py captures/iter/c02.bin 83333.333
python3 tools/sic_demo.py --out /tmp/sic_demo              # offline collision cancellation demo
```

Hardware tools need the console firmware (see [firmware/README.md](firmware/README.md)):

```bash
cd firmware && pio run -e xiao_console -t upload --upload-port /dev/ttyACM0 && cd ..
python3 tools/sc.py -p /dev/ttyACM0 "mr F30154 1" "rc 22A 10"      # read gain reg, raw packet status
python3 -u tools/continuous_capture.py --seconds 0 --decode       # live MeshCore sniffer from raw IQ
```

## Layout

```
docs/        write-ups (see table above)
analysis/    opcode table, register fingerprints, JLI map, measurement JSON, SIC demo outputs
tools/       host tools: dump/disasm helpers, serial CLI, IQ capture, decoders, SIC, tests
tools/bench/ bench drivers: serial logger, link/capture scripts, APRS encoder/decoder
firmware/pram/ custom PRAM commands (ARC assembly) loaded by tools/pram.py
firmware/    standalone test firmware (PlatformIO): console, rate_bench, dualband_bench
docker/      ARC binutils image for disassembly
captures/    IQ test vectors (own node only) used by the tests
logs/        gain sweep register dumps, RXSTAT logs
```

## Disclaimer

This is independent research, not affiliated with or endorsed by Semtech. LoRa is a
trademark of Semtech Corporation. Undocumented commands can hang the chip until it is reset,
and some of them overwrite PRAM or live RAM. Use them on your own hardware at your own risk.
Only transmit where you're licensed to, and test into a dummy load.

## License

The code and documentation in this repository are released under the MIT license (see `LICENSE`).
