// Transmitting from the LR2021's own CPU (docs/pram-lora-tx.md).
//
// The CPU can't write the TX FIFO directly: its fill level lives in the SPI block, so bytes the CPU
// puts in the FIFO's RAM underflow (the header goes out, the payload is garbage). Two ways round it:
//
// - Entry 3 clocks WriteTxFifo into the chip's own SPI slave through the SPI pads (NSS, SCK, MOSI =
//   pads 1, 2, 3), setting each level with the pad's pull while the MCU leaves its pins floating;
//   the hardware modem then sends the packet.
// - Entry 1 encodes the packet itself (whitening, CRC, Hamming, interleaving, Gray:
//   tools/lora_encode.py is the reference) and plays the LoRa chirps by writing the synthesizer
//   word 0xF40144 every 2 us during a CW carrier (docs/pram-pll.md), cycle-counted with interrupts
//   off.
//
// At 62.5 kHz one synthesizer LSB is 1e6 / 2^20 Hz, so the bandwidth is exactly 65536 LSB, and with
// 8 updates per chip each step of an SF7 chirp is exactly 64 LSB. In general:
//   bw code b (3 = 62.5 kHz ... 6 = 500 kHz): bandwidth = 2^(13 + b) LSB, updates per chip
//   U = 2^(6 - b), updates per symbol M = 2^(sf + 6 - b), step = 2^(2b + 7 - sf) LSB.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;

struct resp { u32 len; u8 *buf; };
typedef int (*handler_t)(u32 len, const u8 *p, struct resp *r);
#define H_STANDBY     ((handler_t)0xFF18)   // 0x0128
#define H_TX_TEST     ((handler_t)0x103AC)  // 0x020E SetTxTestMode (02 = CW)

#define PLL_FREQ (*(volatile u32 *)0xF40144)

// RAM above the stack (docs/lr2021-cpu.md#memory)
#define PKT  ((u8 *)0x807800)               // packet to send (256)
#define SEG  ((u16 *)0x807900)              // chirp list: preamble, sync, SFD, data (<= 460 x u16)
#define WB   ((u8 *)0x807C98)               // whitened payload + CRC (<= 257)
#define SEG_MAX 460
#define SEG_DOWN 0x8000u
#define SEG_QUARTER 0x4000u

static u32 crc16(const u8 *p, u32 n) {
  u32 c = 0;
  for (u32 i = 0; i < n; i++) {
    u32 b = p[i];
    for (u32 k = 0; k < 8; k++) {
      c = (((c >> 8) ^ b) & 0x80) ? (c << 1) ^ 0x1021 : c << 1;
      c &= 0xFFFF;
      b <<= 1;
    }
  }
  return c;
}

static u32 inv_gray(u32 g) {
  g ^= g >> 1; g ^= g >> 2; g ^= g >> 4; g ^= g >> 8;
  return g;
}

static u32 hamming(u32 n, u32 cr) {
  u32 b0 = n & 1, b1 = (n >> 1) & 1, b2 = (n >> 2) & 1, b3 = (n >> 3) & 1;
  if (cr == 1) return (b0 << 4) | (b1 << 3) | (b2 << 2) | (b3 << 1) | (b0 ^ b1 ^ b2 ^ b3);
  u32 p0 = b0 ^ b1 ^ b2, p1 = b1 ^ b2 ^ b3, p2 = b0 ^ b1 ^ b3, p3 = b0 ^ b2 ^ b3;
  u32 full = (b0 << 7) | (b1 << 6) | (b2 << 5) | (b3 << 4) | (p0 << 3) | (p1 << 2) | (p2 << 1) | p3;
  return full >> (4 - cr);
}

// nibble j of the whitened payload + CRC, low nibble first
static u32 nib(u32 j) { return (WB[j >> 1] >> ((j & 1) << 2)) & 0xF; }

