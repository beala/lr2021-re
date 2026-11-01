// LR2021 console: raw chip access and IQ capture (LR2021DebugCLI.h), plus a hardware LoRa
// receiver/transmitter so one board can act as the reference receiver or the test transmitter.
// Serial, 115200, one command per line (CR or LF).
//
// Debug commands (see LR2021DebugCLI.h): mr, mw, rc, err, romprobe, cap, capwait, stream,
// streamdump, streamstats, iqstream, hold on|off.
//
// Radio commands (replies use the "  -> " prefix the host tools parse):
//   get radio                       -> "  -> > <MHz>,<BW kHz>,<SF>,<CR>"
//   set radio <MHz>,<BW kHz>,<SF>,<CR>   re-initialise the chip with these LoRa settings
//   get tx / set tx <dBm>           TX power (LR2021 drive level, -9..22)
//   loratx <hex>                    send one LoRa packet (explicit header, CRC on, 16-symbol preamble)
//   rxstats                         hardware RX counters
//   spifree <hex>                   send a command frame, release NSS/SCK/MOSI until BUSY drops (PRAM drives them)
//   rxread                          read the packet in the RX FIFO (after a raw RX, e.g. tools/scan.py)
//   pinmon <ms> / pinsnap <n>       XIAO only: watch the pads wired to the MCU (docs/test-bus.md)
//   wfrun <nseg> [frames] [ov]      stream the on-chip spectrum (PRAM entry 9) as binary frames until x;
//                                   ov = 1: overlap the next capture with the FFT (nseg 1)
//   aprswait <ms>                   print the frames the chip's background APRS receiver (PRAM demod.bin)
//                                   decodes, waking on its RxDone IRQ
//   scanwait <ms>                   print the packets the chip's background channel scanner (PRAM
//                                   scan.bin entry 4) catches, waking on RxDone, and resume it
//   help
//
// Receiver: while not held the chip receives LoRa with the current settings and prints one line
// per packet. It boots held (so it never logs other people's traffic by itself); "hold off" or
// "set radio" starts it:
//   RXSTAT len=<n> st=<readData status> rssi=<dBm> snr=<dB> raw=<GetLoraPacketStatus, 10 bytes>
//          gain=<AGC step> crc=<1|0> data=<payload hex>
// Debug commands that reconfigure the chip leave it held; "hold off" re-initialises the radio
// before receiving again, so no reboot is needed after raw-mode experiments.
// Boot settings: 910.525 MHz, 62.5 kHz, SF7, CR 4/5, -9 dBm, receiver held, nothing sent.

#include <Arduino.h>
#include <LR2021Board.h>
#include "LR2021DebugCLI.h"

static struct { float mhz = DEFAULT_FREQ_MHZ, bw = DEFAULT_BW_KHZ; uint8_t sf = DEFAULT_SF, cr = DEFAULT_CR;
                int8_t dbm = DEFAULT_TX_DBM; } cfg;
static bool rxActive = false;
static bool dirty = false;  // a debug command may have changed the chip; re-init before RX
static uint32_t rxOk = 0, rxCrc = 0, rxHdr = 0, txCount = 0;

static void stopRx() {
  if (rxActive) radio.standby();
  rxActive = false;
}

static bool reinit() {
  bool ok = radio.init(cfg.mhz, cfg.bw, cfg.sf, cfg.cr, cfg.dbm);
  dirty = !ok;
  return ok;
}

static void startRx() {
  if (dirty && !reinit()) { lr2021dbg::hold = true; Serial.println("# radio re-init failed, holding"); return; }
  radio.standby();  // startReceive while already in RX fails with -706
  int16_t st = radio.startReceive();
  rxActive = st == RADIOLIB_ERR_NONE;
  if (!rxActive) {
    dirty = true;
    lr2021dbg::hold = true;
    Serial.printf("# startReceive failed: %d, holding\r\n", st);
  }
}

