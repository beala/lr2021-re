// LR2021 bit-rate bench: how fast can LoRa and FLRC actually move data between two boards?
//
// One firmware for both boards; the role is picked over serial (115200, line-based).
// RadioLib does the setup (frequency, calibration, PA, RF switch); the per-packet path uses
// raw SPI at up to 16 MHz so host overhead stays small next to the airtime.
//
// Commands (one per line):
//   spi <Hz>                                  SPI clock for the fast path (default 16000000)
//   freq <MHz>                                set + calibrate frequency
//   pwr <dBm>                                 TX power
//   lora <bw kHz> <sf> <cr 1..4> <pre> <implicit 0|1> <crc 0|1> <len>  [ldro 0..3] [mode 0..3]
//                                             cr 1..4 = 4/5..4/8; len only matters for implicit
//   rc <op> [bytes] / rr <op> <n> [bytes]     raw write / read command (hex), prints the status
//   ptx <len> <ms>                            one packet with the chip as it is (no modem setup)
//   flrc <kbps> <cr 0..3> <pre 0..7> <crc 0..3> <shape>
//                                             cr: 0=1/2 1=3/4 2=1(uncoded) 3=2/3; pre: (n+1)*4 bits;
//                                             crc: 0 off, 1..3 = 16/24/32 bit; shape 0=off, else RadioLib code
//   toa <reps> <len> [len ...]                time SetTx -> TX_DONE per length
//   tx <count> <len> <pipe 0|1> <gap us>      back-to-back packets; pipe=1 loads the next packet
//                                             into the TX FIFO while the current one is on air
//   rx                                        continuous RX, 1 s stats; x stops and prints a summary
//   iqtx <rate S/s> <pairs> <t|c> <kHz> <amp> <bursts>
//                                             raw I/Q TX via undocumented 0x0293 (method from
//                                             github.com/SAMS0N1TE/lr2021-sdr): t = tone at +kHz,
//                                             c = chirp sweeping kHz wide (period 2048 samples),
//                                             n = white (random phase per sample), m = 5-tone (+25 -60 +110 -150 +190 kHz)
//   cap <bw kHz|cX> <ms>                      one-shot 8 KB raw IQ capture (0x0290, packet type 0x0B)
//                                             with the LoRa channel filter set to bw; dumps C, lines
//   afsk clear | afsk <nbits> <hex>           load AFSK 1200 tone bits for "iqtx <rate> <pairs> a <dev kHz> ..." (APRS)
//   fskafsk <bps> <dev Hz> <shape> <msb>      send the loaded AFSK bits through the GFSK modem (no raw IQ)
//   ft8 <79 tone digits>                      load an FT8 symbol sequence for "iqtx <rate> <pairs> f <base kHz> ..."
//   iqstream <ms> [mhz] [dec] [bits] [rate]   continuous raw IQ to the host (LRIQ framing, lr2021-re tools)
//   pll <MHz> [MHz ...]                       RX-only synthesizer range check: lock/calibration error flags
//   noise <ms> <f1> <f2> <step MHz>           RSSI_inst noise floor sweep (current modem's filter)
//   x                                         stop
//
// Output: A,... toa rows   T,... tx summary   S,... rx 1 s stats   SUM,... rx summary   #... info

#include <Arduino.h>
#include <SPI.h>
#include <algorithm>
#include <LR2021Board.h>

extern RADIO_CLASS radio;

struct LRAccess : public LR2021Dev {
  static LRAccess& of(LR2021Dev& r) { return static_cast<LRAccess&>(r); }
  using LRxxxx::SPIcommand;
  using LRxxxx::highFreq;
};
static LRAccess& lr = LRAccess::of(radio);

struct HalAccess : public ArduinoHal {
  using ArduinoHal::spiSettings;
};

// ---------------------------------------------------------------- timing (DWT, 64 MHz)

static void usInit() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
static void usStop() { DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk; }
static uint32_t cyc() { return DWT->CYCCNT; }
static uint64_t us64() {
  static uint32_t last = 0;
  static uint64_t hi = 0;
  uint32_t c = DWT->CYCCNT;
  if (c < last) hi += 1ULL << 32;
  last = c;
  return (hi | c) / 64;
}

// ---------------------------------------------------------------- raw SPI

static uint32_t spiHz = 16000000;
static SPISettings fastSpi(16000000, MSBFIRST, SPI_MODE0);
static uint32_t busyTimeouts = 0;

static bool waitBusy() {
  uint32_t n = 0;
  while (digitalRead(P_LORA_BUSY)) {
    if (++n > 2000000) { busyTimeouts++; return false; }  // ~100 ms
  }
  return true;
}

static bool xfer(const uint8_t* tx, uint8_t* rx, size_t n) {
  if (!waitBusy()) return false;
  SPI.beginTransaction(fastSpi);
  digitalWrite(P_LORA_NSS, LOW);
  SPI.transfer(tx, rx, n);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
  return true;
}

// Write command. MISO carries Stat(2) + IrqStatus(4) for commands of >= 6 bytes.
static uint32_t lastIrq = 0;
static uint16_t lastStat = 0;  // Stat of the previous command, from MISO bytes 0-1
static bool cmd(uint16_t op, const uint8_t* p = nullptr, size_t n = 0) {
  uint8_t tx[16] = { (uint8_t)(op >> 8), (uint8_t)op };
  uint8_t rx[16] = { 0 };
  if (n > 14) return false;
  if (n) memcpy(tx + 2, p, n);
  if (!xfer(tx, rx, n + 2)) return false;
  lastStat = ((uint16_t)rx[0] << 8) | rx[1];
  if (n + 2 >= 6) lastIrq = ((uint32_t)rx[2] << 24) | ((uint32_t)rx[3] << 16) | ((uint32_t)rx[4] << 8) | rx[5];
  return true;
}

// Read command: opcode frame, then a response frame clocked with zeros.
static bool rd(uint16_t op, uint8_t* data, size_t n) {
  uint8_t c[2] = { (uint8_t)(op >> 8), (uint8_t)op };
  uint8_t z[16] = { 0 }, r[16] = { 0 };
  if (n > 14 || !xfer(c, nullptr, 2) || !xfer(z, r, n + 2)) return false;
  memcpy(data, r + 2, n);
  return true;
}

static uint8_t fifoBuf[1040];
static bool writeTxFifo(const uint8_t* d, size_t n) {
  fifoBuf[0] = 0x00; fifoBuf[1] = 0x02;
  memcpy(fifoBuf + 2, d, n);
  return xfer(fifoBuf, nullptr, n + 2);
}

static bool readRxFifo(uint8_t* d, size_t n) {
  if (!waitBusy()) return false;
  uint8_t c[2] = { 0x00, 0x01 };
  SPI.beginTransaction(fastSpi);
  digitalWrite(P_LORA_NSS, LOW);
  SPI.transfer(c, nullptr, 2);
  SPI.transfer(nullptr, d, n);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
  return true;
}