// Interleave sf_app codewords of cw_len bits into cw_len symbols; append them to out.
static u32 interleave(const u8 *cw, u32 sf_app, u32 cw_len, u32 hdr, u32 nmask, u16 *out) {
  for (u32 i = 0; i < cw_len; i++) {
    u32 v = 0;
    for (u32 j = 0; j < sf_app; j++) {
      u32 r = i + 2 * sf_app - j - 1;   // (i - j - 1) mod sf_app; i < 8 < 2 * sf_app
      while (r >= sf_app) r -= sf_app;
      v = (v << 1) | ((cw[r] >> (cw_len - 1 - i)) & 1);
    }
    v = inv_gray(v);
    out[i] = (hdr ? (v << 2) + 1 : v + 1) & nmask;
  }
  return cw_len;
}

// Payload -> data symbols (explicit header, CRC on). Returns the symbol count.
static u32 lora_encode(const u8 *pkt, u32 len, u32 sf, u32 cr, u32 ldro, u16 *out) {
  u32 s = 0xFF;
  for (u32 j = 0; j < len; j++) {
    WB[j] = pkt[j] ^ s;
    s = ((s << 1) & 0xFF) | (((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1);
  }
  u32 crc = len >= 2 ? crc16(pkt, len - 2) ^ pkt[len - 1] ^ (pkt[len - 2] << 8) : crc16(pkt, len);
  WB[len] = crc; WB[len + 1] = crc >> 8;
  u32 total = 2 * len + 4, k = 0, n = 0, nmask = (1u << sf) - 1;
  // header nibbles and checksum
  u32 h0 = len >> 4, h1 = len & 0xF, h2 = (cr << 1) | 1;
#define B(v, i) (((v) >> (i)) & 1)
  u32 c4 = B(h0, 3) ^ B(h0, 2) ^ B(h0, 1) ^ B(h0, 0);
  u32 c3 = B(h0, 3) ^ B(h1, 3) ^ B(h1, 2) ^ B(h1, 1) ^ B(h2, 0);
  u32 c2 = B(h0, 2) ^ B(h1, 3) ^ B(h1, 0) ^ B(h2, 3) ^ B(h2, 1);
  u32 c1 = B(h0, 1) ^ B(h1, 2) ^ B(h1, 0) ^ B(h2, 2) ^ B(h2, 1) ^ B(h2, 0);
  u32 c0 = B(h0, 0) ^ B(h1, 1) ^ B(h2, 3) ^ B(h2, 2) ^ B(h2, 1) ^ B(h2, 0);
#undef B
  u8 hn[5] = { (u8)h0, (u8)h1, (u8)h2, (u8)c4, (u8)((c3 << 3) | (c2 << 2) | (c1 << 1) | c0) };
  u8 cw[12];
  for (u32 r = 0; r < sf - 2; r++) {
    u32 v = r < 5 ? hn[r] : (k < total ? nib(k++) : 0);
    cw[r] = hamming(v, 4);
  }
  n += interleave(cw, sf - 2, 8, 1, nmask, out + n);
  u32 sf_app = ldro ? sf - 2 : sf;
  while (k < total) {
    for (u32 r = 0; r < sf_app; r++) cw[r] = hamming(k < total ? nib(k++) : 0, cr);
    n += interleave(cw, sf_app, cr + 4, ldro, nmask, out + n);
  }
  return n;
}

// lp with lp_count = 0 runs 2^32 times: callers keep n > 0
static inline void delay(u32 n) {
  __asm__ volatile("mov lp_count,%0\n\tlp 1f\n\tnop_s\n1:" :: "r"(n) : "lp_count", "memory");
}

// Play the chirp list. Each update writes fmin + (idx mod M) * step, then waits d cycles, except
// the last of each symbol, which waits dlast: that absorbs the per-symbol overhead so every update
// is the same length and a symbol is exactly 2^SF chips. Interrupts must be off. Up and down cost
// the same.
__attribute__((noinline, optimize("O2")))
static void emit(const u16 *seg, u32 n, u32 fmin, u32 ush, u32 sf, u32 step_sh, u32 d, u32 dlast) {
  u32 m = 1u << (sf + ush), mask = m - 1;
  for (u32 i = 0; i < n; i++) {
    u32 v = seg[i];
    u32 idx = v & SEG_DOWN ? mask : (v & 0xFFF) << ush;
    u32 cnt = (v & SEG_QUARTER ? m >> 2 : m) - 1;
    u32 dir = v & SEG_DOWN ? 0xFFFFFFFFu : 1;
    for (u32 k = 0; k < cnt; k++) {
      PLL_FREQ = fmin + ((idx & mask) << step_sh);
      idx += dir;
      delay(d);
    }
    PLL_FREQ = fmin + ((idx & mask) << step_sh);
    delay(dlast);
  }
}

struct txcfg { u32 sf, bwc, cr, ldro, pre, d, dlast, off; };

// Build the full chirp list for PKT[0..len) in SEG. Returns its length.
static u32 build(u32 len, const struct txcfg *c) {
  u32 n = 0;
  for (u32 i = 0; i < c->pre; i++) SEG[n++] = 0;
  SEG[n++] = 1 << 3;                    // sync word 0x12: nibbles x 8
  SEG[n++] = 2 << 3;
  SEG[n++] = SEG_DOWN;
  SEG[n++] = SEG_DOWN;
  SEG[n++] = SEG_DOWN | SEG_QUARTER;
  u32 k = lora_encode(PKT, len, c->sf, c->cr, c->ldro, SEG + n);
  for (u32 i = 0; i < k; i++) SEG[n + i] = (SEG[n + i] + c->off) & ((1u << c->sf) - 1);
  return n + k;
}

// Transmit SEG[0..n) as LoRa: CW carrier at the current RF frequency, chirps, standby.
// dry = 1: no carrier, just the timing (the CPU clock in standby may differ from TX).
static u32 transmit(u32 n, const struct txcfg *c, u32 dry) {
  static const u8 cw_mode[1] = { 2 };
  static const u8 stby[1] = { 0 };
  if (!(dry & 3)) H_TX_TEST(1, cw_mode, 0);
  u32 base = PLL_FREQ;
  u32 bw = 1u << (13 + c->bwc);
  u32 st;
  __asm__ volatile("clri %0" : "=r"(st) :: "memory");
  emit(SEG, n, base - bw / 2, 6 - c->bwc, c->sf, 2 * c->bwc + 7 - c->sf, c->d, c->dlast);
  PLL_FREQ = base;
  __asm__ volatile("seti %0" :: "r"(st) : "memory");
  if (!(dry & 1)) H_STANDBY(1, stby, 0);
  return base;
}

static u32 be16(const u8 *p) { return (p[0] << 8) | p[1]; }

// Entry 1: transmit PKT. Params: len (1), sf (1), bw code (1), cr (1), ldro (1), preamble (1),
//   d (2), dlast (2), flags (1: bit 0 dry run, bit 1 only build,
//   bit 2 the carrier is already on), data symbol offset (1, optional, test).
//   Response: chirps (2), base word (4).
int tx_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 11) return 1;
  struct txcfg c = { p[1], p[2], p[3], p[4], p[5], be16(p + 6), be16(p + 8), len > 11 ? p[11] : 0 };
  u32 plen = p[0], flags = p[10];
  if (plen == 0 || c.sf < 5 || c.sf > 12 || c.bwc < 3 || c.bwc > 6 || c.cr < 1 || c.cr > 4 || c.d == 0 || c.dlast == 0 || c.pre > 64)
    return 1;
  if (2 * c.bwc + 7 < c.sf) return 1;
  u32 n = build(plen, &c);
  if (n > SEG_MAX) return 1;
  u32 base = flags & 2 ? PLL_FREQ : transmit(n, &c, (flags & 1) | ((flags >> 1) & 2));
  r->buf[0] = n >> 8; r->buf[1] = n;
  r->buf[2] = base >> 24; r->buf[3] = base >> 16; r->buf[4] = base >> 8; r->buf[5] = base;
  r->len = 6;
  return 3;
}

