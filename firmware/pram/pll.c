// FM from the chip itself: while the radio transmits a carrier (SetTxTestMode 0x020E 02), write the
// synthesizer frequency word 0xF40144 from a loop. The word is Hz * 2^20 / 1e6 (0.95 Hz per LSB)
// and a write takes effect at once, no latch needed; the PLL follows within ~2 us and its FM
// response is flat to ~100 kHz (docs/pram-pll.md).
//
// The core has no timers, so timing is a cycle-counted delay with interrupts off. The CPU runs at
// 32.0 MHz in TX; each update costs a fixed overhead on top of the delay (measured: ~32 cycles for
// entry 1 mode 0, ~34.5 for mode 1, ~25 per sample for entry 3), which the host subtracts.
// Interrupts are off for the whole command, so the ROM processes nothing meanwhile: fine for a CW
// carrier, and the command must stay under the host's ~1 s BUSY limit.
//
// Entry 1 params (BE): n symbols (2), cycles per symbol (4), mode (1), step (4, signed, word units),
//   tlen (2, optional)
//   mode 0: square wave, v = +1/-1 alternating
//   mode 1: table of int8 at 0x807800, v = table[i % tlen] (tlen param, 0 = n; <= 2048)
//   mode 2: sweep, v = i - n/2 (one step per symbol)
// Each symbol writes base + v * step; the base word is restored at the end.
// Response: base word (4).
typedef unsigned int u32;
typedef signed char s8;
typedef unsigned char u8;
struct resp { u32 len; u8 *buf; };
#define PLL_FREQ (*(volatile u32 *)0xF40144)
#define TAB ((const s8 *)0x807800)

static inline void delay(u32 n) {
  if (!n) return;
  __asm__ volatile("mov lp_count,%0\n\tlp 1f\n\tnop_s\n1:" :: "r"(n) : "lp_count", "memory");
}

int pll_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 11) return 1;
  u32 n = (p[0] << 8) | p[1];
  u32 cyc = ((u32)p[2] << 24) | (p[3] << 16) | (p[4] << 8) | p[5];
  u32 mode = p[6];
  int step = (int)(((u32)p[7] << 24) | (p[8] << 16) | (p[9] << 8) | p[10]);
  u32 tlen = len >= 13 ? (p[11] << 8) | p[12] : 0;
  if (!tlen) tlen = n;
  if (mode == 1 && tlen > 2048) return 1;
  u32 j = 0;
  u32 base = PLL_FREQ;
  volatile u32 st_save;
  { u32 st; __asm__ volatile("clri %0" : "=r"(st) :: "memory"); st_save = st; }
  int v = 1;
  for (u32 i = 0; i < n; i++) {
    if (mode == 0) v = -v;
    else if (mode == 1) { v = TAB[j]; if (++j == tlen) j = 0; }
    else v = (int)i - (int)(n >> 1);
    PLL_FREQ = base + v * step;
    delay(cyc);
  }
  PLL_FREQ = base;
  { u32 st = st_save; __asm__ volatile("seti %0" :: "r"(st) : "memory"); }
  r->buf[0] = base >> 24; r->buf[1] = base >> 16; r->buf[2] = base >> 8; r->buf[3] = base;
  r->len = 4;
  return 3;
}

// AFSK: FM by an audio tone that switches between two frequencies, phase-continuously (Bell 202 /
// APRS: mark 1200 Hz, space 2200 Hz, 1200 baud). The host sends tone bits (already NRZI coded,
// bit-stuffed, with flags), packed LSB first at 0x807800 (1 = mark).
// Entry 3 params (BE): nbits (2), cycles per audio sample (4), samples per bit (2), step (4, word
//   units per sine LSB: peak deviation = 127 * step), mark phase increment (4), space increment (4)
//   (increment = f_tone / f_sample * 2^32). Response: base word (4).
static const s8 SINE[256] = {
     0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
    49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
    90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
   117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
   127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
   117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
    90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
    49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
     0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
   -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
   -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
  -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
  -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
  -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
   -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
   -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3
};
#define BITS ((const u8 *)0x807800)

int afsk_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 20) return 1;
  u32 nbits = (p[0] << 8) | p[1];
  u32 cyc = ((u32)p[2] << 24) | (p[3] << 16) | (p[4] << 8) | p[5];
  u32 spb = (p[6] << 8) | p[7];
  int step = (int)(((u32)p[8] << 24) | (p[9] << 16) | (p[10] << 8) | p[11]);
  u32 inc_m = ((u32)p[12] << 24) | (p[13] << 16) | (p[14] << 8) | p[15];
  u32 inc_s = ((u32)p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
  if (nbits > 2048 * 8 || spb == 0) return 1;
  u32 base = PLL_FREQ, ph = 0;
  volatile u32 st_save;
  { u32 st; __asm__ volatile("clri %0" : "=r"(st) :: "memory"); st_save = st; }
  for (u32 b = 0; b < nbits; b++) {
    u32 inc = (BITS[b >> 3] >> (b & 7)) & 1 ? inc_m : inc_s;
    for (u32 k = 0; k < spb; k++) {
      PLL_FREQ = base + SINE[ph >> 24] * step;
      ph += inc;
      delay(cyc);
    }
  }
  PLL_FREQ = base;
  { u32 st = st_save; __asm__ volatile("seti %0" :: "r"(st) : "memory"); }
  r->buf[0] = base >> 24; r->buf[1] = base >> 16; r->buf[2] = base >> 8; r->buf[3] = base;
  r->len = 4;
  return 3;
}
