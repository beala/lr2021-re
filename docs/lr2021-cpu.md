# Programming the LR2021's CPU

Everything learned so far about running our own code on the LR2021's embedded processor:
what the core is, where code and data can go, how the host calls it, how it shares the chip with
the radio hardware, the toolchain, and what hangs the chip. Worked examples (a spectrum sweep and
a 464 frames/s FFT waterfall) are in [pram-custom-commands.md](pram-custom-commands.md); their
source is in `firmware/pram/`.

Tags: **[HW]** = measured or verified on hardware, **[RE]** = read from the ROM,
**[guess]** = unverified.

## At a glance

| | |
|---|---|
| Core | Synopsys ARCv2 EM (IDENTITY 0x46), **reduced 16-entry register file** [HW] |
| Clock | **32.1 MHz** (10⁶ one-cycle loop iterations in 31.3 ms); **32.0 MHz** in TX, from SDR-timed cycle loops ([pram-pll.md](pram-pll.md)) [HW] |
| Code memory | ROM 80 KB at 0x000000; **PRAM 4 KB at 0x101000**, the only place our code can run [HW] |
| Data memory | DCCM RAM from 0x800000 (data only, not executable) [HW] |
| Multiplier | `mpy`/`mpyw`/`mpyu`..., 3 cycles, no DSP extensions [HW]/[RE] |
| Timers | none (TIMER_BUILD = 0) [HW] |
| Entry point | ActivatePram (0x012D) sub-commands: `0x012D k <params>` runs entry k of a PRAM table [HW] |
| Free PRAM | all 4 KB when Semtech's patch isn't loaded (RadioLib never loads it) [HW] |

## The core

### Registers [HW]

RF_BUILD = 0x4B02 (bit 9 set): the reduced register file. Only **r0–r3, r10–r15 and r26–r31**
exist. r4–r9 and r16–r25 don't: using one faults, e.g. `enter_s [r13-r19, ...]` hangs the
chip. The ROM itself sticks to the 16 that exist.

| Register | Use for our code |
|---|---|
| r0–r3, r10–r12 | scratch (arguments in r0–r3; the ABI's r4–r7 argument registers don't exist) |
| r13–r15 | callee-saved |
| r26 (gp) | **the ROM's**: small-data base 0x806000, set at reset. Don't change it while interrupts are on |
| r27 (fp) | usable, but not together with `enter_s [... fp ...]` (see pitfalls) |
| r28 (sp) | stack, top 0x8077F8, grows down |
| r29 (ilink) | interrupt return address: off limits while interrupts are enabled |
| r30 | general purpose on ARCv2 |
| r31 (blink) | return address; free inside a function that saved it |

So C code gets 12–13 registers (GCC needs a patch for the 13th, below) and a hand-written leaf
routine 13 plus `lp_count`.

### Timing [HW]