// ClearIrq(all): the reply carries the IRQ status from just before the clear.
static uint32_t getAndClearIrq() {
  uint8_t m[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
  cmd(RADIOLIB_LR2021_CMD_CLEAR_IRQ, m, 4);
  return lastIrq;
}

static bool setTx() { uint8_t t[3] = { 0, 0, 0 }; return cmd(RADIOLIB_LR2021_CMD_SET_TX, t, 3); }
static bool setRxCont() { uint8_t t[3] = { 0xFF, 0xFF, 0xFF }; return cmd(RADIOLIB_LR2021_CMD_SET_RX, t, 3); }

static uint16_t rxFifoLevel() {
  uint8_t b[2] = { 0 };
  rd(RADIOLIB_LR2021_CMD_GET_RX_FIFO_LEVEL, b, 2);
  return ((uint16_t)b[0] << 8) | b[1];
}
static uint16_t rxPktLen() {
  uint8_t b[2] = { 0 };
  rd(RADIOLIB_LR2021_CMD_GET_RX_PKT_LENGTH, b, 2);
  return ((uint16_t)b[0] << 8) | b[1];
}

static void writeReg(uint32_t addr, uint32_t v) {
  uint8_t p[7] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                   (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
  cmd(RADIOLIB_LR2021_CMD_WRITE_REG_MEM_32, p, 7);
}

// datasheet 5.3.3: move both FIFOs to the 1 KB region. CalibFE overwrites their contents,
// so this runs after every calibration; the registers themselves persist in standby.
static void bigFifos() {
  writeReg(0xF3002C, 0x804000);
  writeReg(0xF30034, 0x3FC);
  writeReg(0xF30028, 0x804400);
  writeReg(0xF30030, 0x3E8);
  cmd(RADIOLIB_LR2021_CMD_CLEAR_RX_FIFO);
  cmd(RADIOLIB_LR2021_CMD_CLEAR_TX_FIFO);
}

// ---------------------------------------------------------------- config

enum Modem { M_LORA, M_FLRC };
static Modem modem = M_LORA;
static double freqMHz = 908.0;  // double: float32 rounds to ~30 Hz at 432 MHz
static int8_t txPower = 10;

struct LoraCfg { int16_t bwRaw = -1; float bw = 1000; uint8_t sf = 5, cr = 1; uint16_t pre = 8; uint8_t implicit = 0, crc = 1; uint16_t len = 255; int8_t ldro = -1; uint8_t mode = 0; } lc;
struct FlrcCfg { uint16_t kbps = 2600; uint8_t cr = 2, pre = 7, crc = 1, shape = RADIOLIB_LR2021_GFSK_BPSK_FLRC_OOK_SHAPING_GAUSS_BT_0_5; } fc;

static const uint32_t SYNC_FLRC = 0x2D014B1D;

static bool ck(int16_t st, const char* what) {
  if (st != RADIOLIB_ERR_NONE) { Serial.printf("# ERR %s -> %d\n", what, st); return false; }
  return true;
}

static uint8_t loraBwCode(float bw) {
  struct { float k; uint8_t c; } t[] = {
    { 31.25f, RADIOLIB_LR2021_LORA_BW_31 }, { 41.67f, RADIOLIB_LR2021_LORA_BW_41 }, { 62.5f, RADIOLIB_LR2021_LORA_BW_62 },
    { 83.34f, RADIOLIB_LR2021_LORA_BW_83 }, { 101.5625f, RADIOLIB_LR2021_LORA_BW_101 }, { 125, RADIOLIB_LR2021_LORA_BW_125 },
    { 203.125f, RADIOLIB_LR2021_LORA_BW_203 }, { 250, RADIOLIB_LR2021_LORA_BW_250 }, { 406.25f, RADIOLIB_LR2021_LORA_BW_406 },
    { 500, RADIOLIB_LR2021_LORA_BW_500 }, { 812.5f, RADIOLIB_LR2021_LORA_BW_812 }, { 1000, RADIOLIB_LR2021_LORA_BW_1000 },
  };
  for (auto& e : t) if (fabsf(e.k - bw) < 2.0f) return e.c;
  return 0xFF;
}

static uint8_t flrcBrCode(uint16_t k) {
  switch (k) {
    case 2600: return RADIOLIB_LR2021_FLRC_BR_2600; case 2080: return RADIOLIB_LR2021_FLRC_BR_2080;
    case 1300: return RADIOLIB_LR2021_FLRC_BR_1300; case 1040: return RADIOLIB_LR2021_FLRC_BR_1040;
    case 650: return RADIOLIB_LR2021_FLRC_BR_650;   case 520: return RADIOLIB_LR2021_FLRC_BR_520;
    case 325: return RADIOLIB_LR2021_FLRC_BR_325;   case 260: return RADIOLIB_LR2021_FLRC_BR_260;
  }
  return 0xFF;
}

// Packet params. TX: exact length. RX: maximum accepted length (dynamic formats).
static bool packetParams(uint16_t len) {
  if (modem == M_LORA) {
    return ck(radio.setLoRaPacketParams(lc.pre, lc.implicit ? RADIOLIB_LRXXXX_LORA_HEADER_IMPLICIT : RADIOLIB_LRXXXX_LORA_HEADER_EXPLICIT,
                                        len > 255 ? 255 : len, lc.crc, 0), "loraPacketParams");
  }
  return ck(radio.setFlrcPacketParams(fc.pre, 4, 1, 1, false, fc.crc, len), "flrcPacketParams");
}

// Raw SetLoraModulationParams: RadioLib's auto-LDRO ignores the ldro argument, and the ROM also takes
// an undocumented 2-bit field in bits 3:2 of byte 2 ("mode"). Prints the status of the command.
static bool loraRawMod() {
  uint8_t bw = lc.bwRaw >= 0 ? (uint8_t)lc.bwRaw : loraBwCode(lc.bw);
  float symUs = (float)(1UL << lc.sf) * 1000.0f / lc.bw;
  uint8_t ldro = lc.ldro >= 0 ? (uint8_t)lc.ldro : (symUs >= 16000 ? 1 : 0);
  uint8_t p[2] = { (uint8_t)((lc.sf << 4) | (bw & 0xF)), (uint8_t)((lc.cr << 4) | ((lc.mode & 3) << 2) | (ldro & 3)) };
  cmd(0x0220, p, 2);
  uint8_t m[4] = { 0, 0, 0, 0 };
  cmd(RADIOLIB_LR2021_CMD_CLEAR_IRQ, m, 4);  // its MISO carries the 0x0220 status
  Serial.printf("# raw 0220 %02X %02X stat 0x%04X\n", p[0], p[1], lastStat);
  return true;
}

static bool applyModem() {
  radio.standby();
  if (modem == M_LORA) {
    uint8_t bw = lc.bwRaw >= 0 ? (uint8_t)lc.bwRaw : loraBwCode(lc.bw);
    if (bw == 0xFF) { Serial.println("# ERR lora bw"); return false; }
    float symUs = (float)(1UL << lc.sf) * 1000.0f / lc.bw;
    return ck(radio.setPacketType(RADIOLIB_LR2021_PACKET_TYPE_LORA), "setPacketType")
        && ck(radio.setLoRaModulationParams(lc.sf, bw, lc.cr, symUs >= 16000 ? 1 : 0), "loraModParams")
        && ck(radio.setSyncWord((uint8_t)0x12), "loraSync")
        && packetParams(lc.implicit ? lc.len : 255) && loraRawMod();
  }
  uint8_t br = flrcBrCode(fc.kbps);
  if (br == 0xFF) { Serial.println("# ERR flrc rate"); return false; }
  return ck(radio.setPacketType(RADIOLIB_LR2021_PACKET_TYPE_FLRC), "setPacketType")
      && ck(radio.setFlrcModulationParams(br, fc.cr, fc.shape), "flrcModParams")
      && ck(radio.setFlrcSyncWord(1, SYNC_FLRC), "flrcSync")
      && packetParams(511);
}

static int16_t setFreqAny(double mhz) {
  const uint32_t hz = (uint32_t)(mhz * 1e6 + 0.5);
  if (mhz >= 150.0) {
    // RadioLib's setFrequency keeps its bookkeeping (path, calibration); then set the exact Hz
    int16_t st = radio.setFrequency((float)mhz);
    return st ? st : radio.setRfFrequency(hz);
  }
  // RadioLib refuses < 150 MHz (the datasheet floor); the synthesizer and front-end calibration
  // work down to ~102 MHz (see pll), so set it with the raw commands.
  int16_t st = radio.setRfFrequency(hz);
  if (st) return st;
  uint16_t cal[3] = { (uint16_t)((uint16_t)(mhz / 4.0 + 0.5) | RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH), 0, 0 };
  return radio.calibrateFrontEnd(cal);
}

static bool applyRf() {
  radio.standby();
  radio.setTxParams(0, 0);  // see dualband_bench: SetPaConfig fails if the old power is out of range
  return ck(setFreqAny(freqMHz), "setFrequency")
      && ck(radio.setOutputPower(txPower), "setOutputPower")
      && ck(radio.setRxPath(lr.highFreq ? RADIOLIB_LR2021_RX_PATH_HF : RADIOLIB_LR2021_RX_PATH_LF,
                            lr.highFreq ? radio.gainModeHf : radio.gainModeLf), "setRxPath");
}

static bool applyAll() {
  bool ok = applyModem() && applyRf();
  bigFifos();
  ck(radio.setRxTxFallbackMode(RADIOLIB_LR2021_FALLBACK_MODE_FS), "fallback");
  return ok;
}

static void printConfig() {
  Serial.printf("# freq %.6f MHz pwr %d dBm spi %lu Hz hf=%d\n", freqMHz, txPower, (unsigned long)spiHz, lr.highFreq);
  if (modem == M_LORA)
    Serial.printf("# lora bw %.3f (raw %d) sf %u cr 4/%u pre %u implicit %u crc %u len %u\n", lc.bw, lc.bwRaw, lc.sf, lc.cr + 4, lc.pre,
                  lc.implicit, lc.crc, lc.len);
  else
    Serial.printf("# flrc %u kbps cr %u pre %u bits crc %u shape %u\n", fc.kbps, fc.cr, (fc.pre + 1) * 4, fc.crc, fc.shape);
}

static bool stopRequested() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'x' || c == 'X') return true;
  }
  return false;
}