static void serviceRx() {
  if (!rxActive || digitalRead(P_LORA_DIO_1) == LOW) return;
  uint32_t irq = radio.getIrqStatus();
  if (irq & RADIOLIB_LR2021_IRQ_RX_DONE) {
    static uint8_t buf[256];
    size_t len = radio.getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    int16_t st = radio.readData(buf, len);
    bool crcOk = st == RADIOLIB_ERR_NONE;
    if (crcOk) rxOk++; else rxCrc++;
    float rssi = radio.getRSSI(), snr = radio.getSNR();
    uint8_t ps[10] = { 0 };
    radio.rawCmd(RADIOLIB_LR2021_CMD_GET_LORA_PACKET_STATUS, NULL, 0, ps, sizeof(ps));
    Serial.printf("RXSTAT len=%u st=%d rssi=%.1f snr=%.2f raw=", (unsigned)len, st, rssi, snr);
    for (int i = 0; i < 10; i++) Serial.printf("%02X ", ps[i]);
    Serial.printf("gain=%u crc=%d data=", ps[9], crcOk ? 1 : 0);
    for (size_t i = 0; i < len; i++) Serial.printf("%02X", buf[i]);
    Serial.print("\r\n");
  } else if (irq & RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR) {
    rxHdr++;
  }
  // The LR2021 stays in continuous RX after a packet: just clear the flags.
  radio.clearIrqState(RADIOLIB_LR2021_IRQ_ALL);
}

#if defined(BOARD_XIAO_WIO_LR2021)
static void usInitDwt() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// pinmon <ms>: watch the LR2021 pads wired to the XIAO (DIO7 -> D7, DIO8 -> D8, DIO11 -> D9).
// Edges are counted in hardware (GPIOTE -> PPI -> TIMER in counter mode), so MHz-rate signals
// are counted exactly; the high fraction comes from sampling the port in a loop.
// Output: PM,<pad>,<edges>,<toggle Hz>,<high fraction>,<level at end>
static void pinmon(uint32_t ms) {
  static const uint8_t pins[3] = { D7, D8, D9 };
  static const uint8_t pads[3] = { 7, 8, 11 };
  NRF_TIMER_Type* timers[3] = { NRF_TIMER1, NRF_TIMER3, NRF_TIMER4 };
  for (int i = 0; i < 3; i++) {
    pinMode(pins[i], INPUT);
    uint32_t pin = g_ADigitalPinMap[pins[i]];
    NRF_TIMER_Type* t = timers[i];
    t->TASKS_STOP = 1;
    t->MODE = TIMER_MODE_MODE_Counter;
    t->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    t->TASKS_CLEAR = 1;
    NRF_GPIOTE->CONFIG[5 + i] = (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) | ((pin & 31) << GPIOTE_CONFIG_PSEL_Pos) |
                                ((pin >> 5) << GPIOTE_CONFIG_PORT_Pos) | (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos);
    NRF_PPI->CH[10 + i].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[5 + i];
    NRF_PPI->CH[10 + i].TEP = (uint32_t)&t->TASKS_COUNT;
    NRF_PPI->CHENSET = 1UL << (10 + i);
    t->TASKS_START = 1;
  }
  uint32_t hi[3] = { 0, 0, 0 }, n = 0, t0 = millis();
  while (millis() - t0 < ms) {
    for (int i = 0; i < 3; i++) hi[i] += digitalRead(pins[i]);
    n++;
  }
  for (int i = 0; i < 3; i++) {
    NRF_TIMER_Type* t = timers[i];
    t->TASKS_CAPTURE[0] = 1;
    uint32_t edges = t->CC[0];
    t->TASKS_STOP = 1;
    NRF_PPI->CHENCLR = 1UL << (10 + i);
    NRF_GPIOTE->CONFIG[5 + i] = 0;
    Serial.printf("PM,%u,%lu,%.0f,%.3f,%d\r\n", pads[i], (unsigned long)edges, edges * 500.0f / ms, (float)hi[i] / n,
                  digitalRead(pins[i]));
  }
  pinMode(P_LORA_DIO_1, INPUT);
}

// pinsnap <n>: sample P1 in a tight loop (n <= 65536 samples). Each byte holds
// bit0 = DIO7 (D7, P1.12), bit1 = DIO8 (D8, P1.13), bit2 = DIO11 (D9, P1.14).
// Prints "SNAPBIN <n> <ns per sample>" then n raw bytes.
static void pinsnap(uint32_t n) {
  static uint8_t snap[65536];
  if (n > sizeof(snap)) n = sizeof(snap);
  pinMode(D7, INPUT); pinMode(D8, INPUT); pinMode(D9, INPUT);
  const volatile uint32_t* in = &NRF_P1->IN;
  usInitDwt();
  noInterrupts();
  uint32_t c0 = DWT->CYCCNT;
  for (uint32_t i = 0; i < n; i++) snap[i] = (uint8_t)((*in >> 12) & 7);
  uint32_t c1 = DWT->CYCCNT;
  interrupts();
  Serial.printf("SNAPBIN %lu %.2f\n", (unsigned long)n, (c1 - c0) * 1000.0f / 64.0f / n);
  for (uint32_t i = 0; i < n; i += 512) Serial.write(snap + i, n - i > 512 ? 512 : n - i);
  Serial.println();
  pinMode(P_LORA_DIO_1, INPUT);
}
#endif

