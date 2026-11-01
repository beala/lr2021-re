# Demodulating on the chip: APRS in the background [HW]

The LR2021 has no AFSK modem, but its CPU can read the raw I/Q the radio streams into its RX
FIFO. A PRAM blob (`firmware/pram/demod.bin`, source `demod.c` + `afsk_rx.h`) decodes APRS
(AFSK 1200, Bell 202) from that stream on the chip and works like a built-in receiver: one
command starts it and returns, the chip demodulates in the background on an interrupt, and each
good frame raises **RxDone** on the IRQ line. The MCU sleeps until then and reads only the
frame. Nothing is streamed over SPI.

```
python3 tools/aprs_rx.py bench --trials 20     # the other board sends APRS from its PLL, this one decodes
python3 tools/aprs_rx.py listen --seconds 60   # print frames as the chip decodes them
python3 tools/aprs_rx.py iq                    # check: a CW tone through the buffer switching
```

The test signal is the other board's on-chip AFSK generator ([pram-pll.md](pram-pll.md)): 908 MHz,
−9 dBm drive, dummy loads, frames from the N0CALL placeholder. `listen` prints whatever is on the
channel; other people's traffic isn't something to publish.

## Results

| Receiver | Frames (random text, 30 flags preamble) | RxDone interrupts | Demodulated | FIFO overruns |
|---|---|---|---|---|
| XIAO | **20/20** | 20 | 42.4 s (43,800 buffers) | 0 |
| NiceRF board | **20/20** | 20 | 42.4 s (43,783 buffers) | 0 |

- No false frames. One bad FCS in 42 s of the NiceRF run (noise between frames that happened to
  look like a frame end).
- **CPU load:** the interrupt handler takes at most 0.39 ms (~12k cycles) of each 0.96 ms buffer,
  measured as how far the next buffer had filled when it finished. The chip answers host commands
  normally meanwhile.
- **Continuity:** with a CW tone at +2 kHz and the decimated I/Q recorded instead of frames, the
  phase step per sample has an rms deviation of 0.15 rad (noise, |z| ≈ 15) and no offset at the
  buffer switches. A lost input sample would show as a 0.058 rad step.
- The same C core on a PC decodes both frames of a 125 kS/s recording (decimation 4 instead of
  8), with one sample dropped every 120, and with added noise down to 0 dB SNR in 125 kHz
  (`firmware/pram/test_afsk_rx.c`).

## The receiver (`afsk_rx.h`)

Integer only and no library calls, so the same file builds for the chip and for a PC:

1. **Decimate** source 0 (int8 I/Q, 250 kS/s with the 62.5 kHz LoRa setting) by 8 to 31.25 kS/s
   by summing pairs. The channel filter (−3 dB ≈ ±35 kHz) passes the ±3 kHz FM with room for
   frequency error.
2. **FM discriminator:** the cross product of consecutive samples (Q·I′ − I·Q′), scaled by a
   shift that follows its peak, so the output stays at about 9 bits whatever the signal level.
3. **Tones:** sliding correlation with 1200 Hz and 2200 Hz over one bit (26 samples), four
   running sums with ring buffers; mark when |mark| > |space| (|z| ≈ max + min/2).
4. **Bit clock:** a 32-bit phase advancing 1200/31250 per sample, pulled 25 % toward zero at
   each tone change; a bit is sampled when it wraps.
5. **NRZI** (same tone = 1), HDLC flags, bit unstuffing, then CRC-16/X.25 on the frame. Good
   frames go to the output as {u16 length, AX.25 bytes without the FCS}.

## Reading the FIFO from the CPU