// ---------------------------------------------------------------- packets

static const uint8_t MAGIC = 0xDB;
static const size_t HDR = 7;  // magic, len(2), seq(4)

static void buildPacket(uint8_t* b, uint16_t len, uint32_t seq) {
  b[0] = MAGIC; b[1] = len & 0xFF; b[2] = len >> 8;
  memcpy(b + 3, &seq, 4);
  uint8_t v = (uint8_t)(seq * 7);
  for (uint16_t i = HDR; i < len; i++) b[i] = v + (uint8_t)i;
}

// 0 = good, 1 = corrupt payload, 2 = not ours / misaligned
static int checkPacket(const uint8_t* b, uint16_t len, uint32_t* seq) {
  if (len < HDR || b[0] != MAGIC || (uint16_t)(b[1] | (b[2] << 8)) != len) return 2;
  memcpy(seq, b + 3, 4);
  uint8_t v = (uint8_t)(*seq * 7);
  for (uint16_t i = HDR; i < len; i++) if (b[i] != (uint8_t)(v + i)) return 1;
  return 0;
}

// ---------------------------------------------------------------- TX

struct Stat {
  uint32_t n = 0, mn = UINT32_MAX, mx = 0; uint64_t sum = 0;
  void add(uint32_t v) { n++; sum += v; if (v < mn) mn = v; if (v > mx) mx = v; }
  uint32_t avg() const { return n ? (uint32_t)(sum / n) : 0; }
};

static bool txPrep(uint16_t len) {
  radio.standby();
  if (!packetParams(len)) return false;
  if (!ck(radio.setDioIrqConfig(radio.irqDioNum, RADIOLIB_LR2021_IRQ_TX_DONE), "dio")) return false;
  cmd(RADIOLIB_LR2021_CMD_CLEAR_TX_FIFO);
  getAndClearIrq();
  return true;
}

// Returns airtime in cycles (end of SetTx -> TX_DONE), 0 on timeout.
static uint32_t waitTxDone(uint32_t tStart, uint32_t limitCyc) {
  uint32_t tBeat = tStart;
  while (digitalRead(P_LORA_DIO_1) == LOW) {
    if (cyc() - tStart > limitCyc) return 0;
    // USB serial output gets lost after a few seconds of silence, so keep talking
    if (cyc() - tBeat > 64000000) { tBeat = cyc(); Serial.println("H,toa"); }
  }
  return cyc() - tStart;
}

static void cmdToa(int reps, int argc, char** argv) {
  static uint8_t pkt[512];
  Serial.println("#,len,reps,min_us,avg_us,max_us,timeouts");
  for (int a = 0; a < argc; a++) {
    uint16_t len = atoi(argv[a]);
    if (len < HDR || len > (modem == M_LORA ? 255 : 511)) { Serial.printf("# skip len %u\n", len); continue; }
    if (!txPrep(len)) return;
    Stat s; uint32_t to = 0;
    for (int r = 0; r < reps; r++) {
      buildPacket(pkt, len, r);
      writeTxFifo(pkt, len);
      setTx();
      uint32_t t0 = cyc();
      uint32_t d = waitTxDone(t0, 64000000UL * 30);
      getAndClearIrq();
      if (d) s.add(d / 64); else { to++; radio.standby(); txPrep(len); }
      delay(2);
    }
    Serial.printf("A,%u,%d,%lu,%lu,%lu,%lu\n", len, reps, (unsigned long)s.mn, (unsigned long)s.avg(), (unsigned long)s.mx,
                  (unsigned long)to);
  }
  radio.standby();
}

static void cmdTx(uint32_t count, uint16_t len, bool pipe, uint32_t gapUs) {
  static uint8_t pkt[512];
  if (len < HDR || len > (modem == M_LORA ? 255 : 511)) { Serial.println("# ERR len"); return; }
  if (pipe && len > 500) { Serial.println("# ERR pipe needs 2 packets in the 1 KB FIFO"); return; }
  if (!txPrep(len)) return;
  Stat load, air, gap;  // FIFO write, on-air (SetTx -> TX_DONE), end-to-start gap
  uint32_t ok = 0, to = 0;
  uint64_t tFirst = 0, tLast = 0;
  uint32_t tPrevDone = 0;
  uint32_t tBeat = millis();
  if (pipe) { buildPacket(pkt, len, 0); writeTxFifo(pkt, len); }
  for (uint32_t seq = 0; seq < count; seq++) {
    uint32_t t0 = cyc();
    if (!pipe) { buildPacket(pkt, len, seq); writeTxFifo(pkt, len); }
    uint32_t t1 = cyc();
    setTx();
    uint32_t t2 = cyc();
    if (seq == 0) tFirst = us64();
    else gap.add((t2 - tPrevDone) / 64);
    if (pipe && seq + 1 < count) {
      buildPacket(pkt, len, seq + 1);
      writeTxFifo(pkt, len);
      load.add((cyc() - t2) / 64);
    } else if (!pipe) {
      load.add((t1 - t0) / 64);
    }
    uint32_t d = waitTxDone(t2, 64000000);
    tPrevDone = cyc();
    getAndClearIrq();
    if (d) { ok++; air.add(d / 64); } else { to++; radio.standby(); txPrep(len); }
    tLast = us64();
    if (gapUs) { uint32_t g0 = cyc(); while (cyc() - g0 < gapUs * 64) {} }
    if (millis() - tBeat >= 1000) {
      tBeat = millis();
      Serial.printf("H,%lu,%lu\n", (unsigned long)tBeat, (unsigned long)seq);
      if (stopRequested()) break;
    }
  }
  radio.standby();
  double el = (double)(tLast - tFirst) / 1e6;
  // T,count_ok,timeouts,len,elapsed_s,offered_kbps,air_avg_us,air_min,air_max,load_avg_us,gap_avg_us,gap_max_us
  Serial.println("#,ok,timeouts,len,elapsed_s,offered_kbps,air_avg_us,air_min_us,air_max_us,fifo_write_avg_us,turnaround_avg_us,turnaround_max_us,busy_timeouts");
  Serial.printf("T,%lu,%lu,%u,%.3f,%.2f,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", (unsigned long)ok, (unsigned long)to, len, el,
                el > 0 ? ok * len * 8.0 / el / 1000.0 : 0.0, (unsigned long)air.avg(), (unsigned long)air.mn,
                (unsigned long)air.mx, (unsigned long)load.avg(), (unsigned long)gap.avg(), (unsigned long)gap.mx,
                (unsigned long)busyTimeouts);
}