// ---- Experiment: write the TX FIFO through the chip's own SPI pads (docs/pram-lora-tx.md) ----
//
// Pads 1-3 are NSS, SCK, MOSI (pad mux codes 3-5; found with entry 4), 4 is BUSY. SetDioFunction's GPIO functions show
// how the CPU drives a pad: mux 1, output enable (cfg bit 1), and 1 << pad written to 0xF23058
// (high) or 0xF2305C (low). 0xF0203C holds the pad input levels. If the SPI slave keeps listening
// to the pad input while the pad is a GPIO output, the CPU can clock WriteTxFifo (0x0002) into
// its own SPI slave. The host must release its NSS/SCK/MOSI pins first (console `spifree`).
#define PAD_CFG(i) (*(volatile u32 *)(0xF02004 + 4 * (i)))
#define PAD_MUX(i) (*(volatile u32 *)(0xF26004 + 4 * (i)))
#define GPIO_SET   (*(volatile u32 *)0xF23058)
#define GPIO_CLR   (*(volatile u32 *)0xF2305C)
#define PAD_IN     (*(volatile u32 *)0xF0203C)
#define TXF_LEVEL  (*(volatile u32 *)0xF2A0B0)
#define TXF_COUNT  (*(volatile u32 *)0xF3003C)
#define P_MOSI 3
#define P_SCK  2
#define P_NSS  1
#define SELF_LOG ((volatile u32 *)0x807F00)