The raw-I/Q route ([raw-iq.md](raw-iq.md#streaming-through-the-rx-fifo-hw)) fills the RX FIFO for
SPI reads. The FIFO is a buffer in chip RAM, and the host can put it in free RAM (0x807800) where
the CPU may read it safely while it fills. The problem is consuming it:

- **The read pointer isn't visible.** ReadRxFifo is handled by the SPI block in hardware.
  Writing 0xF3003C did nothing, and no register in 0xF2A000–0xF2A0B0, 0xF24000–0xF2403C or
  0xF30000–0xF3005C moved with the bytes the host read.
- So the FIFO can't be drained by the CPU, and **a full FIFO stops**.
- But two registers tell the CPU where it is. 0xF2A0AC (what GetRxFifoLevel reads) is the level.
  0xF30038 is the number of bytes in RAM, always 24 below the level: 24 bytes are in flight.

The way round it: **move the FIFO instead of draining it.** Writing a new base to 0xF30028 and then
setting bit 2 of 0xF3001C (one of the steps of ClearRxFifo) restarts the FIFO at the new base.
Two buffers then take turns: while one fills, the CPU demodulates the other. A CW tone across
the switch, near 0 Hz and at 2.3 kHz (to resolve the phase ambiguity), measured the gap:

| Restart with | Gap at the switch |
|---|---|
| ClearRxFifo (ROM 0x6478: seven register writes) | 17–18 samples (~70 µs) |
| 0xF3001C bit 2 only | **−1 to 0 samples** |
| other single steps of ClearRxFifo | don't restart the FIFO |

**The 24 bytes in flight land at the new base.** Taking the old buffer's length from the level
(0xF2A0AC) instead of the RAM count (0xF30038) repeats them: the phase of a CW tone stepped back
by ~0.6 rad (≈ 10 input samples) at every switch, enough to break HDLC framing.

## Running in the background

A blocking command that polls the FIFO works too (entry 4: 3/3 frames, 0.96 ms buffers, ~1000–1500
spare polls each). It holds BUSY, though, and the host must call it again within a millisecond to
keep the stream. The background mode uses an interrupt instead.

**A CPU interrupt for "buffer ready".** The FIFO threshold IRQs of ConfigFifoIrq are set up in the
SPI block (enable 0xF2A004, thresholds 0xF2A098/0xF2A09C), and no CPU interrupt handler reads
that block: they go to the host's DIO pins only. The FIFO/DMA block has its own flags, and those
reach the CPU as IRQ 42:

| Register | Meaning |
|---|---|
| 0xF30004 | FIFO flags (write 1 to clear): bit 3 full, bit 4 overflow. GetFifoIrqFlags reports bits 3–4 as FifoFull / FifoOverflow |
| 0xF30008 | which flags interrupt the CPU (0x10000 after boot) |
| 0xF30010 | 0xF30004 & 0xF30008, the status the IRQ 42 handler reads |
| 0xF30030 | FIFO size: "full" sets when the RAM count reaches size + 4, "overflow" on the next sample |

So the FIFO size becomes the buffer size (480 bytes = 240 samples = 0.96 ms), and "full" is the
interrupt. The sample after full is lost while the interrupt is taken (4 µs per sample, a few µs
of latency); the CW check above shows no measurable offset.

**Taking over IRQ 42.** The ROM's handler (0x78C4) only turns bits 16 and 24 into main-loop events
and has no hook. The vector table is at address 0 (INT_VECTOR_BASE, aux 0x25, set by the ROM's
init through 0x103C4), so entry 6 (start):

1. copies the 52 vectors to PRAM 0x101C00 (1 KB aligned) and puts its own handler in slot 42;
2. sets IRQ 42 to priority 1 (IRQ_SELECT 0x40B, IRQ_PRIORITY 0x206). IRQ_CTRL is 0, so the
   hardware saves only PC and STATUS32. Some of the ROM's priority 0 handlers use r0–r2 without
   saving them (the IRQ 42 handler does), but its priority 1 handlers (e.g. 0x7A90) save r0–r3, r10–r12, r30, blink and the loop registers by
   hand, and `isr42` (`demod_hdr.S`) does the same before calling C;
3. points INT_VECTOR_BASE at the copy, sets the FIFO size and enables bit 3 in 0xF30008, and
   returns.

The handler switches buffers, clears the flags, demodulates the full buffer and does what the
ROM's handler did for the other bits. When a frame is complete it calls the ROM's host-IRQ
routine 0x6ADC with RxDone (bit 18). That routine applies the IRQ gate ([0x80060C], 0xFFFF0880
from the ROM's init) and writes 0xF2A008, so the IRQ line rises as for a received packet. Stop (op 0) restores the vector base and priority.

## Entries (`demod.bin`)

| Entry | Params | What it does |
|---|---|---|
| 1 | addr (4), n (1) | read n ≤ 16 words |
| 2 | – | returns `DEM1` |
| 3 | addr (4), value (4) | write a word, read it back |
| 4 | buffers (2), T (2), flags (1) | blocking receiver: polls the FIFO for that many buffers. Flags: 1 start, 2 debug I/Q, 4 empty the output, 8 return on a frame |
| 6 | op (1): 1 start (+ T (2), flags (1)), 0 stop, 2 status, 3 drop n (2) output bytes | background receiver |

Entries 4 and 6 return 11 words: buffers, most bytes in a buffer, a timeout flag (entry 4) or
INT_VECTOR_BASE (entry 6), handler end point (bytes into the next buffer when it finished),
overruns, then the receiver's samples, flags, frames, bad FCS, frames not stored and output bytes.
Frames are at PRAM 0x101A00 (512 bytes). The receiver state is at RAM 0x807C00, and the FIFO
buffers are at 0x807800 and 0x807A00.

The host side (`tools/aprs_rx.py`): load the blob, start the raw stream (packet type 0x0B, 0x0290
source 0 with the FIFO on, FIFO base 0x807800, SetRx), entry 6 op 1, then the console's
`aprswait`. That waits for the IRQ pin, clears RxDone, reads the output and drops what it read
(op 3), so a frame decoded meanwhile isn't lost.

## Limits and notes

- The receiver uses the LoRa 62.5 kHz bandwidth setting only because source 0 is known there
  (250 kS/s). A narrower setting would cut the decimation work.
- Pre-emphasis isn't handled: the test transmitter sends both tones at the same deviation. A
  real FM radio's twist would bias the mark/space decision.
- Only one frame buffer: 512 bytes, a few typical APRS frames. A frame that doesn't fit is
  counted as lost.
- PRAM is nearly full: code 2.5 KB, frame output 0.5 KB, vector table copy 208 bytes.
- `set radio` resets the chip, which clears PRAM and the vector base; reload after it.
- Console firmware notes from this work: `micros()` on the nRF52 core wraps well before 2³², so
  long BUSY waits now count in `millis()`. A line printed after more than ~2 s without USB output
  didn't reach the PC, so `aprswait` prints a keep-alive line every second.