// ---------------------------------------------------------------- RX

static void cmdRx() {
  static uint8_t pkt[512];
  radio.standby();
  if (!packetParams(modem == M_LORA ? (lc.implicit ? lc.len : 255) : 511)) return;
  uint32_t irqMask = RADIOLIB_LR2021_IRQ_RX_DONE | RADIOLIB_LR2021_IRQ_CRC_ERROR | RADIOLIB_LR2021_IRQ_LEN_ERROR |
                     RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR;
  if (!ck(radio.setDioIrqConfig(radio.irqDioNum, irqMask), "dio")) return;
  cmd(RADIOLIB_LR2021_CMD_CLEAR_RX_FIFO);
  getAndClearIrq();
  setRxCont();

  uint32_t good = 0, corrupt = 0, foreign = 0, crcIrq = 0, lenIrq = 0, hdrIrq = 0, resync = 0, dup = 0;
  uint32_t winGood = 0, maxLevel = 0;
  uint64_t goodBytes = 0, winBytes = 0;
  int64_t firstSeq = -1; uint32_t lastSeq = 0, seqSpanMax = 0;
  uint64_t tFirst = 0, tLast = 0, tWin = us64();
  float rssiSum = 0, snrSum = 0; uint32_t rssiN = 0;
  uint32_t tBeat = millis();
  uint16_t pktLen = 0;

  while (true) {
    if (digitalRead(P_LORA_DIO_1) == HIGH) {
      uint32_t irq = getAndClearIrq();
      if (irq & RADIOLIB_LR2021_IRQ_CRC_ERROR) crcIrq++;
      if (irq & RADIOLIB_LR2021_IRQ_LEN_ERROR) lenIrq++;
      if (irq & RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR) hdrIrq++;
      if (irq & (RADIOLIB_LR2021_IRQ_RX_DONE | RADIOLIB_LR2021_IRQ_CRC_ERROR)) {
        uint16_t l = rxPktLen();
        if (l >= HDR && l <= 511) pktLen = l;
        uint16_t level = rxFifoLevel();
        if (level > maxLevel) maxLevel = level;
        // every packet in a run has the same length; read all complete ones
        while (pktLen && level >= pktLen) {
          readRxFifo(pkt, pktLen);
          level -= pktLen;
          uint32_t seq = 0;
          int r = checkPacket(pkt, pktLen, &seq);
          if (r == 2) { foreign++; resync++; cmd(RADIOLIB_LR2021_CMD_CLEAR_RX_FIFO); break; }
          if (r == 1) { corrupt++; continue; }
          uint64_t t = us64();
          if (firstSeq < 0) { firstSeq = seq; tFirst = t; }
          else if (seq <= lastSeq) dup++;
          lastSeq = seq; tLast = t;
          good++; winGood++; goodBytes += pktLen; winBytes += pktLen;
          if ((good & 31) == 1) {
            float rssi = 0, snr = 0;
            if (modem == M_LORA) {
              uint8_t s[9] = { 0 };
              if (rd(RADIOLIB_LR2021_CMD_GET_LORA_PACKET_STATUS, s, 9) ) {
                snr = (float)((int8_t)s[2]) / 4.0f;
                rssi = (float)(((uint16_t)s[3] << 1) | ((s[5] & 0x02) >> 1)) / -2.0f;
              }
            } else {
              uint8_t s[5] = { 0 };
              if (rd(RADIOLIB_LR2021_CMD_GET_FLRC_PACKET_STATUS, s, 5))
                rssi = (float)(((uint16_t)s[3] << 1) | (s[4] & 0x01)) / -2.0f;  // RSSI at sync
            }
            rssiSum += rssi; snrSum += snr; rssiN++;
          }
        }
      }
    }
    if (millis() - tBeat >= 1000) {
      tBeat = millis();
      uint64_t t = us64();
      double w = (double)(t - tWin) / 1e6;
      // S,t_ms,good,corrupt,crc_irq,len_irq,hdr_irq,resync,lost,window_pkts,window_kbps,max_fifo
      uint32_t span = firstSeq < 0 ? 0 : lastSeq - (uint32_t)firstSeq + 1;
      Serial.printf("S,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%ld,%lu,%.2f,%lu\n", (unsigned long)tBeat, (unsigned long)good,
                    (unsigned long)corrupt, (unsigned long)crcIrq, (unsigned long)lenIrq, (unsigned long)hdrIrq,
                    (unsigned long)resync, (long)span - (long)good, (unsigned long)winGood,
                    w > 0 ? winBytes * 8.0 / w / 1000.0 : 0.0, (unsigned long)maxLevel);
      winGood = 0; winBytes = 0; tWin = t;
      if (stopRequested()) break;
    }
  }
  radio.standby();
  uint32_t span = firstSeq < 0 ? 0 : lastSeq - (uint32_t)firstSeq + 1;
  double el = (double)(tLast - tFirst) / 1e6;
  Serial.println("#,good,corrupt,crc_irq,len_irq,hdr_irq,resync,dups,seq_span,lost,len,elapsed_s,goodput_kbps,rssi_avg,snr_avg,max_fifo");
  Serial.printf("SUM,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%ld,%u,%.3f,%.2f,%.1f,%.2f,%lu\n", (unsigned long)good,
                (unsigned long)corrupt, (unsigned long)crcIrq, (unsigned long)lenIrq, (unsigned long)hdrIrq,
                (unsigned long)resync, (unsigned long)dup, (unsigned long)span, (long)span - (long)good, pktLen, el,
                (el > 0 && good > 1) ? (good - 1) * (double)pktLen * 8.0 / el / 1000.0 : 0.0,
                rssiN ? rssiSum / rssiN : 0.0f, rssiN ? snrSum / rssiN : 0.0f, (unsigned long)maxLevel);
  (void)seqSpanMax;
}


// ---------------------------------------------------------------- raw IQ TX / capture

static bool rd2(uint16_t op, const uint8_t* p, size_t np, uint8_t* data, size_t n) {
  static uint8_t z[160], r[160];
  uint8_t c[8] = { (uint8_t)(op >> 8), (uint8_t)op };
  if (np > 6 || n + 2 > sizeof(r)) return false;
  if (np) memcpy(c + 2, p, np);
  memset(z, 0, n + 2);
  if (!xfer(c, nullptr, np + 2) || !xfer(z, r, n + 2)) return false;
  memcpy(data, r + 2, n);
  return true;
}
static uint32_t rd32(uint32_t addr) {
  uint8_t a[4] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 1 }, d[4] = { 0 };
  rd2(0x0106, a, 4, d, 4);
  return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}
static uint16_t rd16(uint16_t op) { uint8_t b[2] = { 0 }; rd(op, b, 2); return ((uint16_t)b[0] << 8) | b[1]; }

static int8_t wave[2 * 32768];

static uint8_t ft8Tones[79];
static uint8_t ft8N = 0;

static uint8_t afskBits[256];  // tone sequence after NRZI, bit i = byte i/8, LSB first; 1 = mark (1200 Hz)
static uint16_t afskN = 0;

