# RTToF ranging between the two bench boards [HW]

The LR2021 measures distance by round-trip time of flight (RTToF): the manager sends a ranging
request, the subordinate answers automatically after a fixed turnaround, and the manager
reports the round-trip time. `tools/ranging.py` runs it between the two bench boards using only
the console's raw `rc` command. It needs no firmware changes.

```
python3 tools/ranging.py --n 200 --sf 9 --out rng.csv          # NiceRF = manager, XIAO = subordinate
python3 tools/ranging.py --n 200 --sf 9 --no-patch              # without Semtech's PRAM patch
```

## Sequence

This follows Semtech's `usp` v1.2.1 radio layer (`smtc_rac_lora.c`). Both boards get:

1. `set radio` (chip reset) and `set tx`. The console configures the PA and RF switches.
2. Semtech's PRAM patch, loaded with `pram.load` (see [pram-patch.md](pram-patch.md)). The
   tool reads it from Semtech's `lr20xx_pram_lr2021.h` (downloaded once to `~/.cache/lr2021-re/`).
   It is not stored in this repo.
3. Standby RC, then these commands:

| Command | Value |
|---|---|
| SetPacketType 0x0207 | 0x04 (RTToF) |
| SetLoraModulationParams 0x0220 | SF, BW 500 kHz, CR 4/5 |
| SetLoraPacketParams 0x0221 | preamble 12, explicit header, CRC |
| SetLoraSyncword 0x0223 | 0x12 |
| SetDioIrqConfig 0x0115 | the board's IRQ DIO, the five RTToF IRQs (bits 2, 28–31) and timeout |
| SetRangingTxRxDelay 0x027B | Semtech's recommended value for the SF/BW (BW 500, SF9: 20305) |
| SetRangingParams 0x027C | 15 response symbols, normal mode |
| SetRangingAddr 0x0278 (subordinate) / SetRangingReqAddr 0x0279 (manager) | 0x32101222, all 4 bytes checked |

The subordinate then sits in SetRx (continuous). For each exchange the manager sends SetTx and
polls GetAndClearIrqStatus until RngExchVld (bit 30) or RngTimeout (bit 31). It then reads
GetRangingResult type 0 (24-bit signed raw value and RSSI) and type 2 (AGC gain step).
Distance = raw × 150 / (4096 × BW in MHz) m, so one raw unit is 7.3 cm at 500 kHz.

The subordinate's response is a TX the chip starts by itself. On the NiceRF board this works
without help from the MCU: the RF-switch DIOs follow the chip state and CE stays high.

## Results

Setup: 908 MHz, BW 500 kHz, −9 dBm drive on both boards, antenna ports on dummy loads, boards
side by side. So the radios couple only through leakage: a normal LoRa packet from the XIAO
arrives at the NiceRF at −111.5 dBm, SNR −3.7 dB, AGC at maximum gain (step 13). 200 exchanges
per row, none failed.

| SF | Patch | Median | σ | Raw min … max | Reported RSSI |
|---|---|---|---|---|---|
| 7 | on | −4.1 m | 7.8 m | −382 … 197 | −115 dBm |
| 9 | on | +1.9 m | 3.9 m | −126 … 157 | −115 dBm |
| 10 | on | +6.8 m | 2.7 m | −9 … 217 | −115 dBm |
| 9 | off | +3.2 m | 3.8 m | −105 … 212 | **−24 dBm** |

- **The spread comes from SNR.** σ drops with SF as the processing gain rises. At this signal
  level one exchange is good to a few metres; averaging 200 gets the median to a few tens of cm
  from run to run (SF9: +1.9 m, then +1.0 m with 100 exchanges in a later run).
- **The offset is uncalibrated.** Semtech's delay table was measured on their boards. This setup
  differs from theirs: the NiceRF has an external PA and FEM, the signal arrives at low SNR, and
  the median moves with SF (−4 to +7 m). To range for real, calibrate TxRxDelay per SF at a
  known distance with antennas.
- **Patch hook 220 fixes the ranging RSSI.** With the patch, RTToF reports −115 dBm (raw 230),
  close to the −111.5 dBm of a normal packet on the same link. Without it, it reports −24 dBm
  every time, which is wrong. That confirms what hook 220 does (it converts 0xF30B74/78 through
  ROM 0x7E30 into the result). The distances are the same with or without the patch here. The
  PLL hooks (267/268/274/275) zero the low 7 bits of the PLL frequency while ranging, so they
  probably only matter when those bits are nonzero.

## Not tried

- Extended mode (SetRangingParams bit 7, two exchanges whose average cancels Doppler), spy
  mode, and the timing-synchronization DIO.
- Real distances with antennas, and calibrating TxRxDelay.
- The fractional bandwidths (812/406/203/101 kHz), which need Semtech's
  `lr20xx_workarounds_rttof_results_deviation` register writes.
