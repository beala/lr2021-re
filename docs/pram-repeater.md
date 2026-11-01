# A MeshCore repeater on the LR2021's own CPU [HW]

A PRAM blob (`firmware/pram/mesh.bin`, source `mesh.c` + `selfspi.c`, built by `build_mesh.sh`)
turns the LR2021 into a simple MeshCore repeater. The chip receives a packet, parses MeshCore's
header, drops duplicates, edits the path, waits MeshCore's random retransmit delay and transmits
the packet again. Everything runs on the chip. The MCU's only job is to load and start the blob
and then leave its SPI pins floating, because the chip writes its own TX FIFO by driving those
pins ([pram-lora-tx.md](pram-lora-tx.md#writing-the-tx-fifo-through-the-chips-own-spi-pads-hw)).

```
python3 tools/mesh_rpt.py run --seconds 60                         # bench: 908 MHz, 16-symbol preamble
python3 tools/mesh_rpt.py run --mhz 910.525 --preamble 32 --seconds 600
```

The bench below: the XIAO repeating, the NiceRF board sending MeshCore-shaped packets with its
console and listening for the repeats. 908 MHz, SF7, 62.5 kHz, CR 4/5, 16-symbol preamble, −9 dBm
into dummy loads. Then [with a real MeshCore companion](#with-a-real-meshcore-companion).

## What it does

| Packet | Action |
|---|---|
| Flood (route 1), payload type 0–8, not seen, under the hop limit | append our hash (1–3 bytes, from the path_len hash size), hop count + 1, wait rand(0 … 5 t) with t = airtime × flood factor (MeshCore's `tx_delay_factor`, default 0.5), send |
| Direct (route 2), first path hash is ours, not seen | remove our hash, hop count − 1, wait rand(0 … 5 t) with the direct factor (default 0.3), send |
| Seen before | dropped. The hash covers the payload type and payload (FNV-1a here; MeshCore uses SHA-256 over the same fields), so the same packet with a longer path is a duplicate too |
| TRACE (direct, payload type 9), the next hash in its list is ours (`payload[9 + (path_len << size)]`), not seen | append our SNR × 4 (int8) to the path, wait as for direct, send |
| Due advert (see [Adverts](#adverts)) | send the stored advert |
| Transport-code routes (0, 3), other payload types, direct for someone else, bad path_len | ignored |

64 hashes are remembered (MeshCore keeps 160; at 32 bits a false match is about 1 in 70 million
per packet). Packets to send wait in a queue (below).

### TX queue

Packets to send sit back to back in a 1128-byte arena (0x807908–0x807D6F), each as {due time,
length, packet padded to 4 bytes}: about 8 typical MeshCore packets, or 4 of maximum size. Due
times are on the chip's clock (32.768 kHz ticks). After every event the chip sends the earliest
packet if it's due, or receives until it is (at most 60 s); a packet that arrives meanwhile is
received, and the wait is recomputed from the clock. A packet that doesn't fit is dropped (busy).
The received packet is parsed in place in the RX FIFO (0x807800, 260 bytes), so no copy is needed.

## How it runs

The same pattern as the background scanner ([pram-scanner.md](pram-scanner.md#background-mode-hw)):
the blob puts its own function in the ROM's radio state handler slot (gp+0x34), passes every event
to the handler the ROM installed, then acts on the IRQ status.

1. **Receive:** SetRx with a timeout (60 s idle, less when a queued packet is due sooner). The ROM's
   RX FIFO is a 232-byte ring at 0x806140, so a longer packet wraps over its own start; the blob
   moves the FIFO to 0x807800 with 260 bytes (0xF30028 / 0xF30030) at every RX start.
2. **RxDone, CRC good:** length from GetRxPktLength's handler (0x5CA8), SNR from
   GetLoraPacketStatus's (0x57E8, byte 2), MeshCore logic on the packet in the FIFO, the result
   queued with its due time.
3. **Wait:** StopTimeoutOnPreamble is on, so a packet that starts during the wait is received
   rather than cut off; the wait is then recomputed from the clock. Airtime comes from the LoRa
   formula (explicit header, CRC on, no LDRO).
4. **Timeout:** SetFs, ClearTxFifo, `fifo_write` clocks WriteTxFifo into the chip's own SPI slave
   through pads 1–3 using the pad pulls, then SetLoraPacketParams and SetTx. If the TX FIFO level
   isn't the packet length afterwards (the MCU is driving the pins), the packet is dropped and
   counted as an SPI write failure.
5. **TxDone:** the next queued packet, or RX.

## Clock and adverts

**The chip keeps its own time.** The radio's timer block (0xF11000, the one the ROM's RX timeout
uses) has a free-running 32.768 kHz counter at **0xF11014** (it advanced 32.6k per second of host
time). Every event adds the ticks since the last one, masked to 24 bits in case the counter is
that narrow. Idle RX is now a 60 s RX with a timeout instead of a continuous one, so an event comes
at least once a minute; restarting costs a gap of about a millisecond. The status reports the
uptime.

### Adverts

MeshCore adverts are signed with Ed25519, which the chip can't do. `mesh_rpt.py run --name NAME`:

1. keeps an Ed25519 key in `~/.lr2021_repeater.key` (made on first use; `--vanity 00` searches for
   a public key starting with 00);
2. uses the public key's first bytes as the repeater ID, so its path hash is what companions expect;
3. builds a repeater advert (flood, public key, current time, signature, app data `0x82` + name,
   signed over key + time + app data) and writes it to 0x807D70 (≤ 144 bytes);
4. op 4 tells the chip its length, the interval (default 48 h) and when to send the first.

The chip ends its RX wait when the advert is due, sends it (heard 3.3 s after a start that asked for
3 s, signature checked on the receiving board) and marks it seen so echoes aren't forwarded again. The timestamp is fixed, so nodes that already have this advert
ignore later copies; a fresh one needs the host to sign again.

**On air:** run on the NiceRF board, the advert went out and came back 114 bytes long, one hash
added: another MeshCore repeater nearby had re-flooded it, which it only does after checking the
signature. The companion then listed the repeater by name. A trace from the companion was
forwarded by the chip (counted and sent) but the reply never reached the companion: probably the
NiceRF's direct signal doesn't reach it, and unlike the advert nobody relays a trace's last hop.

## Results

| Test | Result |
|---|---|
| Flood, 0 hops, 30 bytes | repeated as `11 01 C7 …`: hop count + 1, our hash appended. 3/3 |
| Direct, path `C7 5E` | repeated as `0A 01 5E …`: our hash removed. 3/3 |
| Same flood again, and again with a longer path | not repeated (duplicate) |
| Direct for another node, TRACE | not repeated |
| Floods of 100, 140, 200, 231, 240, 254 bytes (255 after our hash) | all repeated with CRC good, the chip's copy of the received packet exact. Before the RX FIFO was moved, 240 and 254 bytes came out wrapped |
| `tools/mesh_rpt.py`, 20 random packets (floods with 0–3 hops, directs, duplicates with a longer path, directs for others; 22–144 bytes) | **11/11 repeated byte for byte, 9/9 correctly silent.** The chip's counters: 20 received, 7 floods and 4 directs forwarded, 4 duplicates, 5 ignored, 11 TX done, 0 SPI write failures, 0 CRC errors |
| A wait longer than the MCU kept its pins released (test artifact) | packet dropped and counted as an SPI write failure, nothing sent |

| TRACE through `C7` (bench, XIAO repeating) | came back as `26 01 2E …`: path length 1, our SNR 0x2E = 11.5 dB appended, the rest unchanged |
| Three floods sent back to back (queue) | 3/3 repeats heard and correct; chip: 3 floods forwarded, 0 busy, 4 TX done |
| Four injected packets (40–250 bytes, op 3) | queued byte for byte with their due times, then sent by the chip: 4 TX done, 0 SPI write failures |

Delays follow MeshCore's rule: with the default factor 0.5 a 30-byte flood (160 ms on the air) waits 0–400 ms.

## With a real MeshCore companion

The XIAO repeating at 22 dBm with an antenna, as `00` (`--id 00000000`), MeshCore's 32-symbol
preamble, 908 MHz, SF7, 62.5 kHz, CR 4/5, and a stock MeshCore companion sending on the same
settings:

```
python3 tools/mesh_rpt.py run --mhz 908 --cr 5 --id 00000000 --preamble 32 --dbm 22 --seconds 1800
```

The companion sent two flood messages and **heard both repeated** by the LR2021. The chip's
counters for the run: 2 received, 2 floods forwarded, 2 TX done, no CRC or header errors, no
duplicates, nothing ignored, no SPI write failures.

## With the MCU asleep or off

While the repeater runs, the chip pulls NSS up and SCK and MOSI down itself (pad cfg bits 5:4),
switching to data levels only while it writes the FIFO, so floating MCU pins can't start a stray
SPI transaction. Op 0 puts the pads back as they were.

The same 20-packet bench (floods with 0–3 hops, directs for us and for others, duplicates with a
longer path), with the MCU doing nothing:

| MCU | Result |
|---|---|
| Sleeping: the console idles in `delay()` (`mesh_rpt.py run --sleep`) | 11/11 repeated byte for byte, 9/9 correctly silent; chip counters as before, 0 SPI write failures |
| **Off:** System OFF, USB gone, only a reset wakes it (`mesh_rpt.py run --sysoff`, console `sysoff`) | **11/11 repeated, 9/9 correctly silent** |

Careful with `sysoff`: nothing drives D7 yet, so the board stays off until someone presses reset or
replugs it. For unattended runs use `--sleep`, which the host can end over USB.

So the radio needs nothing from the MCU but power and NRESET held high. On the XIAO, D7 (wired to the
LR2021's DIO7) is the System OFF wake pin, so a future version of the blob could wake the MCU
itself by driving DIO7 high.

## Entries (`mesh.bin`)

| Entry | Params | What it does |
|---|---|---|
| 1 | op (1), then for op 1: ID (4, the first 1–3 bytes are the path hash), RX preamble (1), TX preamble (1), half-bit delay (1, cycles), SF (1), bw code (1, 3–6 = 62.5–500 kHz), CR (1, 1–4), flood factor (1, tenths), direct factor (1, tenths), hop limit (1) | op 1 start, op 2 status, op 3 inject: handle 0x807800[0..len) as if received (len (1)) and queue the result, op 4 advert: length (1, ≤ 144, 0 = none), interval (2, minutes), first one after (2, seconds), packet at 0x807D70, op 0 stop (send SetStandby next) |
| 2 | – | returns `MSH1` |

All ops return 20 LE words: state | queued packets << 8 | hooked << 16, received, CRC errors, header
errors, floods forwarded, directs forwarded, duplicates, ignored, busy, TX done, SPI write
failures, events, last length, queue bytes used, ticks until the next is due, IRQ status, uptime (s),
adverts sent, TRACEs forwarded, last SNR × 4.

RAM: 0x807800 RX FIFO, 0x807908 TX queue, 0x807D70 the stored advert (≤ 144 bytes), 0x807E00
state, 0x807F00 the hashes. `mesh.bin` is 3.4 KB of the 4 KB PRAM.

**SPI pad timing differs by board.** The pulls make slow edges: a half-bit of 8 CPU cycles works on
the XIAO, but on the NiceRF board every FIFO write failed (2 SPI write failures, nothing sent) until
it was raised; 16 and 400 work there. `mesh_rpt.py` now uses 32 (a 255-byte write takes ~7 ms).

## Limits

- **The MCU must keep its NSS, SCK and MOSI pins floating** while the chip repeats: asleep or off
  is fine (above). To use the radio itself it takes the pins back (the console's `spifree` does so when its time is up), and
  any packet the chip tries to send meanwhile is dropped, harmlessly.
- Simple: no transport codes or regions, no ACK special cases, no listen-before-talk
  beyond the RX wait, no flood hop limits per type, no airtime budget, no priorities in the queue.
- Without `--name`, the ID is whatever the host passes; with it, the advert key's first bytes.
- The NiceRF board ran at drive 6 (~20.5 dBm out of its PA): on USB power alone its PA browns the radio
  out at drive ~7 and up.
- Tested at SF7 / 62.5 kHz only: the bench with a 16-symbol preamble, the companion with
  MeshCore's 32 (`--preamble 32`; MeshCore uses 16 from SF9). The receiver's preamble must match the transmitter's
  ([commands.md](commands.md#behavior-of-documented-commands-measured-on-hardware)).
- `set radio` resets the chip and clears PRAM; reload the blob after it.