static void makeWave(uint32_t pairs, char kind, float kHz, int amp, uint32_t rate) {
  if (kind == 'a') {
    // AFSK 1200 (Bell 202) on FM: audio tone 1200 Hz (mark) / 2200 Hz (space), phase-continuous,
    // frequency-modulates the carrier with peak deviation kHz. rate must be a multiple of 1200.
    const uint32_t spb = rate / 1200;
    double aph = 0, ph = 0;
    for (uint32_t n = 0; n < pairs; n++) {
      uint32_t b = n / spb;
      if (b >= afskN) { wave[2 * n] = wave[2 * n + 1] = 0; continue; }
      double tone = (afskBits[b >> 3] >> (b & 7)) & 1 ? 1200.0 : 2200.0;
      aph += 2 * M_PI * tone / rate;
      if (aph > M_PI) aph -= 2 * M_PI;
      ph += 2 * M_PI * kHz * 1000.0 * sin(aph) / rate;
      if (ph > M_PI) ph -= 2 * M_PI;
      if (ph < -M_PI) ph += 2 * M_PI;
      wave[2 * n] = (int8_t)lrint(amp * cos(ph));
      wave[2 * n + 1] = (int8_t)lrint(amp * sin(ph));
    }
    return;
  }
  if (kind == 'f') {
    // FT8: 79 symbols of 0.16 s, tone k at base + k * 6.25 Hz, phase-continuous 8-FSK.
    // kHz is the base offset from the carrier. Samples after the last symbol are zero.
    const uint32_t sym = rate * 16 / 100;
    double ph = 0;
    for (uint32_t n = 0; n < pairs; n++) {
      uint32_t k = n / sym;
      if (k >= ft8N) { wave[2 * n] = wave[2 * n + 1] = 0; continue; }
      double f = kHz * 1000.0 + 6.25 * ft8Tones[k];
      ph += 2 * M_PI * f / rate;
      if (ph > M_PI) ph -= 2 * M_PI;
      wave[2 * n] = (int8_t)lrint(amp * cos(ph));
      wave[2 * n + 1] = (int8_t)lrint(amp * sin(ph));
    }
    return;
  }
  // t = tone at +kHz, c = chirp kHz wide (period 2048 samples),
  // n = white: a random phase every sample (constant envelope), spectrum flat across +-rate/2
  float ph = 0;
  uint32_t lcg = 12345;
  const uint32_t P = 2048;
  for (uint32_t n = 0; n < pairs; n++) {
    if (kind == 'm') {
      // multi-tone: +25, -60, +110, -150, +190 kHz, equal amplitude, fixed phases
      static const float tf[5] = { 25e3f, -60e3f, 110e3f, -150e3f, 190e3f };
      static const float tp[5] = { 0.0f, 1.3f, 2.9f, 4.1f, 5.4f };
      float re = 0, im = 0;
      for (int k = 0; k < 5; k++) {
        float a = fmodf(2.0f * (float)M_PI * tf[k] * (float)n / rate, 2.0f * (float)M_PI) + tp[k];
        re += cosf(a); im += sinf(a);
      }
      wave[2 * n] = (int8_t)lrintf(amp * re / 5.0f);
      wave[2 * n + 1] = (int8_t)lrintf(amp * im / 5.0f);
      continue;
    }
    if (kind == 'n') {
      lcg = lcg * 1664525u + 1013904223u;
      ph = (float)(lcg >> 8) * (2.0f * (float)M_PI / 16777216.0f);
    } else {
      float f = (kind == 'c') ? kHz * 1000.0f * ((float)(n % P) / P - 0.5f) : kHz * 1000.0f;
      ph += 2.0f * (float)M_PI * f / rate;
      if (ph > (float)M_PI) ph -= 2.0f * (float)M_PI;
      if (ph < -(float)M_PI) ph += 2.0f * (float)M_PI;
    }
    wave[2 * n] = (int8_t)lrintf(amp * cosf(ph));
    wave[2 * n + 1] = (int8_t)lrintf(amp * sinf(ph));
  }
}

static void cmdIqTx(uint32_t rate, uint32_t pairs, char kind, float kHz, int amp, int bursts) {
  if (pairs < 64 || pairs > 32767 || amp > 56) { Serial.println("# ERR pairs 64..32767, amp <= 56"); return; }
  makeWave(pairs, kind, kHz, amp, rate);
  const uint32_t total = 2 * pairs;
  Serial.printf("# iqtx rate %lu pairs %lu (%lu us) %c %.1f kHz amp %d bursts %d\n", (unsigned long)rate,
                (unsigned long)pairs, (unsigned long)((uint64_t)pairs * 1000000 / rate), kind, kHz, amp, bursts);
  Serial.println("#,burst,tx_done,timeout,us_settx_to_done,expected_us,min_fifo,refills,underflow_flags,max_gap_us,queued");
  for (int b = 0; b < bursts; b++) {
    radio.standby();
    uint8_t type = 0x0A;  // OOK: passes amplitude as well as phase (lr2021-sdr)
    uint8_t mod[7] = { (uint8_t)(rate >> 24), (uint8_t)(rate >> 16), (uint8_t)(rate >> 8), (uint8_t)rate, 0, 226, 0 };
    uint8_t pkt[6] = { 0, 32, 0, (uint8_t)(total >> 8), (uint8_t)total, 0 };
    uint8_t sync[5] = { 0 };
    uint8_t fb = 1;
    uint8_t c293[7] = { (uint8_t)(pairs >> 8), (uint8_t)pairs, (uint8_t)(rate >> 24), (uint8_t)(rate >> 16),
                        (uint8_t)(rate >> 8), (uint8_t)rate, 0 };
    cmd(0x0207, &type, 1);
    if (b == 0) { applyRf(); }  // frequency + calibration + PA/power, once
    cmd(0x0281, mod, 7);
    cmd(0x0282, pkt, 6);
    cmd(0x0284, sync, 5);
    cmd(0x0206, &fb, 1);
    writeReg(0xF3002C, 0x804000);
    writeReg(0xF30034, 0x3FC);
    cmd(0x0293, c293, 7);
    cmd(0x011F);
    getAndClearIrq();
    uint8_t ff[2] = { 0x3F, 0x3F };
    cmd(0x0114, ff, 2);
    uint32_t queued = 0;
    auto enqueue = [&](uint32_t limit) {
      if (limit > total - queued) limit = total - queued;
      while (limit) {
        uint32_t n = limit > 1020 ? 1020 : limit;
        writeTxFifo((const uint8_t*)wave + queued, n);
        queued += n; limit -= n;
      }
    };
    enqueue(896);
    uint32_t expUs = (uint32_t)((uint64_t)pairs * 1000000 / rate);
    // SetTx timeout ticks are 32.768 kHz (lr2021-sdr's /1000*32 is 2.3% short, which cuts long bursts)
    uint32_t ticks = (uint32_t)(((uint64_t)(expUs + 50000) * 32768 + 999999) / 1000000);
    uint8_t to[3] = { (uint8_t)(ticks >> 16), (uint8_t)(ticks >> 8), (uint8_t)ticks };
    cmd(0x020D, to, 3);
    uint32_t t0 = cyc(), tLast = t0, maxGap = 0, refills = 0, minFifo = 1024;
    uint8_t flagsAcc = 0;
    uint32_t irq = 0;
    while (true) {
      uint32_t now = cyc();
      if ((now - tLast) / 64 > maxGap) maxGap = (now - tLast) / 64;
      tLast = now;
      if (queued < total) {
        uint16_t fill = rd16(0x011D);
        if (fill < minFifo) minFifo = fill;
        if (fill <= 768) { enqueue((896 - fill) & ~1u); refills++; }
      }
      uint8_t fl[2] = { 0 };
      rd(0x011B, fl, 2);
      flagsAcc |= fl[1];
      uint8_t ib[4] = { 0 };
      rd(0x0117, ib, 4);
      irq = ((uint32_t)ib[0] << 24) | ((uint32_t)ib[1] << 16) | ((uint32_t)ib[2] << 8) | ib[3];
      if (irq & ((1UL << 19) | (1UL << 21))) break;
      if ((cyc() - t0) / 64 > expUs + 200000) break;
    }
    uint32_t el = (cyc() - t0) / 64;
    Serial.printf("Q,%d,%d,%d,%lu,%lu,%lu,%lu,0x%02X,%lu,%lu\n", b, !!(irq & (1UL << 19)), !!(irq & (1UL << 21)),
                  (unsigned long)el, (unsigned long)expUs, (unsigned long)minFifo, (unsigned long)refills, flagsAcc,
                  (unsigned long)maxGap, (unsigned long)queued);
    if (stopRequested()) break;
  }
  // lr2021-sdr resets the chip after a raw burst; do the same, then restore our config
  radio.standby();
  radio_init();
  radio.gainModeLf = 4; radio.gainModeHf = 4;
  static_cast<HalAccess*>(static_cast<ArduinoHal*>(radio.getMod()->hal))->spiSettings = fastSpi;
  applyAll();
}

