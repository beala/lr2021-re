# A Meshtastic node on the LR2021's own CPU [HW]

The chip runs a Meshtastic node by itself. It relays the mesh's packets with Meshtastic's
managed-flooding rules for the CLIENT role. Every few hours it also sends a NodeInfo, so phones list
it by name. It is built the same way as the [MeshCore repeater](pram-repeater.md): a PRAM blob
hooks the ROM's radio state handler, and it writes its TX FIFO through the chip's own SPI pads.
The MCU only loads it, then lets go of the SPI pins (or sleeps).

- Blob: `firmware/pram/meshtastic/mt.c` + `../selfspi.c` → `mt.bin` (3.3 KB of the 4 KB PRAM),
  built by `build_mt.sh`.
- Host tool: `tools/meshtastic_node.py`, with `run`, `send` (test packets) and `listen`
  (decrypts and decodes).

**License:** the routing rules, packet layout, timing, channel hash and key handling are ported from
the [Meshtastic firmware](https://github.com/meshtastic/firmware), which is GPL-3.0. So
`firmware/pram/meshtastic/` and `tools/meshtastic_node.py` are **GPL-3.0-only** (see
`firmware/pram/meshtastic/COPYING`). The rest of the repository stays MIT. `mt.bin` links the
MIT-licensed `selfspi.c`, and the combined blob is GPL-3.0.

## What it does

The chip has no AES, so it can't read the payload. It works from the 16-byte outer header that
every Meshtastic packet carries in the clear (`RadioInterface.h` `PacketHeader`, little endian):

| Bytes | Field |
|---|---|
| 0–3 | to (0xFFFFFFFF = broadcast) |
| 4–7 | from |
| 8–11 | packet id |
| 12 | flags: hop_limit (bits 2:0), want_ack (3), via_mqtt (4), hop_start (7:5) |
| 13 | channel hash (xor of channel name and key; LongFast with the default key: 0x08) |
| 14 | next_hop (last byte of the next relay's node number, 0 = any) |
| 15 | relay_node (last byte of the node that sent this copy) |

Meshtastic itself relays packets it can't decode the same way (`NextHopRouter::relayOpaquePacket`).
The chip applies the rules a CLIENT uses for packets on its own channel (`NextHopRouter`,
`FloodingRouter`, `PacketHistory`):

| Packet | Action |
|---|---|
| New (from, id); not from or to us; id ≠ 0; hop_limit > 0; next_hop 0 or our last byte | queue a copy with hop_limit − 1 and relay_node = our last byte; send after (2·CWmax + rand(2^CW)) slots |
| Seen, with more hops left than any copy so far ("upgrade") | requeue our copy with the higher hop_limit and a new delay, or relay it again if ours already went |
| Seen, and from the originator again (hop_start = hop_limit) | relay again, unless our copy is still queued |
| Seen, otherwise (another node relayed it) | **drop our queued copy**. This is what keeps a dense mesh from echoing everything. Not when the first copy named us as next hop, nor in ROUTER mode |
| next_hop is someone else's byte, to us, to NODENUM_BROADCAST_NO_LORA (1), hop_limit 0 | not relayed (still recorded as seen) |
| Shorter than 16 bytes, from us, id 0 | ignored |
| Due NodeInfo | send the next stored copy |

**Timing** follows `RadioInterface::getTxDelayMsecWeighted`:
- A slot is 4.5 symbols (a 4-symbol CAD + 0.5) + 7.6 ms. On LongFast (SF11, 250 kHz) that's 44.4 ms.
- CW = map(SNR, −20…10 dB → 3…8), so better links wait longer and the far nodes relay first.
- A CLIENT waits 16 slots (0.71 s) plus rand(2^CW) slots, 0.71 to 12 s in all. With the ROUTER flag,
  the wait is rand(2·CW) slots and queued relays are never dropped.

**Dedup:** 64 entries of FNV-1a(from, id). The low bits hold the highest hop_limit seen and whether
the first copy named us as next hop. Meshtastic keeps more, with ages; here the oldest entry is
overwritten.

## NodeInfo

A phone lists a node once it gets the node's NodeInfo: a `Data` message (portnum 4) holding a
`User`, AES-128-CTR encrypted with the channel key. The nonce is the packet id (8 bytes LE), the
sender (4 bytes LE), then zeros. The default LongFast key is `1PG7OiApB1nwvP+rz05pAQ==`
(`Channels.h` `defaultpsk`).

The chip can't encrypt, and every packet needs a fresh id or receivers drop it as a duplicate. So the
host encrypts 4 copies with different ids and stores them at 0x807D00, each a length byte then the
packet. The chip sends the next copy every interval (Meshtastic's default is 3 h), so ids repeat only
every 12 h.

The `User` it sends:
- `id` "!xxxxxxxx", long and short name;
- `hw_model` PRIVATE_HW;
- `is_unmessagable` true, since the chip can't read DMs.

It has no public key and isn't signed. Only Meshtastic's opt-in Strict signature mode drops unsigned
packets. The official Meshtastic protobufs (Python `meshtastic` package) parse the encoding exactly.

## How it runs

As in [pram-repeater.md](pram-repeater.md#how-it-runs):
- receive with a timeout up to the next due packet;
- StopTimeoutOnPreamble, so a packet that starts during the wait finishes;
- write the TX FIFO through the chip's own SPI pads;
- SetTx, then back to RX.

The clock is the radio timer's 32.768 kHz counter, and the queue is the same 1 KB arena.

The host sets the modulation and Meshtastic's sync word 0x2B (console `set sync 2B`; RadioLib
`setSyncWord(0x2B)`; register 0xF30A68 then reads `04 16`). It also sets the 16-symbol preamble, as
the blob does for every RX and TX. The chip keeps all of this.

RAM, above the stack:

| Address | Use |
|---|---|
| 0x807800–0x807903 | RX FIFO (moved there so 255-byte packets don't wrap) |
| 0x807908–0x807CFF | TX queue (1016 bytes: ~10 typical packets, 3 of 255 bytes) |
| 0x807D00–0x807DFF | NodeInfo copies from the host |
| 0x807E00 | state |
| 0x807F00–0x807FFF | 64 seen entries |

## Entries (`mt.bin`)

| Entry | Params | What it does |
|---|---|---|
| 1 | op (1), then for op 1: node number (4, BE), RX preamble (1), TX preamble (1), SPI half-bit delay (1, cycles), SF (1), bw code (1, 3–6 = 62.5–500 kHz), mode (1, bit 0 = ROUTER timing) | op 1 start, op 2 status, op 3 inject: handle 0x807800[0..len) as if received (len (1), SNR × 4 (1)), op 4 NodeInfo: count (1), interval (2, minutes), first after (2, seconds), op 0 stop. Reply: 20 LE words (state, received, CRC errors, header errors, relayed, duplicates, relays dropped, upgrades, ignored, busy, TX done, SPI write failures, events, last length, queue bytes, ticks to next TX, IRQ, uptime s, NodeInfos sent, last SNR × 4) |
| 2 | – | returns `MTS1` |

```
python3 tools/meshtastic_node.py run --name "LR2021 PRAM" --short PRAM --seconds 3600    # US LongFast
python3 tools/meshtastic_node.py --preset MediumFast run --sleep --seconds 600          # MCU asleep
python3 tools/meshtastic_node.py --port <other board> send "hello"                      # test packet
python3 tools/meshtastic_node.py --port <other board> listen --seconds 60
```

The frequency defaults to the preset's US slot: hash(preset name) mod 104, so LongFast lands on
906.875 MHz. `--mhz` overrides it.

## Results

On the bench, the XIAO ran the node at −9 dBm on US LongFast and the NiceRF played the rest of the
mesh, sending with `loratx` and decoding with the `listen` decoder:

| Test | Result |
|---|---|
| NodeInfo 3 s after start | heard; decrypts to `!c0ffee42` "LR2021 PRAM" / "PRAM" |
| New broadcast, hop 3/3 | relayed after 4 s as hop 2/3, relay byte 0x42, payload unchanged (decrypts to the same text) |
| Same packet heard as hop 2 from relay 0x77 while ours waits | our copy dropped, nothing sent |
| next_hop 0x55 / 0x42 | not relayed / relayed |
| To us; hop_limit 0 | not relayed |
| Hop 1/3 copy, then a hop 3/3 copy | one relay, with hop 2: the queued copy was upgraded |
| The originator sends it again after our relay | relayed again |
| Chip counters, 10 packets | 10 received, 5 relays queued, 3 duplicates, 1 relay dropped, 1 upgrade, 3 ignored, 5 TX done (NodeInfo + 4 relays), 0 SPI write failures |

`tools/meshtastic_bench.py` runs these cases and checks them: **7/7 passed**.

The node also ran 10 minutes with the MCU asleep (`run --sleep`). Its NodeInfo went out on time, with
0 SPI write failures. A Meshtastic node on the same LongFast channel would list it by that NodeInfo.
No other Meshtastic traffic was heard on US LongFast at the bench, so it hasn't relayed a real mesh
yet.

## Not done

- **No decryption.** It can't read or answer DMs, send ACKs, or report position or telemetry. It
  treats every packet as on its own channel. A real CLIENT drops its relay on a duplicate only for
  packets it can decode, and relays the rest without that check.
- No NextHopRouter fallback to flooding, no traffic management, no rebroadcast modes other than ALL.
- The dedup table has no ages: 64 entries, then the oldest goes.
- Its node number is whatever the host gives it. The default is random, kept in `~/.lr2021_meshtastic_node`.

## Host USB autosuspend

While testing this, `spifree` replies started to vanish whenever the MCU stayed quiet for more than
about 2.3 s. The cause was the laptop: TLP had USB autosuspend on (`power/control` = `auto`, 2 s
delay). The host suspends the boards' CDC links when idle, and the nRF52 core doesn't wake them to
send. The tools now send an empty line every 0.5 s while they wait (`pram.Console.read_for`); the
console ignores empty lines, and `spifree` drops what it gets. Alternatively, run
`echo on | sudo tee /sys/bus/usb/devices/<port>/power/control`.