// Method 0 drives a pad as a GPIO (mux 1); method 1 leaves the SPI mux alone and sets the level
// with the pad's pull (cfg bits 5:4: pull_hi / pull_lo codes), so the SPI slave still owns the pad.
static u32 __attribute__((section(".data"))) method = 0, pull_hi = 2, pull_lo = 1;   // .data: .bss isn't uploaded
static void pad(u32 i, u32 v) {
  if (method == 0) { if (v) GPIO_SET = 1u << i; else GPIO_CLR = 1u << i; }
  else PAD_CFG(i) = (PAD_CFG(i) & ~0x32u) | ((v ? pull_hi : pull_lo) << 4);
}
static void take(u32 i, u32 cfg, u32 mux) {
  if (method == 0) { PAD_MUX(i) = (mux & ~0x1Fu) | 1; PAD_CFG(i) = cfg | 6; }
  else PAD_CFG(i) = (cfg & ~0x32u) | 4;     // input on, output off; pad() sets the pull
}

__attribute__((noinline)) static void wait(u32 n) { if (n) delay(n); }

// Entry 3: params: wait before taking the pads (4, cycles), half-bit delay (2, cycles), n (1),
//   flags (1: bit 0 don't clock, only take the pads and read them back; bit 1 write 0xF24004 =
//   0x80000000 first (SPI ready, BUSY low); bit 2 write 0x02000000 after (BUSY high)), optional method (1),
//   pull code for high (1), pull code for low (1). Data at PKT. Clocks
//   NSS low, 0x00 0x02, data[n], NSS high. Log at 0x807F00 (and the response), LE words: pad
//   inputs before / after taking the pads / again, TX level and count
//   before and after, pad cfg and mux of pads 1-3 as found.
int selfspi_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 8) return 1;
  u32 pre = ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  u32 hb = be16(p + 4), n = p[6], flags = p[7];
  if (hb == 0) return 1;
  volatile u32 *L = SELF_LOG;
  wait(pre);
  L[0] = PAD_IN;
  L[3] = TXF_LEVEL; L[4] = TXF_COUNT;
  u32 cfg[4], mux[4];
  for (u32 i = 1; i <= 3; i++) { cfg[i] = PAD_CFG(i); mux[i] = PAD_MUX(i); }
  L[7] = cfg[1] | (cfg[2] << 8) | (cfg[3] << 16);
  L[8] = mux[1] | (mux[2] << 8) | (mux[3] << 16);
  if (len >= 11) { method = p[8]; pull_hi = p[9]; pull_lo = p[10]; } else method = 0;
  if (method == 0) { pad(P_NSS, 1); pad(P_SCK, 0); pad(P_MOSI, 0); }
  for (u32 i = 1; i <= 3; i++) take(i, cfg[i], mux[i]);
  pad(P_NSS, 1); pad(P_SCK, 0); pad(P_MOSI, 0);
  wait(hb);
  L[1] = PAD_IN;
  L[2] = PAD_IN;
  wait(hb);
  // flags bit 1: tell the SPI block we're ready (what the main loop does before it sleeps: BUSY low)
  if (flags & 2) { *(volatile u32 *)0xF24004 = 0x80000000u; wait(hb); }
  if (!(flags & 1)) {
    pad(P_NSS, 0);
    wait(hb);
    for (u32 k = 0; k < n + 2; k++) {
      u32 b = k == 0 ? 0x00 : k == 1 ? 0x02 : PKT[k - 2];
      for (u32 bit = 0; bit < 8; bit++) {
        pad(P_MOSI, (b >> (7 - bit)) & 1);
        wait(hb);
        pad(P_SCK, 1);
        wait(hb);
        pad(P_SCK, 0);
      }
    }
    wait(hb);
  }
  pad(P_NSS, 1);
  wait(hb);
  if (flags & 4) { *(volatile u32 *)0xF24004 = 0x02000000u; wait(hb); }   // BUSY back up
  for (u32 i = 1; i <= 3; i++) { PAD_CFG(i) = cfg[i]; PAD_MUX(i) = mux[i]; }
  L[5] = TXF_LEVEL; L[6] = TXF_COUNT;
  for (u32 i = 0; i < 9; i++) {
    u32 v = L[i];
    r->buf[4 * i] = v; r->buf[4 * i + 1] = v >> 8; r->buf[4 * i + 2] = v >> 16; r->buf[4 * i + 3] = v >> 24;
  }
  r->len = 36;
  return 3;
}