// wfrun <nseg> [frames] [ov]: repeat the PRAM capture + FFT + dB command (entry 9, firmware/pram; load
// it with tools/pram.py) and stream binary frames, without host round trips. With ov = 1 (nseg 1),
// entry 9 leaves the next capture running while it computes, and a final "entry 9, nseg 0" stops it.
// Prints "WFBIN <nseg>\n",
// then per frame: A5 5A, seq (u16 LE), t_us (u32 LE, MCU time), chip_us (u32 LE, entry 9 runtime),
// 256 x u8 half-dB (FFT order: bin 0 = DC, 128..255 negative). Stops on 'x' or after `frames`
// (0 = until stopped), then prints "wfrun done <n>". Needs packet type 0x0B (the host sets it).
struct HalAccess : public ArduinoHal {
  using ArduinoHal::spiSettings;
};

// ReadRegMem32 on raw SPI at 16 MHz with one block transfer: RadioLib's memRead costs ~0.85 ms
// per 32 words here, this ~0.1 ms. nwords <= 32. Words come big-endian off the wire;
// out[] gets the bytes in memory order (byte at addr + k = out[k]).
static uint8_t lastReadStat[4];   // status bytes of the last raw read (printed on errors)
static bool fastMemRead(uint32_t addr, uint8_t* out, uint8_t nwords) {
  static uint8_t rx[2 + 128];
  // Response frame: clocked with 0x00 (NOP). The buffer must be in RAM, not const: the nRF52's
  // SPIM EasyDMA can't read flash, so a const buffer sends nothing useful.
  static uint8_t zeros[2 + 128];
  uint8_t cmd[6] = { 0x01, 0x06, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, nwords };
  SPISettings fast(16000000, MSBFIRST, SPI_MODE0);
  for (int pass = 0; pass < 2; pass++) {
    delayMicroseconds(2);  // BUSY rises a moment after NSS goes high: don't catch it still low
    uint32_t t = micros();
    while (digitalRead(P_LORA_BUSY)) if (micros() - t > 5000) return false;
    SPI.beginTransaction(fast);
    digitalWrite(P_LORA_NSS, LOW);
    if (pass == 0) SPI.transfer(cmd, nullptr, sizeof(cmd));
    else SPI.transfer(zeros, rx, 2 + 4 * nwords);
    digitalWrite(P_LORA_NSS, HIGH);
    SPI.endTransaction();
  }
  for (int i = 0; i < 4; i++) lastReadStat[i] = rx[i];
  for (int i = 0; i < nwords; i++)
    for (int b = 0; b < 4; b++) out[4 * i + b] = rx[2 + 4 * i + 3 - b];
  return true;
}

// Read-type command on raw SPI (command frame, BUSY wait, response frame clocked with 0x00 from RAM).
// Returns the command status (bits 3:1 of the first status byte; 3 = data) or -1 on a BUSY timeout.
static int fastCmdRead(const uint8_t* cmd, size_t clen, uint8_t* out, size_t olen, uint32_t busyUs) {
  static uint8_t tx[16], rx[2 + 48], zeros[2 + 48];
  if (clen > sizeof(tx) || olen > 48) return -1;
  memcpy(tx, cmd, clen);
  SPISettings fast(16000000, MSBFIRST, SPI_MODE0);
  for (int pass = 0; pass < 2; pass++) {
    delayMicroseconds(2);
    // micros() here doesn't run the full 32 bits before wrapping, so a long wait counts in millis()
    uint32_t t = micros(), tm = millis();
    while (digitalRead(P_LORA_BUSY))
      if (busyUs >= 100000 ? millis() - tm > busyUs / 1000 : micros() - t > busyUs) return -1;
    SPI.beginTransaction(fast);
    digitalWrite(P_LORA_NSS, LOW);
    if (pass == 0) SPI.transfer(tx, nullptr, clen);
    else SPI.transfer(zeros, rx, 2 + olen);
    digitalWrite(P_LORA_NSS, HIGH);
    SPI.endTransaction();
  }
  memcpy(out, rx + 2, olen);
  return (rx[0] >> 1) & 7;
}

