# Multi-channel LoRa receiver on the chip [HW]

A PRAM blob (`firmware/pram/scan.c`, built as `scan.bin`) makes the LR2021 hop over a table of up
to 32 channels by itself. On each channel it runs CAD (channel activity detection), and on the
first channel with a real preamble it stays in RX, so the packet arrives like any other. One radio
can listen to many channels, or to several spreading factors, with no help from the MCU between
packets.

```
python3 tools/scan.py listen --ch 902.5:906:0.5 --seconds 60       # 8 channels, SF7/62.5 kHz
python3 tools/scan.py listen --ch 903:7,904:8,905:9,906:10         # one SF per channel
python3 tools/scan.py bench --channels 20 --trials 30              # XIAO transmits on random channels
```

`listen` prints what it receives. On a real channel that is other people's traffic, so its output
isn't something to publish. The bench numbers below come from the two bench boards only: 902.5–910.25
MHz, −9 dBm drive, dummy loads.

## Results

The XIAO sends MeshCore-style packets (SF7, 62.5 kHz, 32-symbol preamble) on a random channel of
the table, at a random moment during the scan. A trial counts only if the scanner stops on the
right channel and the packet arrives intact (CRC good, payload matches).

| Channels | CAD | One pass over the table | Decoded |
|---|---|---|---|
| 4 | 2 symbols | 17 ms | 30/30 |
| 8, 12 | 2 symbols | 33, 50 ms | 20/20, 20/20 |
| 16 | 2 symbols | 67 ms | 16/20 (pass longer than the 65 ms preamble) |
| 8, 12, 16 | 1 symbol | 17, 25, 34 ms | 20/20 each |
| **20** | 1 symbol | 42 ms | **89/90** over three runs |
| 24 | 1 symbol | 50 ms | 28/30, 29/30 |
| 28 | 1 symbol | 59 ms | 26/30 |
| 32 | 1 symbol | 67 ms | 22/30 |

So with MeshCore's preamble at SF7/62.5 kHz, one chip reliably covers about **20 channels**. The
limit is the preamble: a detection has to come early enough to leave the receiver a few symbols
to lock on, and past ~24 channels a pass takes most of the 65 ms.

- **A hop costs about 70 µs** on top of the CAD itself: 160 two-symbol CADs took 4.17 ms each
  against 4.10 ms of air time. Retuning, the modulation and CAD parameters, and restarting CAD all
  run on the chip.
- **Mixed table:** SF7, SF8, SF9 and SF10 channels (each with MeshCore's preamble: 32 symbols
  up to SF8, 16 from SF9) decoded 23/24, every SF10 and SF8 packet included.
- **Listen mode:** 8 channels for 30 s while the XIAO sent 8 packets on 7 of them: all 8 received
  on the right channel.
- **False alarms are handled on the chip.** One-symbol CAD fires on noise about 1% of the time
  (138 in 30 s of listening on 8 channels). A false hit never shows a preamble, so the chip waits
  8 symbols for one and otherwise goes back to scanning; each costs ~16 ms at SF7.

## How it works

The host writes a channel table into free RAM at 0x807800 (`{u32 Hz; mod0, mod1, CAD peak,
preamble}` per channel) and calls entry 1 (params: channels, CAD symbols, exit mode, symbols to
wait for a preamble, rounds). For each channel the blob calls the ROM's own handlers:
SetRfFrequency (0xFC00), SetLoraModulationParams (0xF238) and SetLoraPacketParams (0xF274) when
they change, SetLoraCadParams (0xF188), then SetLoraCad (0xF178). It polls the IRQ status register
0xF2A00C (write 1 to clear) for CadDone:

- no detection: the chip is back in the fallback mode, FS (`0x0206 03`), so the synthesizer is
  still running and the next retune is quick;
- CadDetected with exit mode RX (`0x01`; RadioLib's 0x02 is wrong, see
  [commands.md](commands.md#behavior-of-documented-commands-measured-on-hardware)): the chip is
  receiving on that channel. The blob waits for PreambleDetected, with the RX timeout set to 8
  symbols of that channel's SF and StopTimeoutOnPreamble (`0x0209 01`) on, so a real preamble
  stops the clock. Preamble: return the channel index. Timeout: count a false alarm and carry on.

The host then waits for RxDone and reads the packet with the console's `rxread` (the RX FIFO can't
be read with `rc`: the data has to follow the opcode in the same SPI transfer).

The RX preamble length must match the transmitter's. With the console's default 16 and a 32-symbol
transmitter, about half the packets failed with header CRC errors, the same mismatch noted in
[commands.md](commands.md#behavior-of-documented-commands-measured-on-hardware).

### A long command has to run the ROM's event loop itself

The first version never saw CadDone: the flag only appeared after the command returned. Host
commands don't run in an interrupt (AUX_IRQ_ACT = 0, interrupts enabled inside the command). They
run from the ROM main loop (0xA1AC), which:

1. with interrupts off, takes the pending-event word at gp+0x44 (gp = 0x806000) that the
   interrupt handlers fill, and clears it;
2. runs host commands for bit 0 (0x23F4, which is where a PRAM command executes);
3. hands the other bits to the current state handler, a pointer at gp+0x34 (0x8074 for LoRa CAD),
   which raises the host IRQs (through 0x6ADC, which writes 0xF2A008) and moves the radio to its
   next state;
4. calls the callback at gp+0x38 with 256 if event bit 14 is left, ORs the rest into gp+0x6C,
   refreshes the host-visible status (0x25A4), and sleeps when nothing is pending.

The radio hardware keeps going while a command holds the CPU (that is why captures work from
inside a command), but anything the ROM finishes in software (CAD results, RX/TX state changes,
IRQ flags) waits for the loop. The blob's `yield()` does steps 1 and 3–4 while it polls, leaving
bit 0 alone. With it, CAD runs normally under the command. See
[lr2021-cpu.md](lr2021-cpu.md#long-commands-and-the-rom-event-loop-hw).

### Limits

- The command must return within RadioLib's 1 s BUSY limit, so the host re-issues it (`scan.py`
  sizes it to ~600 ms). From the PC over USB the gap between commands is tens of ms; an MCU talking
  SPI directly would re-issue in well under a millisecond.
- BUSY is high while it scans: to transmit, the MCU waits for the command to return (at most the
  size it chose).
- The CAD detect peaks are the datasheet's two-symbol values for every CAD length. Higher peaks
  would cut false alarms with one-symbol CAD; not tuned.
- `set radio` resets the chip and clears PRAM; reload the blob after it.