static void cmdCap(uint8_t bwCode, uint32_t ms) {
  const uint32_t words = 2048;
  radio.standby();
  radio.setPacketType(RADIOLIB_LR2021_PACKET_TYPE_LORA);
  radio.setLoRaModulationParams(7, bwCode, 1, 0);
  for (uint32_t i = 0; i < words; i++) writeReg(0x804000 + 4 * i, 0xA5A5A5A5);
  uint8_t stby = 0;
  cmd(0x0128, &stby, 1);
  uint8_t t = 0x0B;
  cmd(0x0207, &t, 1);
  uint8_t c[10] = { 0xFF, 0, 0, 0, 0, 0xD0, 0, 0, (uint8_t)(words >> 8), (uint8_t)words };
  cmd(0x0290, c, 10);
  setRxCont();
  uint32_t t0 = millis(), tc = 0, gainMin = 15, gainMax = 0;
  uint16_t last = 0xFFFF, ptr = 0;
  while (millis() - t0 < ms) {
    ptr = rd16(0x0291);
    uint32_t g = (rd32(0xF30154) >> 20) & 0xF;
    if (g < gainMin) gainMin = g;
    if (g > gainMax) gainMax = g;
    if (ptr != last) { last = ptr; tc = millis(); }
    if (tc && millis() - tc > 50) break;
  }
  uint8_t stop[6] = { 0xFF, 0, 0, 0, 0, 0 };
  cmd(0x0290, stop, 6);
  cmd(0x0128, &stby, 1);
  uint32_t fresh = 0;
  static uint8_t buf[128];
  Serial.printf("# cap bw code 0x%X ptr 0x%04X agc gain %lu..%lu\n", bwCode, ptr, (unsigned long)gainMin, (unsigned long)gainMax);
  for (uint32_t i = 0; i < words; i += 32) {
    uint32_t a = 0x804000 + 4 * i;
    uint8_t p[4] = { (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 32 };
    rd2(0x0106, p, 4, buf, 128);
    Serial.print("C,");
    for (int j = 0; j < 128; j++) Serial.printf("%02X", buf[j]);
    Serial.println();
    for (int j = 0; j < 128; j += 4) fresh += !(buf[j] == 0xA5 && buf[j + 1] == 0xA5 && buf[j + 2] == 0xA5 && buf[j + 3] == 0xA5);
  }
  Serial.printf("# cap fresh %lu/%lu words\n", (unsigned long)fresh, (unsigned long)words);
  applyAll();
}



// ---------------------------------------------------------------- AFSK via the GFSK modem (no raw IQ)

// fskafsk <bps> <dev Hz> <shape> <msb 0|1>: send the loaded AFSK tone bits through the documented
// GFSK packet modem. Each modem bit sets the carrier to +dev or -dev; the bit pattern is a square
// wave at the AFSK tone (bit = sin(audio phase) >= 0), so an FM receiver hears 1200/2200 Hz.
// bps should be a multiple of 1200 (26400: 22 modem bits per AFSK bit, 1200 Hz = 11+11, 2200 Hz = 6+6).
// No preamble, sync word, CRC or whitening; the payload is streamed through the 1 KB TX FIFO.
static void cmdFskAfsk(uint32_t bps, uint32_t devHz, uint8_t shape, bool msb) {
  if (!afskN) { Serial.println("# ERR load bits with afsk first"); return; }
  const uint32_t per = bps / 1200;
  const uint32_t nbits = (uint32_t)afskN * per;
  const uint32_t nbytes = (nbits + 7) / 8;
  if (nbytes > sizeof(wave) || nbytes > 65535) { Serial.println("# ERR too long"); return; }
  uint8_t* pl = (uint8_t*)wave;
  memset(pl, 0, nbytes);
  double aph = 0;
  for (uint32_t i = 0; i < nbits; i++) {
    uint32_t b = i / per;
    double tone = (afskBits[b >> 3] >> (b & 7)) & 1 ? 1200.0 : 2200.0;
    aph += 2 * M_PI * tone / bps;
    if (aph > M_PI) aph -= 2 * M_PI;
    if (sin(aph) >= 0) pl[i >> 3] |= msb ? (0x80 >> (i & 7)) : (1 << (i & 7));
  }
  radio.standby();
  int16_t st = radio.setPacketType(RADIOLIB_LR2021_PACKET_TYPE_GFSK);
  if (!st) st = applyRf() ? 0 : -1;
  if (!st) st = radio.setGfskModulationParams(bps, shape, RADIOLIB_LR2021_GFSK_OOK_RX_BW_59_5, devHz);
  if (!st) st = radio.setGfskSyncword(nullptr, 0, true);
  if (!st) st = radio.setGfskPacketParams(0, 0, false, false, 0, RADIOLIB_LR2021_GFSK_OOK_PACKET_FORMAT_FIXED, nbytes, 0, 0);
  uint16_t err = 0; radio.getChipErrors(&err);
  Serial.printf("# fskafsk %lu bps dev %lu Hz shape %u msb %d: %lu modem bits, %lu bytes, setup st=%d chip errors 0x%04X\n",
                (unsigned long)bps, (unsigned long)devHz, shape, msb, (unsigned long)nbits, (unsigned long)nbytes, st, err);
  if (st) { applyAll(); return; }
  writeReg(0xF3002C, 0x804000);
  writeReg(0xF30034, 0x3FC);
  radio.setDioIrqConfig(radio.irqDioNum, RADIOLIB_LR2021_IRQ_TX_DONE);
  cmd(RADIOLIB_LR2021_CMD_CLEAR_TX_FIFO);
  getAndClearIrq();
  uint32_t queued = 0;
  auto enqueue = [&](uint32_t limit) {
    if (limit > nbytes - queued) limit = nbytes - queued;
    while (limit) { uint32_t n = limit > 1020 ? 1020 : limit; writeTxFifo(pl + queued, n); queued += n; limit -= n; }
  };
  enqueue(896);
  uint32_t expUs = (uint32_t)((uint64_t)nbits * 1000000 / bps);
  uint32_t ticks = (uint32_t)(((uint64_t)(expUs + 50000) * 32768 + 999999) / 1000000);
  uint8_t to[3] = { (uint8_t)(ticks >> 16), (uint8_t)(ticks >> 8), (uint8_t)ticks };
  cmd(RADIOLIB_LR2021_CMD_SET_TX, to, 3);
  uint32_t t0 = millis(), minFifo = 1024, irq = 0;
  while (millis() - t0 < expUs / 1000 + 500) {
    if (queued < nbytes) {
      uint16_t fill = rd16(RADIOLIB_LR2021_CMD_GET_TX_FIFO_LEVEL);
      if (fill < minFifo) minFifo = fill;
      if (fill <= 768) enqueue(896 - fill);
    }
    uint8_t ib[4] = { 0 };
    rd(0x0117, ib, 4);
    irq |= ((uint32_t)ib[0] << 24) | ((uint32_t)ib[1] << 16) | ((uint32_t)ib[2] << 8) | ib[3];
    if (irq & ((1UL << 19) | (1UL << 21))) break;
  }
  Serial.printf("Q,fsk,%d,%d,%lu,%lu,%lu,%lu\n", !!(irq & (1UL << 19)), !!(irq & (1UL << 21)), (unsigned long)(millis() - t0),
                (unsigned long)(expUs / 1000), (unsigned long)minFifo, (unsigned long)queued);
  radio.standby();
  applyAll();
}

// ---------------------------------------------------------------- continuous raw IQ stream

// The streamer (shared with the console, include/LR2021IQStream.h) expects these names in its
// namespace. It reuses the TX waveform buffer as its ring.
namespace lr2021dbg {
static bool hold = false;
static const uint32_t RING = sizeof(wave);
static uint8_t* const ring = (uint8_t*)wave;
static uint32_t ringLen = 0;
static SPISettings fastSpi(16000000, MSBFIRST, SPI_MODE0);
#include "LR2021IQStream.h"
}

// ---------------------------------------------------------------- synthesizer range (RX only, no TX)

// pll <MHz> [MHz ...]: tune the receiver (no transmit), calibrate, enter RX, then report the chip's
// error flags (bit 2 PLL_LOCK_ERR, bit 5 PLL_CALIB_ERR, bit 9 RXFREQ_NO_FE_CAL_ERR) and RSSI.
static void cmdPll(int argc, char** argv) {
  Serial.println("#,MHz,st_freq,st_cal,st_rx,errors,rssi_dbm");
  for (int a = 0; a < argc; a++) {
    float f = atof(argv[a]);
    radio.standby(RADIOLIB_LR2021_STANDBY_XOSC);
    radio.clearChipErrors();
    int16_t s1 = radio.setRfFrequency((uint32_t)(f * 1e6f + 0.5f));
    uint16_t cal[3] = { (uint16_t)((uint16_t)(f / 4.0f + 0.5f) | RADIOLIB_LR2021_CALIBRATE_FE_LF_PATH), 0, 0 };
    int16_t s2 = radio.calibrateFrontEnd(cal);
    radio.setRxPath(RADIOLIB_LR2021_RX_PATH_LF, radio.gainModeLf);
    radio.setDioIrqConfig(radio.irqDioNum, 0);
    int16_t s3 = radio.setRx(0xFFFFFF);
    delay(20);
    float sum = 0; int n = 0;
    for (int i = 0; i < 20; i++) { float r = 0; if (radio.getRssiInst(&r) == RADIOLIB_ERR_NONE) { sum += r; n++; } delay(2); }
    radio.standby();
    uint16_t err = 0;
    radio.getChipErrors(&err);
    Serial.printf("P,%.3f,%d,%d,%d,0x%04X,%.1f\n", f, s1, s2, s3, err, n ? sum / n : 0.0f);
  }
  radio.clearChipErrors();
  applyAll();
}

// ---------------------------------------------------------------- noise floor

// noise <ms per step> <f1 MHz> <f2 MHz> <step MHz>: RSSI_inst in continuous RX with the current
// modem's channel filter. N,freq,samples,min,avg(power mean),p90,max
static void cmdNoise(uint32_t ms, float f1, float f2, float step) {
  static float v[2000];
  float saved = freqMHz;
  Serial.println("#,freq,n,min_dbm,mean_dbm,p90_dbm,max_dbm");
  for (float f = f1; f <= f2 + 0.001f; f += step) {
    freqMHz = f;
    applyRf();
    radio.setDioIrqConfig(radio.irqDioNum, 0);
    setRxCont();
    delay(5);
    uint32_t n = 0, t0 = millis();
    while (millis() - t0 < ms && n < 2000) {
      float r = 0;
      if (radio.getRssiInst(&r) == RADIOLIB_ERR_NONE) v[n++] = r;
      delayMicroseconds(200);
    }
    radio.standby();
    if (!n) continue;
    std::sort(v, v + n);
    double p = 0;
    for (uint32_t i = 0; i < n; i++) p += pow(10.0, v[i] / 10.0);
    Serial.printf("N,%.3f,%lu,%.1f,%.1f,%.1f,%.1f\n", f, (unsigned long)n, v[0], 10.0 * log10(p / n), v[n * 9 / 10], v[n - 1]);
    if (stopRequested()) break;
  }
  freqMHz = saved;
  applyAll();
}

// ---------------------------------------------------------------- command line

static void handleLine(char* line) {
  char* argv[16];
  int argc = 0;
  for (char* t = strtok(line, " \t"); t && argc < 16; t = strtok(NULL, " \t")) argv[argc++] = t;
  if (!argc) return;
  const char* c = argv[0];
  if (!strcmp(c, "help")) {
    Serial.println("# spi <Hz> | freq <MHz> | pwr <dBm> | lora <bw> <sf> <cr> <pre> <impl> <crc> <len> [ldro] [mode]");
    Serial.println("# flrc <kbps> <cr> <pre> <crc> <shape> | toa <reps> <len..> | tx <count> <len> <pipe> <gapUs> | rx | x");
    printConfig();
  } else if (!strcmp(c, "spi") && argc >= 2) {
    spiHz = atol(argv[1]);
    fastSpi = SPISettings(spiHz, MSBFIRST, SPI_MODE0);
    static_cast<HalAccess*>(static_cast<ArduinoHal*>(radio.getMod()->hal))->spiSettings = fastSpi;
    printConfig();
  } else if (!strcmp(c, "freq") && argc >= 2) {
    freqMHz = strtod(argv[1], NULL); applyAll(); printConfig();
  } else if (!strcmp(c, "pwr") && argc >= 2) {
    txPower = atoi(argv[1]); applyAll(); printConfig();
  } else if (!strcmp(c, "lora") && argc >= 8) {
    modem = M_LORA;
    // "c<hex>" sends a raw bandwidth code (e.g. c8 for the RFU code 0x8)
    lc.bwRaw = (argv[1][0] == 'c') ? (int16_t)strtol(argv[1] + 1, NULL, 16) : -1;
    lc.bw = lc.bwRaw >= 0 ? 1000 : atof(argv[1]); lc.sf = atoi(argv[2]); lc.cr = atoi(argv[3]); lc.pre = atoi(argv[4]);
    lc.implicit = atoi(argv[5]); lc.crc = atoi(argv[6]); lc.len = atoi(argv[7]);
    lc.ldro = argc >= 9 ? atoi(argv[8]) : -1;   // optional raw ldro (0..3), -1 = auto
    lc.mode = argc >= 10 ? atoi(argv[9]) : 0;   // optional hidden mode field (0..3)
    applyAll(); printConfig();
    uint16_t err = 0; radio.getChipErrors(&err); Serial.printf("# bw code 0x%X, chip errors 0x%04X\n", lc.bwRaw, err); radio.clearChipErrors();
  } else if (!strcmp(c, "flrc") && argc >= 6) {
    modem = M_FLRC;
    fc.kbps = atoi(argv[1]); fc.cr = atoi(argv[2]); fc.pre = atoi(argv[3]); fc.crc = atoi(argv[4]); fc.shape = atoi(argv[5]);
    applyAll(); printConfig();
  } else if (!strcmp(c, "rc") && argc >= 2) {
    // rc <op hex> [byte hex ...]: raw write command; the next command's MISO carries its status
    uint8_t p[14]; size_t n = 0;
    for (int i = 2; i < argc && n < sizeof(p); i++) p[n++] = (uint8_t)strtol(argv[i], NULL, 16);
    cmd((uint16_t)strtol(argv[1], NULL, 16), p, n);
    uint8_t m[4] = { 0, 0, 0, 0 };
    cmd(RADIOLIB_LR2021_CMD_CLEAR_IRQ, m, 4);
    uint16_t st = lastStat;  // CommandStatus is bits 11:9 (0 FAIL, 1 PERR, 2 OK, 3 DAT)
    Serial.printf("# rc stat 0x%04X (%s)\n", st, ((st >> 9) & 7) == 2 ? "OK" : ((st >> 9) & 7) == 1 ? "PERR" : ((st >> 9) & 7) == 0 ? "FAIL" : "DAT");
  } else if (!strcmp(c, "rr") && argc >= 3) {
    // rr <op hex> <n> [byte hex ...]: raw read command, prints n response bytes
    uint8_t tx[16] = { 0 }, z[16] = { 0 }, r[16] = { 0 };
    uint16_t op = (uint16_t)strtol(argv[1], NULL, 16);
    size_t n = atoi(argv[2]), np = 0;
    tx[0] = op >> 8; tx[1] = op;
    for (int i = 3; i < argc && np < 14; i++) tx[2 + np++] = (uint8_t)strtol(argv[i], NULL, 16);
    if (n > 14) n = 14;
    xfer(tx, nullptr, np + 2);
    xfer(z, r, n + 2);
    Serial.printf("# rr stat 0x%02X%02X data", r[0], r[1]);
    for (size_t i = 0; i < n; i++) Serial.printf(" %02X", r[2 + i]);
    Serial.println();
  } else if (!strcmp(c, "ptx") && argc >= 3) {
    // ptx <len> <timeout ms>: send one packet with whatever modem setup the chip has now (no
    // modem/packet params are touched), report airtime, IRQ bits and chip errors
    static uint8_t pkt[512];
    uint16_t len = atoi(argv[1]);
    if (len > 511) len = 511;
    for (uint16_t i = 0; i < len; i++) pkt[i] = (uint8_t)(i * 7 + 0x31);
    usInit();
    radio.setDioIrqConfig(radio.irqDioNum, RADIOLIB_LR2021_IRQ_TX_DONE | RADIOLIB_LR2021_IRQ_TIMEOUT);
    cmd(RADIOLIB_LR2021_CMD_CLEAR_TX_FIFO);
    getAndClearIrq();
    writeTxFifo(pkt, len);
    setTx();
    uint8_t m[4] = { 0, 0, 0, 0 };
    cmd(RADIOLIB_LR2021_CMD_CLEAR_IRQ, m, 4);
    uint16_t st = lastStat;
    uint32_t t0 = cyc();
    uint32_t d = waitTxDone(t0, 64000UL * atol(argv[2]));
    uint32_t irq = getAndClearIrq();
    radio.standby();
    uint16_t err = 0; radio.getChipErrors(&err); radio.clearChipErrors();
    usStop();
    Serial.printf("P,%u,%lu,0x%08lX,settx_stat 0x%04X,errors 0x%04X\n", len, d ? (unsigned long)(d / 64) : 0UL, (unsigned long)irq, st, err);
  } else if (!strcmp(c, "toa") && argc >= 3) {
    usInit(); cmdToa(atoi(argv[1]), argc - 2, argv + 2); usStop();
  } else if (!strcmp(c, "tx") && argc >= 5) {
    usInit(); cmdTx(atol(argv[1]), atoi(argv[2]), atoi(argv[3]), atol(argv[4])); usStop();
  } else if (!strcmp(c, "rx")) {
    usInit(); cmdRx(); usStop();
  } else if (!strcmp(c, "iqtx") && argc >= 7) {
    usInit(); cmdIqTx(atol(argv[1]), atol(argv[2]), argv[3][0], atof(argv[4]), atoi(argv[5]), atoi(argv[6])); usStop();
  } else if (!strcmp(c, "cap") && argc >= 3) {
    uint8_t code = argv[1][0] == 'c' ? (uint8_t)strtol(argv[1] + 1, NULL, 16) : loraBwCode(atof(argv[1]));
    cmdCap(code, atol(argv[2]));
  } else if (!strcmp(c, "afsk") && argc >= 2) {
    // afsk clear | afsk <nbits> <hex>: append tone bits (LSB first per byte) for "iqtx <rate> <pairs> a <dev kHz> ..."
    if (!strcmp(argv[1], "clear")) afskN = 0;
    else if (argc >= 3) {
      uint16_t nb = atoi(argv[1]);
      const char* h = argv[2];
      for (uint16_t i = 0; i < nb && afskN < sizeof(afskBits) * 8; i++) {
        uint8_t byte = 0;
        char hx[3] = { h[(i / 8) * 2], h[(i / 8) * 2 + 1], 0 };
        byte = (uint8_t)strtol(hx, NULL, 16);
        uint8_t bit = (byte >> (i & 7)) & 1;
        if (bit) afskBits[afskN >> 3] |= 1 << (afskN & 7); else afskBits[afskN >> 3] &= ~(1 << (afskN & 7));
        afskN++;
      }
    }
    Serial.printf("# afsk %u bits loaded\n", afskN);
  } else if (!strcmp(c, "fskafsk") && argc >= 5) {
    cmdFskAfsk(atol(argv[1]), atol(argv[2]), atoi(argv[3]), atoi(argv[4]));
  } else if (!strcmp(c, "ft8") && argc >= 2) {
    ft8N = 0;
    for (const char* p = argv[1]; *p && ft8N < 79; p++) if (*p >= '0' && *p <= '7') ft8Tones[ft8N++] = *p - '0';
    Serial.printf("# ft8 %u symbols loaded\n", ft8N);
  } else if (!strcmp(c, "iqstream") && argc >= 2) {
    // iqstream <ms> [mhz] [dec] [bits] [rawrate]: LRIQ binary stream (docs/raw-iq.md)
    lr2021dbg::iqstream::run(radio, Serial, atol(argv[1]), argc > 2 ? atol(argv[2]) : 16, argc > 3 ? atoi(argv[3]) : 3,
                             argc > 4 ? atoi(argv[4]) : 8, argc > 5 ? atol(argv[5]) : 250000, 0);
    applyAll();
  } else if (!strcmp(c, "pll") && argc >= 2) {
    cmdPll(argc - 1, argv + 1);
  } else if (!strcmp(c, "noise") && argc >= 5) {
    cmdNoise(atol(argv[1]), atof(argv[2]), atof(argv[3]), atof(argv[4]));
  } else if (!strcmp(c, "x")) {
  } else {
    Serial.printf("# unknown or incomplete command: %s (try help)\n", c);
  }
  Serial.println("# ready");
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  board.begin();
  if (!radio_init()) {
    Serial.println("# radio init FAILED");
    while (true) delay(1000);
  }
  pinMode(P_LORA_DIO_1, INPUT);
  radio.gainModeLf = 4;
  radio.gainModeHf = 4;
  fastSpi = SPISettings(spiHz, MSBFIRST, SPI_MODE0);
  static_cast<HalAccess*>(static_cast<ArduinoHal*>(radio.getMod()->hal))->spiSettings = fastSpi;
  applyAll();
  Serial.printf("# rate_bench ready on %s\n", board.getManufacturerName());
  printConfig();
  Serial.println("# ready");
}

void loop() {
  static char line[160];
  static size_t n = 0;
  while (Serial.available()) {
    int ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { line[n] = 0; handleLine(line); n = 0; }
    else if (n < sizeof(line) - 1) line[n++] = (char)ch;
  }
}