static void wfrun(uint32_t nseg, uint32_t frames, bool ov) {
  HalAccess* hal = static_cast<HalAccess*>(static_cast<ArduinoHal*>(radio.getMod()->hal));
  SPISettings saved = hal->spiSettings;
  hal->spiSettings = SPISettings(16000000, MSBFIRST, SPI_MODE0);
  static uint8_t frame[4 + 4 + 4 + 256];
  uint8_t rsp[4];
  Serial.printf("WFBIN %lu\n", (unsigned long)nseg);
  uint32_t n = 0, sumChip = 0, sumRead = 0, sumWrite = 0;
  while (frames == 0 || n < frames) {
    if (Serial.available() && Serial.read() == 'x') break;
    uint32_t t0 = micros();
    uint8_t cmd[5] = { 0x01, 0x2D, 9, (uint8_t)nseg, (uint8_t)ov };   // ActivatePram sub-command 9
    int st = fastCmdRead(cmd, sizeof(cmd), rsp, 4, 200000);
    uint32_t chip = micros() - t0;
    if (st != 3 || rsp[1] == 0) {
      Serial.printf("\nwfrun error status=%d seg=%u last read frame %02X %02X %02X %02X\n", st, rsp[1], lastReadStat[0], lastReadStat[1],
                    lastReadStat[2], lastReadStat[3]);
      break;
    }
    uint32_t t1 = micros();
    if (!fastMemRead(0x807B00, frame + 12, 32) || !fastMemRead(0x807B80, frame + 140, 32)) {
      Serial.println("\nwfrun error busy");
      break;
    }
    uint32_t t2 = micros();
    frame[0] = 0xA5; frame[1] = 0x5A; frame[2] = n; frame[3] = n >> 8;
    memcpy(frame + 4, &t0, 4);
    memcpy(frame + 8, &chip, 4);
    Serial.write(frame, sizeof(frame));
    sumChip += chip; sumRead += t2 - t1; sumWrite += micros() - t2;
    n++;
  }
  if (ov) {                                    // stop the capture left running (bank back to the CPU)
    uint8_t stop[4] = { 0x01, 0x2D, 9, 0 };
    fastCmdRead(stop, sizeof(stop), rsp, 0, 200000);
  }
  hal->spiSettings = saved;
  // averages per frame: chip command, SPI readback, USB write
  Serial.printf("\nwfrun done %lu chip_us %lu read_us %lu write_us %lu\n", (unsigned long)n, (unsigned long)(n ? sumChip / n : 0),
                (unsigned long)(n ? sumRead / n : 0), (unsigned long)(n ? sumWrite / n : 0));
}

