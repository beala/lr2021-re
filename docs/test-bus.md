# Internal test bus on the DIO pads (0x012F) and DIO function codes

Undocumented command 0x012F routes an internal 13-bit test bus onto the chip's pads. This page
covers what the ROM does, and what three of the bus bits show on hardware. Data:
`analysis/test_bus_sweep.json`.

## 0x012F in the ROM

Handler 0xE958 → worker 0xE804. Parameters: `selector [A_hi A_lo B_hi B_lo C_hi C_lo]`.

- The test-bus control register is **0xF2F020**: bits 15:8 = selector, bit 0 = enable.
- The chip has **13 pads, pad *i* = DIO*i***, with pad configuration at `0xF02004 + 4i`
  (bit 1 output enable, bit 2 input enable, bits 5:4 pull) and pad mux at `0xF26004 + 4i`
  (bits 4:0). These are the same registers SetDioFunction uses.
- For each pad whose bit is set in mask **A**, the worker sets the pad mux to **0xA** (not in the
  datasheet: the test-bus function), output enable from mask **B** and input enable from mask
  **C**. Pad *i* then shows bus bit *i*.
- Without masks, the ROM picks defaults by selector, **mostly A = 0x1FFF: all 13 pads, including
  pads 0–4, which are SPI and BUSY**. That cuts off SPI until a reset. Always pass the masks.
- Selector 0xFF disables the bus and restores the pads (ROM 0x10B90, then 0x4888). A chip reset
  also clears it.
- Default masks exist for selectors 0x00–0x5F and for 0xD1, 0xE0, 0xE8, 0xE9, 0xF0, 0xF1;
  all 256 selectors are accepted.

Example (XIAO: DIO7, DIO8 and DIO11 are wired to the MCU; bits 7, 8, 11 = mask 0x0980):

```
rc 012F 0 D1 09 80 09 80 00 00      selector 0xD1 on pads 7, 8, 11
rc 012F 0 FF                        off, pads restored
```

## What bits 7, 8 and 11 show (XIAO + Wio-LR2021)

The console's `pinmon <ms>` counts edges on the three pads in hardware (GPIOTE → PPI → TIMER)
and samples the high fraction; `pinsnap <n>` takes raw port snapshots at ~7.1 MS/s. Each of the
255 selectors was swept with the radio in standby, in continuous LoRa RX on an idle channel, and
in RX while the other board sent packets.

| Selector(s) | Bits | Standby | RX | Interpretation |
|---|---|---|---|---|
| 0x41 | 7 | 32.3 kHz square wave | same | LF (32 kHz RC) clock |
| 0x40 | 7 (standby), 8 (RX) | ~50 % high, too fast to count | same | fast clock (HF clock?) |
| 0xD0, 0xD1, 0xE0, 0xE8, 0xE9 | 7, 8 | static | ~2.1 M edges/s, ~40 % high | fast digital activity, **RX only** |
| 0xD1, 0xE0, 0xE8, 0xE9 | 11 | 10–25 % high, too fast to count | same | switching signal; the XIAO runs the SIMO DC-DC, possibly its drive |
| 0x0A, 0x1A, 0x2A, 0x3A, 0x5A, 0x6A | 8 | 0 | 0 idle; high 58–79 % while packets arrive | **packet-in-progress** flag |
| 0xD2–0xDA | 8 | 0 | 1 idle; 0 once packets have arrived | latched flag (the IRQs were never cleared in this test: probably IRQ status) |
| 0xEA–0xF3 | 8 | 0 | 0 idle; 1 once packets have arrived | latched flag, opposite sense |
| most others | | static | static | levels that follow the low bits of the selector, and many change between standby and RX: mode/state flags |

Notes on the fast RX-only lines:
- Their edge rate doesn't scale with the LoRa bandwidth: the same at 62.5 and 125 kHz, static
  at 250 kHz, different again at 500 and 1000 kHz.
- They toggle faster than the 7.1 MS/s port sampling (most runs are one sample), so the
  snapshots are aliased. They show strong periodicity on a 125 kHz grid (32 MHz / 256).
- A CW carrier 10 kHz off the RX frequency changed nothing. If these were I/Q sign bits, a slow
  10 kHz square wave would have shown up even at this sample rate.

Not found: demodulated data or a bit clock. That is a statement about 3 of 13 bus bits at
7 MS/s; the other pads are SPI or not wired to the MCU on these boards.

## DIO function codes (SetDioFunction 0x0112)

ROM 0x4944 validates the arguments and 0xEF14 configures the pad from a jump table:

- **DIOs 5–13** are accepted (the datasheet says 5–11). DIO12/13 aren't on the package pinout
  as far as we know; untested.
- Function codes 0–10 are accepted, 11+ are rejected. Codes 9 (TX_TRIGGER) and 10
  (RX_TRIGGER) are documented triggers (datasheet 5.5): they turn the pad into an input and
  register an edge handler.
- **Codes 3 and 4 (RFU) jump straight to the error exit.** On hardware they return CMD_FAIL
  (−706), like 0xB and 0xF, and leave the pad untouched. They're dead slots.
- DIO5 needs pull setting 2 for any function other than none; DIO10–13 have extra conditions
  (LF clock source, RAM flags).

## Safety

- Only route pads that nothing else drives. On the NiceRF carrier, DIO5–8 switch the 2 W PA,
  LNA and FEM: don't route them.
- Keep SPI pads (0–4) out of mask A. If the chip stops answering, reset it.