Measured with PRAM benchmark entry 10 (`rc 012D 0 0A <mode>`, timed by the console's `us=`):
10⁶ iterations of a zero-overhead loop around one instruction type.

| Loop body | Cycles per iteration |
|---|---|
| `nop_s` | 1.0 |
| 2 × `nop_s` | 3.0 (a loop body over one instruction costs one extra cycle per iteration) |
| 2 × 32-bit `add` with a 32-bit constant | 4.0 |
| `ldh.x` from RAM | 2.0 |
| `sth` to RAM | 1.0 |
| `mpyw` (16 × 16) | 3.0 |
| `mpy` (32 × 32) | 3.0 |

- Zero-overhead loops (`lp`, `lp_count`) work.
- Using a load or multiply result right away appears to stall: reordering the same
  instructions so the multiplies issue before unrelated loads made the FFT ~5% faster (see
  Scheduling under Toolchain). [guess]: inferred, not measured per instruction
- Reads from RAM are faster than reads from PRAM, so tables used in hot loops are better
  built in RAM (the FFT builds its twiddles at 0x807800).
- **Open:** the same FFT benchmark ran 5% faster (11.50 vs 12.11 ms for 10 FFTs) when the chip
  was left in FS than in standby RC. The CPU clock may depend on the chip's state, e.g. RC
  oscillator vs crystal. The 32.1 MHz figure hasn't been re-measured per state. In TX (CW), cycle-counted loops timed
  on an SDR put it at 32.0 MHz ([pram-pll.md](pram-pll.md)). [HW]/[guess]

### Multiplier and extensions

- MULTIPLY_BUILD = 0x020506. The DSP field is 0. [HW]
- The ROM uses `mpy`, `mpyw`, `mpyuw`, `mpym` and `mpyu`. The few `dmpyh`/`macu` hits in the
  disassembly are data bytes, not code, so there are no dual 16-bit MACs to build a complex
  multiply from. [RE]
- No timers (aux 0x21 reads fail with −707). The 2 actionpoints are reachable through 0x012C. [HW]

### Interrupts [RE]

The ROM masks and unmasks interrupts itself (`clri` × 18, `seti` × 19). Untested ideas:
- With interrupts off around a ~1 ms computation, gp and ilink would also be free (15
  registers for C, 16 in assembly once sp is parked in RAM).
- Radio interrupts would be delayed meanwhile, but BUSY is high during our commands anyway.

## Memory

All addresses are the CPU's. The host reaches the same ones with ReadRegMem32 (0x0106) and
WriteRegMem32 (0x0104), except for the PRAM write window. Full map: [memory-map.md](memory-map.md).

| Range | What | For our code |
|---|---|---|
| 0x000000–0x013FFF | ROM | callable, see [Calling the ROM](#calling-the-rom-hw) |
| 0x101000–0x101FFF | PRAM, 4 KB | code, constants, initialised data. Read-only view; written through 0x801000 |
| 0x801000–0x801FFF | write-only window into PRAM (host) | how a blob is loaded |
| 0x800000–... | RAM (DCCM) | data only |
| 0x804000–0x805FFF | capture / packet DMA bank | **owned by the DMA while a capture or a LoRa RX runs** |
| 0x806000 | gp, ROM small data | don't touch |
| below 0x8077F8 | stack (top), grows down | |
| 0x8077F8–0x80780B | Semtech patch variables (5 words) | unused without the patch; overlaps the start of the free area below |
| 0x807800–0x807FFF | above the stack, never written by the ROM in our tests | free: 2 KB of working RAM |
| 0x802000 | "live data", purpose unknown | not proven free |

How 0x807800 was checked: over a receive, a capture and FFTs it kept its random power-up
contents, while only 0x8076F8–0x8077BC changed (deepest stack use). The FFT also keeps a marker
word there and rebuilds its table if anything ever overwrites it. [HW]

**There is no `.bss` clearing and no C runtime.** Only what's in the uploaded image exists:
- globals need an initial value, placed in `.data`;
- RAM outside the image starts with random power-up contents.

## Running code: PRAM sub-commands

### Blob layout [RE]/[HW]

From the ROM (`pram_check` 0x24FC, ActivatePram handler 0x10AC4):

| Address | Contents |
|---|---|
| 0x101000 | magic 0x600DB002 |
| 0x101004 | low byte = number of entries |
| 0x101008 | entry table: one code address per entry |

Entry 0 is the install routine: it runs at activation and must return 2. Entries 1..n−1 are
sub-commands.

### Loading [HW]

1. Write the image with WriteRegMem32 to host address 0x801000 (write-only).
2. Activate: `0x012D 00`. `pram_check` finds the magic, stores `[0x8007F0]` = 0x101008 and
   `[0x8007F4]` = 0x101000, sets the bank mapping (aux 0x80000008) and calls entry 0.
3. Only now can the code be read back at CPU 0x101000. Reading it before activation stalls the
   chip.

**Reloading:** writing PRAM while a blob is active stalls the chip (−705 on the next command).
Reset the chip first (`set radio …` in the console re-initialises it).

### Calling convention [HW]

`0x012D k <params>` with 1 ≤ k < entry count calls entry k with:

| Register | Value |
|---|---|
| r0 | parameter length (without the k byte) |
| r1 | pointer to the parameters |
| r2 | pointer to a response struct `{u32 length; u8 *buffer}` |

Return value = command status: 2 OK, 1 parameter error, 0 fail, **3 data** (fill
`buffer`, set `length`, as the ROM's getters do, e.g. 0x0291 at 0x5B44). The host reads the
response with the usual read frame; larger results go to RAM for ReadRegMem32. The nRF52 side
reads at most 32 words per call.

- BUSY stays high while the handler runs. RadioLib gives up after 1 s (−705), so split long
  jobs into several commands.
- Normal ARC calling convention otherwise: r13–r15 callee-saved, blink = return address.
- The CPU is ours while a handler runs, but the radio hardware's DMAs keep working (next section).

### Calling the ROM [HW]

Handlers take the same `(len, params, resp)` arguments, so a PRAM command can call them directly.
Pass `resp = 0` when no response is needed.

| Address | Routine | Notes |
|---|---|---|
| 0xFC00 | SetRfFrequency (0x0200) | r0 = 4, r1 = big-endian Hz. Accepted mid-RX; RSSI follows |
| 0x5C4C | instant RSSI | returns 9 bits, dBm = −value / 2 |
| 0xF9A0 | capture config (0x0290) | 10 bytes to start, 6 to stop |
| 0x5B44 | capture write pointer (0x0291) | fills a 2-byte response |
| 0xFCFC | SetRx (0x020C) | |
| 0xEB40 | SetFs | |

More ROM functions: [rom-functions.md](rom-functions.md).

## Working alongside the radio hardware [HW]

- **DMA and CPU run in parallel.** The capture DMA keeps filling RAM while our code holds the
  CPU, so a command can start a capture, compute on something else and collect it later.
  This is what makes the overlapped waterfall work.
- **DMA-owned bank.** While a capture is armed, or the packet engine receives LoRa, the bank
  0x804000–0x805FFF belongs to the DMA. A CPU *or* host access to it stalls the chip until a
  reset. Keep work buffers and results elsewhere (PRAM after the code, or 0x807800+). Stop the
  capture (0x0290 mode 0) before touching the bank.
- **A capture starts only on SetRx from FS.** Re-arming 0x0290 while the receiver keeps running,
  or a second SetRx while already in RX, doesn't start it.
- **SetRx from FS takes 0.19 ms, from standby 1.27 ms** (synthesizer lock). End each job in FS
  if another receive follows.
- **RSSI lags a retune:** at 62.5 kHz bandwidth, ~1.4 ms after SetRfFrequency before the
  instant RSSI reflects the new frequency.

### Long commands and the ROM event loop [HW]

Hardware runs on while a command holds the CPU, but whatever the ROM finishes in software waits.
A PRAM command that started CAD and polled for CadDone never saw it; the flag appeared as soon as
the command returned.

- Commands don't run in an interrupt: inside one, AUX_IRQ_ACT (aux 0x43) reads 0 and STATUS32
  0x80000608 (interrupts enabled).
- The ROM main loop (0xA1AC), with interrupts off, takes the pending-event word at gp+0x44
  (gp = 0x806000, filled by interrupt handlers) and clears it. Bit 0 runs host commands (0x23F4);
  the other bits go to the current state handler, a function pointer at gp+0x34 (e.g. 0x8074 for
  LoRa CAD). That handler raises host IRQs through 0x6ADC (masked by [0x80060C], written to
  0xF2A008) and picks the radio's next state. Then the loop calls [gp+0x38](256) if event bit 14
  is left, ORs the remaining bits into gp+0x6C, refreshes the host status (0x25A4, registers
  0xF24040/44) and sleeps when idle.
- The IRQ status the host reads is 0xF2A00C (GetAndClearIrqStatus reads it and writes the value
  back to clear; ClearIrq writes the mask).
- **To wait for a radio event inside a command, run that dispatch yourself:** take gp+0x44 with
  interrupts off (`clri`/`seti`), put bit 0 back, call the state handler with the rest, then do the
  bit-14 callback, the gp+0x6C OR and 0x25A4. `yield()` in `firmware/pram/scan.c` does this; with
  it, CAD and the CAD-to-RX transition work under a command
  ([pram-scanner.md](pram-scanner.md)).
- **Or take the state handler slot instead, and no command is needed:** put your own function at
  gp+0x34, call the handler that was there with the events, then act on the result. Take the slot
  back after any ROM call that replaces it. The background channel scanner runs this way
  ([pram-scanner.md](pram-scanner.md#background-mode-hw)). The ROM's event bits seen set by its
  interrupt handlers are 0, 1, 6, 13, 14, 18 and 22–25.
- **BUSY drops only when the main loop goes idle.** Right before its sleep instruction the loop
  writes 0x80000000 to 0xF24004 (only after it has done work). A blob that kept setting an event
  bit, so the loop never went idle, held BUSY high: its own start command never returned (host
  −705).
- There's no CPU timer: TIMER_BUILD (aux 0x75) reads 0 and reading COUNT0 (aux 0x21) hangs the
  chip. The radio's RX timeout is a channel of a 32.768 kHz timer block: 0xC7F4(ticks) arms it,
  0xC828 stops it (armed flag at gp+12), 0xC84C(ticks) reloads it.

### Interrupts [HW]/[RE]

- IRQ_CTRL (aux 0x0E) is 0: on an interrupt the hardware saves only PC and STATUS32. Some of the
  ROM's priority 0 handlers use r0–r2 without saving them; its priority 1 handlers (e.g. 0x7A90)
  save r0–r3, r10–r12, r30, blink and lp_count/lp_start/lp_end by hand.
- The vector table is at address 0 (INT_VECTOR_BASE, aux 0x25, written by the ROM's init through
  0x103C4). Entries 16–50 are IRQs; many handlers read a status register at 0xFxx010, write it
  back at 0xFxx004 to clear it, and call a hook from a RAM table (0x806860, 0x806870, 0x8060DC…)
  or set a main-loop event bit.
- **A blob can take over an interrupt:** copy the 52 vectors to a 1 KB aligned PRAM address,
  replace one, and point INT_VECTOR_BASE at the copy. IRQ_SELECT (0x40B) + IRQ_PRIORITY (0x206)
  set its priority. The APRS demodulator does this for IRQ 42, the RX FIFO "full" flag
  ([pram-demod.md](pram-demod.md#running-in-the-background)).
- Known sources: IRQ 40 (0x7A90) the RX datapath block 0xF24010; IRQ 42 (0x78C4) the FIFO/DMA
  block, status 0xF30010 = flags 0xF30004 & enable 0xF30008; IRQ 39 the synthesizer block
  0xF40010.
- Host IRQs are raised with 0x6ADC(mask), which applies the gate at 0x80060C (0xFFFF0880 from the
  ROM's init, 0x6A6C) and writes 0xF2A008; RxDone is bit 18. The status lands in 0xF2A00C. Which
  pin each bit drives is separate: SetDioIrqConfig (0x4A4C) writes the pin of bit n to
  0xF2A014 + 4n and enables it in 0xF2A010.

## Toolchain

`firmware/pram/build.sh` uses a patched ARC GCC 13.3 in Docker (`docker/Dockerfile.gcc`,
~15 min to build), Ubuntu's ARC binutils, and the linker script `pram.ld` (`.text`, `.rodata`,
`.data` packed from 0x101000).

```
-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin
-fno-tree-loop-distribute-patterns -fno-pic
-fcall-saved-blink -mtune=arc700 -fschedule-insns -fsched-pressure
```

with `__attribute__((optimize("O2")))` on hot functions.

### The GCC patch: blink as a 13th register

With `-mrf16`, stock GCC allocates only 12 registers: r0–r3, r10–r15, r30 and fp. blink could
be a 13th in any function that saves it, but GCC never uses it, even with `-fcall-saved-blink`.
The reason: `arc_epilogue_uses` reports blink live at the function exit, so the dataflow sees
it live from entry to exit, and it conflicts with everything.

`docker/gcc-arc-blink.patch` makes two changes, both only when `-fcall-saved-blink` is given:

1. `arc_epilogue_uses` reports blink live only once the epilogue exists (as GCC's ARM port does
   for lr). The prologue then saves blink whenever it was allocated, the epilogue restores it
   before `j_s [blink]`, and calls already clobber it explicitly.
2. `REGNO_OK_FOR_BASE_P` accepts blink as an address base register, as it already does for r30
   when r30 isn't fixed. Without this, the allocator (whose base class includes blink) can put
   an address in blink, and a later check rejects it with an internal compiler error. The
   hardware's 32-bit loads and stores take r31 as a base like any other register.

Without the flag GCC behaves as before.

#### Testing the patch

GCC's own test suite needs a simulator to run its execution tests, and there's no free one for
ARC EM. Instead, `docker/check-gcc-patch.sh` compiles GCC 13.3's torture and ARC target tests
(5,197 files) to assembly with the patched compiler and with the same compiler built unpatched,
and compares:

| Configuration | Result |
|---|---|
| `-O2 -mrf16` and `-O2` (full register file), no flag | **byte-identical assembly** for all 4,828 files that compile |
| `-O2 -mrf16 -fcall-saved-blink` | no compiler errors (unpatched GCC: 3); 158 functions in 73 files allocate blink |
| `-Os -mrf16 -fcall-saved-blink` | the same 2 errors as unpatched GCC (`taux-1.c`, `uncached-6.c`, unrelated); 46 functions allocate blink |
| `-O2 -fcall-saved-blink` | no compiler errors (unpatched: 1); 26 functions allocate blink |

In every function that allocates blink, the prologue saves it and, if the function returns,
the epilogue restores it (functions ending in `exit` don't). Unpatched GCC's own
`-fcall-saved-blink` errors were the same base-register problem, so change 2 fixes those too.

A first version of the patch (change 1 only) passed the FFT on hardware but failed this check:
up to 11 new compiler errors per configuration, all blink used as an address base. Not covered:
running the execution tests. Generated code with blink is only checked statically, plus the FFT
results on the chip.

### Scheduling

GCC has no pipeline model for ARC EM, so it loads everything first and the multiplies wait on
the loads. `-mtune=arc700 -fschedule-insns` borrows the ARC700 model, which issues the
multiplies before the remaining loads.

Effect on the FFT butterflies (per 256-point FFT, on the chip):

| Build | Per FFT | Image (limit then 3072 B) |
|---|---|---|
| stock GCC, butterflies in hand-written assembly | 1.294 ms | 3032 B |
| patched GCC, assembly | 1.284 ms | 3036 B |
| patched GCC, assembly, ARC700 schedule | 1.271 ms | 3044 B |
| stock GCC, C butterflies, ARC700 schedule | 1.260 ms | 3084 B: didn't fit |
| **patched GCC, C butterflies, ARC700 schedule** | **1.211 ms** | 3068 B |

With both, plain C beats the hand-scheduled assembly.

### Pitfalls

- **Assembly files need `.arc_attribute Tag_ARC_ABI_rf16, 1`**, or the linker refuses to mix
  them with `-mrf16` C objects.
- **`-mno-sdata`:** gp belongs to the ROM.
- **No libc or libgcc:**
  - stack arrays with initialisers become `memcpy` calls: use `static const` tables;
  - variable 64-bit shifts become `__ashldi3`: use 32-bit halves.
- **`.bss` isn't uploaded:** give globals an initial value in `.data`. `build.sh` fails if
  `.bss` isn't empty.
- **`bl` targets must be 4-byte aligned** (`.align 4`): an unaligned target is silently rounded
  down.
- **Don't use fp as a scratch register with `enter_s [... fp ...]`:** `leave_s` restores sp
  from it.
- **An inline `asm` statement in a loop** (even an empty one used to force an ordering) stops
  GCC from using the zero-overhead `lp` loop.
- **On the nRF52 host side**, SPI buffers must be in RAM (EasyDMA can't read flash). A `const`
  zero buffer for a response frame sends garbage, and the next command reports CMD_PERR.

## What hangs the chip

All of these need a chip reset (`set radio …` or `hold off` in the console).

| Cause | Symptom |
|---|---|
| Using r4–r9 or r16–r25 (e.g. `enter_s [r13-r19]`) | fault, command never returns |
| CPU or host access to the DMA-owned bank 0x804000–0x805FFF during a capture or LoRa RX | bus stall |
| Reading PRAM (0x101000) before activation | bus stall |
| Writing PRAM while a blob is active | next command −705 |
| Reading 0x100000 or 0x102000 (holes) | bus stall |
| Reading 0xF2A008, 0xF24040 and up, or 0xF30060–0xF3007C | bus stall, host −705 |
| Reading aux COUNT0 (0x21): there are no ARC timers | host −705 |
| A handler running over 1 s | RadioLib −705 (the chip is fine once it finishes) |
| Waiting for a capture that never starts (e.g. no SetRx from FS) | command spins until its poll limit, host −705 |

## Open questions

- Does the CPU clock change with the chip's state (the 5% above)?
- Can interrupts be masked around a computation to free gp and ilink?
- What lives at 0x802000, and is more RAM safely free?
- Overlapping captures for multi-segment averaging needs ~2 KB of accumulators outside the
  capture bank.
- Radix-4 FFT: worth ~0.25 ms per frame now that the frame is compute-bound.