// aprswait <ms>: the chip demodulates APRS in the background (entry 6 of firmware/pram/demod.bin,
// started by tools/aprs_rx.py) and raises RxDone on its IRQ line for each good frame. The MCU
// just idles until that line goes high, like for a normal packet: then it clears the IRQ, reads
// the frames from PRAM 0x101A00, prints "APRS <ms> <hex>" per frame (AX.25, no FCS) and drops
// the bytes it read from the chip's output. Runs <ms> ms (0 = until 'x'), then prints
// "aprswait done irqs <n> frames <n>"; "# aprswait <ms>" every second meanwhile.
static uint32_t le32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void aprswait(uint32_t ms) {
  HalAccess* hal = static_cast<HalAccess*>(static_cast<ArduinoHal*>(radio.getMod()->hal));
  SPISettings saved = hal->spiSettings;
  hal->spiSettings = SPISettings(16000000, MSBFIRST, SPI_MODE0);
  static uint8_t out[512];
  uint8_t rsp[44];
  uint32_t irqs = 0, frames = 0, t0 = millis(), tAlive = t0;
  bool err = false;
  while (ms == 0 || millis() - t0 < ms) {
    if (Serial.available() && Serial.read() == 'x') break;
    // A line printed after a couple of seconds of silence didn't reach the PC in testing, so
    // print a keep-alive line every second.
    if (millis() - tAlive >= 1000) { tAlive = millis(); Serial.printf("# aprswait %lu\n", (unsigned long)(tAlive - t0)); }
    if (digitalRead(P_LORA_DIO_1) == LOW) { delay(1); continue; }   // idle until the chip's IRQ
    irqs++;
    uint8_t clr[6] = { 0x01, 0x16, 0x00, 0x04, 0x00, 0x00 };          // ClearIrq RxDone (bit 18)
    uint8_t stat[4] = { 0x01, 0x2D, 6, 2 };                            // entry 6, op 2: status
    if (fastCmdRead(clr, sizeof(clr), rsp, 0, 100000) < 0 || fastCmdRead(stat, sizeof(stat), rsp, sizeof(rsp), 100000) != 3) {
      err = true;
      break;
    }
    uint32_t outlen = le32(rsp + 40);
    if (outlen > sizeof(out)) outlen = sizeof(out);
    for (uint32_t o = 0; o < outlen && !err; o += 128) {
      uint32_t w = (outlen - o + 3) / 4;
      if (!fastMemRead(0x101A00 + o, out + o, w > 32 ? 32 : w)) err = true;
    }
    if (err) break;
    uint32_t o = 0;
    while (o + 2 <= outlen) {
      uint32_t n = out[o] | out[o + 1] << 8;
      if (o + 2 + n > outlen) break;
      Serial.printf("APRS %lu ", (unsigned long)(millis() - t0));
      for (uint32_t i = 0; i < n; i++) Serial.printf("%02X", out[o + 2 + i]);
      Serial.print("\n");
      frames++;
      o += 2 + n;
    }
    uint8_t ack[6] = { 0x01, 0x2D, 6, 3, (uint8_t)(o >> 8), (uint8_t)o };   // op 3: drop what was read
    if (fastCmdRead(ack, sizeof(ack), rsp, sizeof(rsp), 100000) != 3) { err = true; break; }
  }
  hal->spiSettings = saved;
  Serial.printf("aprswait done irqs %lu frames %lu%s\n", (unsigned long)irqs, (unsigned long)frames, err ? " error" : "");
}

// scanwait <ms>: the chip hops over its channel table by itself (entry 4 of
// firmware/pram/scan.bin, started by tools/scan.py) and only RxDone is routed to the IRQ pin, so
// the MCU idles until a packet has arrived on one of the channels. Then it reads the packet,
// sends op 3 (resume: the chip clears RxDone and carries on scanning), and prints
// "SCAN <ms> ch=<index> len=<n> st=<readData status> snr=<dB> rssi=<dBm> data=<hex>". Runs <ms> ms
// (0 = until 'x'), then prints "scanwait done packets <n>"; "# scanwait <ms>" every second.
static void scanwait(uint32_t ms) {
  static uint8_t buf[256];
  uint8_t rsp[40];
  uint32_t pkts = 0, t0 = millis(), tAlive = t0;
  bool err = false;
  while (ms == 0 || millis() - t0 < ms) {
    if (Serial.available() && Serial.read() == 'x') break;
    if (millis() - tAlive >= 1000) { tAlive = millis(); Serial.printf("# scanwait %lu\n", (unsigned long)(tAlive - t0)); }
    if (digitalRead(P_LORA_DIO_1) == LOW) { delay(1); continue; }   // idle until RxDone
    uint32_t t = millis() - t0;
    size_t len = radio.getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    int16_t st = radio.readData(buf, len);
    float snr = radio.getSNR(), rssi = radio.getRSSI();
    uint8_t resume[4] = { 0x01, 0x2D, 4, 3 };                         // entry 4, op 3: resume
    if (fastCmdRead(resume, sizeof(resume), rsp, sizeof(rsp), 100000) != 3) { err = true; break; }
    pkts++;
    Serial.printf("SCAN %lu ch=%u len=%u st=%d snr=%.2f rssi=%.1f data=", (unsigned long)t, rsp[1], (unsigned)len, st, snr, rssi);
    for (size_t i = 0; i < len; i++) Serial.printf("%02X", buf[i]);
    Serial.print("\n");
  }
  Serial.printf("scanwait done packets %lu%s\n", (unsigned long)pkts, err ? " error" : "");
}

