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
//   pinmon <ms> / pinsnap <n>       XIAO only: watch the pads wired to the MCU (docs/test-bus.md)
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
  if (!strcmp(cmd, "rxstats")) {
    Serial.printf("  -> > rx=%lu crc_err=%lu hdr_err=%lu tx=%lu\r\n", (unsigned long)rxOk, (unsigned long)rxCrc,
                  (unsigned long)rxHdr, (unsigned long)txCount);
    return true;
  }
  if (!strcmp(cmd, "help")) {
    Serial.println("# debug: mr mw rc err romprobe cap capwait stream streamdump streamstats iqstream hold");
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
