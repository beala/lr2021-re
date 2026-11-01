// Text on a waterfall: the LR2021 paints characters across frequency with its own CW carrier
// (docs/pram-waterfall-text.md).
//
// While the radio sends a CW carrier (SetTxTestMode 0x020E 02), the blob hops the synthesizer word
// 0xF40144 between pixel positions. One call draws one scanline (font row) of the text: for every
// pass it visits each pixel column left to right, sitting `dwell` cycles at base + off + x * spacing
// if the pixel is lit, or at the park frequency (the carrier, base) if not. Every pixel therefore
// gets the same share of time and the same brightness. A waterfall that averages over a pass shows
// the scanline; the host calls row 0..6 in turn so the text scrolls past top row first.
//
// Font: 5x7 ASCII (font5x7.h) in PRAM, one blank column between characters (not visited).
// Timing: cycle-counted with interrupts off, like pll.c; a call must stay under the host's ~1 s
// BUSY limit, so the host picks passes accordingly.
//
// Entry 1 params (BE): nchars (1), row 0-6 (1), passes (2), dwell cycles (4), spacing (4, word
//   units), offset (4, signed, word units), flags (1, optional: bit 0 mirror left-right).
//   Text: ASCII at 0x807800. Response: base word (4).
typedef unsigned int u32;
typedef unsigned char u8;
struct resp { u32 len; u8 *buf; };
#define PLL_FREQ (*(volatile u32 *)0xF40144)
#define TEXT ((const u8 *)0x807800)
#define MAXCH 64

#include "font5x7.h"

static inline void delay(u32 n) {
  if (!n) return;
  __asm__ volatile("mov lp_count,%0\n\tlp 1f\n\tnop_s\n1:" :: "r"(n) : "lp_count", "memory");
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

int text_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 16) return 1;
  u32 n = p[0], row = p[1], passes = (p[2] << 8) | p[3];
  u32 dwell = be32(p + 4), spacing = be32(p + 8), off = be32(p + 12);
  u32 mirror = len > 16 ? p[16] & 1 : 0;
  if (n == 0 || n > MAXCH || row > 6) return 1;
  // this scanline's pixels, one bit per column (6 per character, the 6th always blank)
  static u32 line[(MAXCH * 6 + 31) / 32] __attribute__((section(".data")));
  u32 w = n * 6;
  for (u32 i = 0; i < (w + 31) / 32; i++) line[i] = 0;
  for (u32 c = 0; c < n; c++) {
    u32 ch = TEXT[c];
    if (ch < 32 || ch > 126) ch = '?';
    const u8 *g = FONT + (ch - 32) * 5;
    for (u32 k = 0; k < 5; k++)
      if ((g[k] >> row) & 1) {
        u32 x = c * 6 + k;
        if (mirror) x = w - 2 - x;
        line[x >> 5] |= 1u << (x & 31);
      }
  }
  u32 base = PLL_FREQ, f0 = base + off;
  u32 st;
  __asm__ volatile("clri %0" : "=r"(st) :: "memory");
  for (u32 ps = 0; ps < passes; ps++)
    for (u32 x = 0; x < w; x++) {
      if (x % 6 == 5) continue;          // gap between characters: no time spent
      PLL_FREQ = (line[x >> 5] >> (x & 31)) & 1 ? f0 + x * spacing : base;
      delay(dwell);
    }
  PLL_FREQ = base;
  __asm__ volatile("seti %0" :: "r"(st) : "memory");
  r->buf[0] = base >> 24; r->buf[1] = base >> 16; r->buf[2] = base >> 8; r->buf[3] = base;
  r->len = 4;
  return 3;
}
