# Transmitting from the CPU: the TX FIFO, the SPI pads and the synthesizer [HW]

The LR2021's CPU can't put bytes into the TX FIFO directly (below), so a PRAM blob can't simply
hand the modem a packet. Two ways round it, both in `firmware/pram/rpt.c` (built as `rpt.bin` by
`build_rpt.sh`), and both decode on another LR2021 with CRC good:

1. **Through the chip's own SPI pads:** the CPU clocks WriteTxFifo into its own SPI slave by
   setting the SPI pads' pulls, and the hardware modem sends the packet. Needs the MCU to leave
   its SPI pins high-Z meanwhile.
2. **Through the synthesizer:** the blob encodes the packet and plays the LoRa chirps itself by
   retuning the synthesizer every 2 µs during a CW carrier ([pram-pll.md](pram-pll.md)).

All tests: XIAO transmitting at 908 MHz, SF7, 62.5 kHz, CR 4/5, explicit header, CRC on,
16-symbol preamble, −9 dBm into dummy loads; the NiceRF board receiving with the console.

## Why the CPU can't feed the TX FIFO

| Test | Result |
|---|---|
| Where WriteTxFifo bytes go | Plain RAM at the TX FIFO base, 0xF3002C = 0x6228 → **0x806228** in LoRa mode (size 0xF30034 = 0xFC). The RX FIFO is just before it: 0xF30028 = 0x6140, 0xF30030 = 0xE4 |
| Fill level | Two hardware counters follow SPI writes: 0xF2A0B0 (what GetTxFifoLevel reads, SPI block) and **0xF3003C** (bytes written to RAM, FIFO/DMA block; the TX twin of 0xF30038). Both went 0 → 20 → 37 with two writes |
| CPU (or host `mw`) writes the payload into 0x806228, then SetTx | Level stays 0. TxDone comes, the receiver gets the right length (the header comes from the packet params) but garbage payload, CRC error, SNR −12 dB instead of +4.75. Some runs set the underflow flag |
| Write 0xF2A0B0 or 0xF3003C | No effect: read-only |
| 0xF3001C bit 3 (the FIFO-side half of ClearTxFifo) after a partial TX | Level drops to 0: no rewind |

So the TX FIFO is a ring buffer in RAM with a write pointer only the SPI slave advances and a read
pointer the modem advances; the modem reads only up to the level. WriteTxFifo (0x0002) isn't in
the ROM's command tables: the SPI peripheral recognises it and streams the data in itself, so
there's no ROM routine the CPU could call. Raw TX (0x0293) goes through the same FIFO.