// spifree <hex bytes>: send one command frame, then let go of NSS, SCK and MOSI (high-Z, NSS
// pulled up, SCK and MOSI pulled down) until BUSY has risen and dropped again, then take SPI back.
// While the pins are free a PRAM command can drive those pads itself (rpt.bin entry 3 clocks
// WriteTxFifo into the chip's own SPI slave). Prints "spifree st=<0 ok, -1 BUSY never rose,
// -2 BUSY stuck high> us=<BUSY high time>". Read results with mr afterwards.
static bool spifreePulls = true;
static uint32_t spifreeMinMs = 0;
static uint8_t sfMosi[16], sfMiso[16];
static uint32_t sfBits;
static inline void sfEdge(int sck, int lastSck) {
  if (sck && !lastSck && sfBits < 128) {
    uint32_t i = sfBits >> 3; uint8_t m = 0x80 >> (sfBits & 7);
    if (digitalRead(P_LORA_MOSI)) sfMosi[i] |= m;
    if (digitalRead(P_LORA_MISO)) sfMiso[i] |= m;
    sfBits++;
  }
}   // `spifree w<ms> ...`: keep the pins released at least this long   // `spifree np ...`: no MCU pulls (the chip sets levels with its own pulls)
static void spifree(const uint8_t* cmd, size_t n) {
  static uint8_t tx[64];
  if (n > sizeof(tx)) n = sizeof(tx);
  memcpy(tx, cmd, n);
  uint32_t t = micros();
  while (digitalRead(P_LORA_BUSY)) if (micros() - t > 100000) { Serial.println("spifree st=-2 us=0"); return; }
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(P_LORA_NSS, LOW);
  SPI.transfer(tx, nullptr, n);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
  SPI.end();
  pinMode(P_LORA_NSS, spifreePulls ? INPUT_PULLUP : INPUT);
  pinMode(P_LORA_SCLK, spifreePulls ? INPUT_PULLDOWN : INPUT);
  pinMode(P_LORA_MOSI, spifreePulls ? INPUT_PULLDOWN : INPUT);
  pinMode(P_LORA_MISO, INPUT);       // reconnect its input buffer so MISO can be watched
  int st = 0;
  uint32_t t0 = micros();
  while (!digitalRead(P_LORA_BUSY)) if (micros() - t0 > 5000) { st = -1; break; }
  uint32_t t1 = micros(), tm = millis();
  // Watch the released pins while the chip runs: samples, NSS low, SCK edges, MOSI high
  uint32_t ns = 0, nssLow = 0, sckEdges = 0, mosiHigh = 0, sckHigh = 0, misoHigh = 0, nssHigh = 0;
  int lastSck = digitalRead(P_LORA_SCLK);
  memset(sfMosi, 0, sizeof(sfMosi)); memset(sfMiso, 0, sizeof(sfMiso)); sfBits = 0;
  if (st == 0) while (digitalRead(P_LORA_BUSY)) {
    if (millis() - tm > 3000) { st = -2; break; }
    int sck = digitalRead(P_LORA_SCLK);
    ns++;
    nssLow += !digitalRead(P_LORA_NSS);
    sckEdges += sck != lastSck;
    mosiHigh += digitalRead(P_LORA_MOSI);
    sckHigh += sck;
    misoHigh += digitalRead(P_LORA_MISO);
    sfEdge(sck, lastSck);
    lastSck = sck;
  }
  if (st == 0) while (millis() - tm < spifreeMinMs) {    // the chip may drop BUSY itself mid-command
    if (Serial.available() && Serial.read() == 'x') break;   // 'x' ends the wait early
    int sck = digitalRead(P_LORA_SCLK);
    ns++;
    nssLow += !digitalRead(P_LORA_NSS);
    sckEdges += sck != lastSck;
    mosiHigh += digitalRead(P_LORA_MOSI);
    sckHigh += sck;
    misoHigh += digitalRead(P_LORA_MISO);
    sfEdge(sck, lastSck);
    lastSck = sck;
  }
  uint32_t us = micros() - t1;
  digitalWrite(P_LORA_NSS, HIGH);
  pinMode(P_LORA_NSS, OUTPUT);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
  SPI.begin();
  Serial.printf("spifree st=%d us=%lu samples=%lu nss_low=%lu sck_edges=%lu mosi_high=%lu sck_high=%lu miso_high=%lu\n",
                st, (unsigned long)us, (unsigned long)ns, (unsigned long)nssLow, (unsigned long)sckEdges, (unsigned long)mosiHigh,
                (unsigned long)sckHigh, (unsigned long)misoHigh);
  Serial.printf("spifree bits=%lu mosi=", (unsigned long)sfBits);
  for (uint32_t i = 0; i < (sfBits + 7) / 8; i++) Serial.printf("%02X", sfMosi[i]);
  Serial.print(" miso=");
  for (uint32_t i = 0; i < (sfBits + 7) / 8; i++) Serial.printf("%02X", sfMiso[i]);
  Serial.print("\n");
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool handleRadio(const char* cmd) {
  if (!strcmp(cmd, "get radio")) {
    Serial.printf("  -> > %.3f,%g,%u,%u\r\n", cfg.mhz, cfg.bw, cfg.sf, cfg.cr);
    return true;
  }
  if (!strncmp(cmd, "set radio ", 10)) {
    // (newlib-nano's sscanf has no %f)
    char* p;
    float mhz = strtod(cmd + 10, &p);
    float bw = *p == ',' ? strtod(p + 1, &p) : 0;
    unsigned sf = *p == ',' ? strtoul(p + 1, &p, 10) : 0;
    unsigned cr = *p == ',' ? strtoul(p + 1, &p, 10) : 0;
    if (mhz <= 0 || bw <= 0 || !sf || !cr) { Serial.println("  -> ERR format"); return true; }
    stopRx();
    cfg.mhz = mhz; cfg.bw = bw; cfg.sf = sf; cfg.cr = cr;
    bool ok = reinit();
    Serial.println(ok ? "  -> OK" : "  -> ERR init");
    if (ok) { lr2021dbg::hold = false; startRx(); }
    return true;
  }
  if (!strcmp(cmd, "get tx")) {
    Serial.printf("  -> > %d\r\n", cfg.dbm);
    return true;
  }
  if (!strncmp(cmd, "set tx ", 7)) {
    cfg.dbm = atoi(cmd + 7);
    bool wasRx = rxActive;
    stopRx();
    int16_t st = radio.setOutputPower(cfg.dbm);
    Serial.printf(st == RADIOLIB_ERR_NONE ? "  -> OK\r\n" : "  -> ERR %d\r\n", st);
    if (wasRx) startRx();
    return true;
  }
  if (!strncmp(cmd, "loratx ", 7)) {
    static uint8_t buf[255];
    size_t n = 0;
    for (const char* p = cmd + 7; p[0] && p[1] && n < sizeof(buf); p += 2) {
      int h = hexval(p[0]), l = hexval(p[1]);
      if (h < 0 || l < 0) break;
      buf[n++] = (uint8_t)(h << 4 | l);
    }
    if (!n) { Serial.println("  -> ERR no payload"); return true; }
    bool wasRx = rxActive;
    stopRx();
    if (dirty) reinit();
    int16_t st = radio.transmit(buf, n);
    if (st == RADIOLIB_ERR_NONE) txCount++;
    Serial.printf(st == RADIOLIB_ERR_NONE ? "  -> OK %u bytes\r\n" : "  -> ERR %d\r\n", st == RADIOLIB_ERR_NONE ? (int)n : st);
    if (wasRx || !lr2021dbg::hold) startRx();
    return true;
  }
#if defined(BOARD_XIAO_WIO_LR2021)
  if (!strncmp(cmd, "pinmon ", 7)) {
    pinmon(strtoul(cmd + 7, NULL, 0));
    Serial.println("pinmon done");
    return true;
  }
  if (!strncmp(cmd, "pinsnap ", 8)) {
    pinsnap(strtoul(cmd + 8, NULL, 0));
    return true;
  }
#endif
  if (!strncmp(cmd, "wfrun ", 6)) {
    char* q;
    uint32_t nseg = strtoul(cmd + 6, &q, 0), frames = strtoul(q, &q, 0), ov = strtoul(q, NULL, 0);
    if (nseg < 1 || nseg > 10) { Serial.println("  -> ERR nseg 1..10"); return true; }
    if (ov && nseg != 1) { Serial.println("  -> ERR ov needs nseg 1"); return true; }
    stopRx();
    wfrun(nseg, frames, ov != 0);
    return true;
  }
  if (!strncmp(cmd, "aprswait ", 9)) {
    stopRx();
    aprswait(strtoul(cmd + 9, NULL, 0));
    return true;
  }
  if (!strncmp(cmd, "scanwait ", 9)) {
    stopRx();
    scanwait(strtoul(cmd + 9, NULL, 0));
    return true;
  }
  if (!strncmp(cmd, "spifree ", 8)) {
    static uint8_t buf[64];
    size_t n = 0;
    const char* p0 = cmd + 8;
    spifreePulls = strncmp(p0, "np ", 3) != 0;
    if (!spifreePulls) p0 += 3;
    spifreeMinMs = 0;
    if (*p0 == 'w') { char* q; spifreeMinMs = strtoul(p0 + 1, &q, 10); p0 = q; }
    for (const char* p = p0; *p && n < sizeof(buf);) {
      char* q;
      unsigned long b = strtoul(p, &q, 16);
      if (q == p) break;
      buf[n++] = (uint8_t)b;
      p = q;
    }
    stopRx();
    spifree(buf, n);
    return true;
  }
  if (!strcmp(cmd, "rxread")) {
    // Read the packet a raw command sequence left in the RX FIFO (e.g. after a PRAM CAD scan),
    // without touching the receiver state. "rc" can't do this: the FIFO data must follow the
    // opcode in the same SPI transfer.
    static uint8_t buf[256];
    size_t len = radio.getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    uint8_t ps[10] = { 0 };
    radio.rawCmd(RADIOLIB_LR2021_CMD_GET_LORA_PACKET_STATUS, NULL, 0, ps, sizeof(ps));
    int16_t st = radio.readData(buf, len);
    Serial.printf("  -> > len=%u st=%d snr=%.2f rssi=%.1f data=", (unsigned)len, st, radio.getSNR(), radio.getRSSI());
    for (size_t i = 0; i < len; i++) Serial.printf("%02X", buf[i]);
    Serial.print("\r\n");
    return true;
  }
  if (!strcmp(cmd, "rxstats")) {
    Serial.printf("  -> > rx=%lu crc_err=%lu hdr_err=%lu tx=%lu\r\n", (unsigned long)rxOk, (unsigned long)rxCrc,
                  (unsigned long)rxHdr, (unsigned long)txCount);
    return true;
  }
  if (!strcmp(cmd, "help")) {
    Serial.println("# debug: mr mw rc err romprobe cap capwait stream streamdump streamstats iqstream hold wfrun aprswait scanwait");
    Serial.println("# radio: get radio | set radio <MHz>,<BW>,<SF>,<CR> | get tx | set tx <dBm> | loratx <hex> | rxstats");
    Serial.printf("# %s, hold %s\r\n", board.getManufacturerName(), lr2021dbg::hold ? "on" : "off");
    return true;
  }
  return false;
}

static void handleLine(char* line) {
  while (*line == ' ') line++;
  if (!*line) return;
  if (handleRadio(line)) return;
  // Capture commands switch the chip to raw mode (packet type 0x0B, FIFO routes): hold the
  // receiver, and re-initialise before receiving again. Other raw commands (mr, mw, rc, ...) run
  // alongside the receiver, as they did on the original repeater build, so e.g. a manual gain set
  // with "rc 021A 0 05" stays in effect.
  bool capture = !strncmp(line, "stream ", 7) || !strncmp(line, "iqstream ", 9) || !strncmp(line, "cap ", 4) ||
                 !strncmp(line, "capwait ", 8);
  if (capture) { stopRx(); lr2021dbg::hold = true; dirty = true; }
  if (lr2021dbg::handle(radio, line, Serial)) {
    if (lr2021dbg::hold) stopRx();
    return;
  }
  Serial.printf("  -> ERR unknown command: %s\r\n", line);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  board.begin();
  pinMode(P_LORA_DIO_1, INPUT);
  if (!radio_init()) {
    Serial.println("# radio init FAILED");
    while (true) delay(1000);
  }
  lr2021dbg::hold = true;  // receiver off until "hold off" / "set radio"
  Serial.printf("# LR2021 console on %s (receiver held)\r\n", board.getManufacturerName());
}

void loop() {
  static char line[300];
  static size_t n = 0;
  while (Serial.available()) {
    int ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      line[n] = 0;
      if (n) handleLine(line);
      n = 0;
    } else if (n < sizeof(line) - 1) {
      line[n++] = (char)ch;
    }
  }
  if (!lr2021dbg::hold && !rxActive) startRx();
  serviceRx();
}