// Entry 4: pad walk. Params: wait (4, cycles), unit (4, cycles), pad mask (1, pads 0-3), optional
// method, pull high, pull low (1 each, as entry 3). Each pad
// in the mask is taken as a GPIO output and held high for (pad + 1) units, then low for a unit,
// then given back. With the host's pins released (console `spifree`), the MCU sees which of its
// lines each pad is. Response: 0xF23050 and 0xF23054 while the last pad was high.
int padwalk_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 9) return 1;
  u32 pre = ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  u32 unit = ((u32)p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
  u32 mask = p[8], v50 = 0, v54 = 0;
  if (len >= 12) { method = p[9]; pull_hi = p[10]; pull_lo = p[11]; } else method = 0;
  wait(pre);
  for (u32 i = 0; i < 4; i++) {
    if (!(mask & (1u << i))) continue;
    u32 cfg = PAD_CFG(i), mux = PAD_MUX(i);
    if (method == 0) pad(i, 0);
    take(i, cfg, mux);
    pad(i, 1);
    for (u32 k = 0; k <= i; k++) wait(unit);
    v50 = *(volatile u32 *)0xF23050; v54 = *(volatile u32 *)0xF23054;
    pad(i, 0);
    wait(unit);
    PAD_CFG(i) = cfg; PAD_MUX(i) = mux;
  }
  u32 v[2] = { v50, v54 };
  for (u32 i = 0; i < 2; i++) { r->buf[4*i] = v[i]; r->buf[4*i+1] = v[i] >> 8; r->buf[4*i+2] = v[i] >> 16; r->buf[4*i+3] = v[i] >> 24; }
  r->len = 8;
  return 3;
}