There is a way round it, though: the CPU can clock WriteTxFifo into the chip's own SPI slave
through the SPI pads ([below](#writing-the-tx-fifo-through-the-chips-own-spi-pads-hw)). The
synthesizer modulator described next doesn't need it.

## Writing the TX FIFO through the chip's own SPI pads [HW]

The SPI pins are pads the chip configures itself, so a PRAM command can wiggle them while the
MCU leaves them alone. The SPI slave then receives WriteTxFifo from its own CPU, the level counter
advances, and the hardware modem transmits the packet like any other.

**Pads** (found by driving each pad while the MCU watched its pins, `rpt.bin` entry 4):

| Pad | Signal | Default mux / cfg |
|---|---|---|
| 1 | NSS | 3 / 5 (input) |
| 2 | SCK | 4 / 5 |
| 3 | MOSI | 5 / 5 |
| 4 | BUSY | 6 / 3 (output) |

Pad cfg 0xF02004 + 4i: bit 1 output enable, bit 2 input enable, bits 5:4 pull (**2 = up,
1 = down**). Pad mux 0xF26004 + 4i: 1 = GPIO, set and cleared bitwise with 1 << pad at
**0xF23058 / 0xF2305C** (SetDioFunction codes 6 and 5; the output latch reads back at 0xF23054).

**What works and what doesn't** (20-byte write, XIAO, MCU pins released by the console's
`spifree`, which watched and decoded the lines):

| Method | Result |
|---|---|
| Pads switched to GPIO (mux 1), driven push-pull | Clean waveform on the wire, but the level stays 0: with the mux off its SPI code the slave doesn't see the pad |
| SPI mux kept, output off, level set by the pad pull (up = 1, down = 0) | **Level 20, the bytes at 0x806228.** The slave answers on MISO (status 05 01 00 03 …) |
| Same, half-bit delay 200 down to 6 cycles | All work: the pulls are fast enough |
| Same, with or without first writing 0xF24004 = 0x80000000 (what the main loop does to drop BUSY) | Both work: the slave takes the transaction even while the CPU is in a command |

A stray SCK pulse while NSS was low shifted the stream by a bit (0x0002 became 0x0001), which hid
the result at first.

**Over the air:** 12-, 40- and 100-byte packets written this way, then SetLoraPacketParams and
SetTx from the host: TxDone, and the NiceRF board received all three with CRC good and the right
payload, SNR 5.0–5.25 dB.

**The catch is the MCU's pins.** The MCU must not drive NSS, SCK and MOSI meanwhile (high-Z, no
pulls: the chip's pulls set the level). A pull can't harm a driven line, so if the MCU does drive
them the write just fails. A standalone repeater would leave those pins released and take SPI back
when it needs the radio.

Entry 3 (`selfspi_cmd`): wait (4, cycles), half-bit delay (2, cycles), n (1), flags (1: bit 0 don't
clock, bit 1 write 0xF24004 = 0x80000000 first, bit 2 write 0x02000000 after), method (1: 0 GPIO,
1 pulls), pull code for high (1), for low (1). Data at 0x807800. Log at 0x807F00.

Console `spifree [np] [w<ms>] <hex frame>`: sends the frame, releases NSS/SCK/MOSI (np: no MCU
pulls) until BUSY drops and at least `ms` have passed, samples the lines and decodes MOSI and MISO
on SCK rising edges, then takes SPI back.

```
spifree np w40 01 2D 03 00 01 77 00 00 10 14 00 01 02 01     20 bytes, half-bit 16 cycles, pulls
```

## The synthesizer modulator

**Units line up.** One synthesizer LSB is 10⁶ / 2²⁰ Hz, so 62.5 kHz is exactly 65536 LSB. With
8 updates per chip an SF7 chirp has 1024 updates of exactly 64 LSB, and at 32 MHz an update every
64 cycles makes a symbol exactly 2^SF / BW. In general, for bandwidth code b (3 = 62.5 kHz … 6 =
500 kHz): bandwidth 2^(13+b) LSB, 2^(6−b) updates per chip, step 2^(2b+7−SF) LSB, always 64
cycles per update.

**Encoding** (`lora_encode` in `rpt.c`, reference `tools/lora_encode.py`): whitening, CRC-16 as
gr-lora_sdr computes it, header nibbles and checksum, Hamming, diagonal interleave, inverse Gray,
+1; the header block at SF−2 bits and CR 4/8. Symbols for 12-, 80-, 100- and 140-byte packets were
compared with the LR2021's own modem through an RTL-SDR and `tools/lora_decode.py`: identical
(apart from 5 symbols at the end, the padding of the last block). The C encoder's output read back
from RAM matches the Python one for 12–255 bytes. Encoding 255 bytes takes 4.3 ms on the chip.

**Chirp list:** preamble upchirps (value 0), the sync word 0x12 as values 8 and 16, two downchirps
and a quarter, then the data symbols, as u16 entries at 0x807900.

**Playing it:** SetTxTestMode CW (handler 0x103AC) called from the blob starts the carrier at
once; the blob then disables interrupts and writes 0xF40144 = fmin + ((idx mod M) << shift),
waiting d cycles after each update except the last of each symbol, which waits dlast so the
per-symbol overhead is absorbed. Then SetStandby.

### Timing

The receiver has to see symbols that are 65536 CPU cycles long to about a cycle: its timing-drift
correction follows the carrier frequency offset, which assumes the symbol clock and the carrier
come from the same crystal. They do here (the CPU runs from the 32 MHz TCXO in TX), as long as the
cycle count is exact.

- The loop costs 20 cycles + d per update. d = 44 → 64 cycles.
- Per-symbol time from the host (difference between 8- and 40-symbol preambles): 2047.68 µs at
  d = 44, dlast = 10; 2015.75 at d = 43.
- The SDR shows the drift directly as a growing offset of the decoded symbol values: at
  dlast = 20, +1 chip by symbol 30 and +8 by symbol 367 (symbols ~10 cycles short).
- **d = 44, dlast = 30** decodes 60–255-byte packets on the LR2021 (single runs at 60, 100, 140,
  200, 224, 228, 232, 236, 255 bytes, all CRC good, SNR 3.75–4.75 dB; the hardware modem gave
  4.75 dB on the same link). 40-byte packets: 15/15 at dlast = 20.

A finer sweep of the window and repeated trials at full length are still to do.

## Entry 1 (`rpt.bin`)

Payload at 0x807800 (write it with WriteRegMem32). Params (big-endian): len (1), SF (1), bw code
(1), CR 1–4 (1), LDRO (1), preamble (1), d (2), dlast (2), flags (1: bit 0 time the loop without a
carrier, bit 1 only build the chirp list, bit 2 the carrier is already on), data symbol offset
(1, optional, for tests). Returns the number of chirps (2) and the carrier's synthesizer word (4).

```
rc 012D 6 01 0C 07 03 01 00 10 00 2C 00 1E 00     12 bytes, SF7/62.5 kHz, CR 4/5, preamble 16, d 44, dlast 30
```

## Limits and pitfalls

- Interrupts are off for the whole packet (0.8 s for 255 bytes at SF7/62.5 kHz): the ROM
  processes nothing meanwhile, and a command doing it must stay under RadioLib's 1 s BUSY limit.
- FM only: the PA ramps at SetTxTestMode and standby, as for any CW.
- Only SF7/62.5 kHz tested; LDRO and other bandwidths are coded but untested. The timing constants
  are for 62.5 kHz on the XIAO.
- `lp` with lp_count = 0 runs 2³² times: the first build hung the chip with dlast = 0.
- Test harness: the console drops RXSTAT lines printed after ~2 s without USB traffic, and stalls
  while the host is silent. Long packets looked lost until the host kept the port busy.
