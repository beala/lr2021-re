// On-chip spectrum: windowed 256-point FFTs over a raw capture, power averaged per bin.
// Called as PRAM entry 7 (see sweep.S). The host runs the capture first (packet type 0x0B,
// 0x0290 one-shot into 0x804000, source 0 = int8 I/Q), stops it, and leaves the chip in standby,
// so the capture bank isn't owned by a DMA while this runs.
//
// Params: nseg (1 byte, segments of 256 samples, <= 10), skip (1 byte, segments to skip at the
// start). Result: 256 x u32 power sums at ACC (bin 0 = DC, bins 128..255 = negative
// frequencies), read back with ReadRegMem32. Returns 2 bytes: segments processed.
//
// Build: -mcpu=em4_dmips -mrf16 (16-entry register file), -mno-sdata (gp belongs to the ROM).
#include "fft_tables.h"

typedef unsigned int u32;
typedef int s32;
typedef unsigned char u8;
typedef signed char s8;

#define N 256
#define CAP ((const s8 *)0x804000)
#define ACC ((volatile u32 *)0x805400)
#define WORK ((s32 *)0x805800)  // re/im interleaved, 2 KB
#define MAXSEG 10

struct resp { u32 len; u8 *buf; };

static inline u32 bitrev8(u32 x) {
  x = ((x & 0xF0) >> 4) | ((x & 0x0F) << 4);
  x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
  x = ((x & 0xAA) >> 1) | ((x & 0x55) << 1);
  return x;
}

// ROM command handlers (same ABI as PRAM entries): (len, params, response)
typedef int (*handler_t)(u32 len, const u8 *p, struct resp *r);
#define H_SET_STANDBY ((handler_t)0xFF18)
#define H_SET_RX      ((handler_t)0xFCFC)
#define H_CAPTURE_CFG ((handler_t)0xF9A0)   // 0x0290
#define H_CAPTURE_PTR ((handler_t)0x5B44)   // 0x0291: 2-byte write pointer
#define CAP_WORDS 1280                      // 5 KB = 10 segments of 256 int8 I/Q samples
#define POLL_MAX 200000

// One-shot capture of source 0 into 0x804000, driven from inside the command. Expects packet
// type 0x0B (raw) to be set. Returns the final write pointer; *polls = poll iterations.
static u32 capture(u32 *polls) {
  // const tables: no memcpy (there's no libc); the ROM handlers only read their parameters
  static const u8 stby[1] = { 0 }, rx[3] = { 0xFF, 0xFF, 0xFF };
  static const u8 cfg[10] = { 0xFF, 0, 0, 0, 0, 0xD0, 0, 0, CAP_WORDS >> 8, CAP_WORDS & 0xFF };
  static const u8 stop[6] = { 0xFF, 0, 0, 0, 0, 0 };
  u8 ptr[2];
  struct resp pr;
  pr.len = 0; pr.buf = ptr;
  u32 v = 0, n = 0;
  H_SET_STANDBY(1, stby, 0);
  H_CAPTURE_CFG(10, cfg, 0);
  H_SET_RX(3, rx, 0);
  for (n = 0; n < POLL_MAX; n++) {
    H_CAPTURE_PTR(0, 0, &pr);
    v = ((u32)ptr[0] << 8) | ptr[1];
    if (v >= CAP_WORDS * 4) break;
  }
  H_CAPTURE_CFG(6, stop, 0);
  H_SET_STANDBY(1, stby, 0);
  *polls = n;
  return v;
}

// Entry 8 (debug): capture only. Returns 6 bytes: pointer (u16), poll iterations (u32).
int capture_cmd(u32 len, const u8 *p, struct resp *r) {
  u32 polls, v = capture(&polls);
  r->buf[0] = v >> 8; r->buf[1] = v;
  r->buf[2] = polls >> 24; r->buf[3] = polls >> 16; r->buf[4] = polls >> 8; r->buf[5] = polls;
  r->len = 6;
  return 3;
}

int fft_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 2) return 1;
  u32 nseg = p[0], skip = p[1];
  if (nseg == 0 || nseg + skip > MAXSEG) return 1;
  s32 *w = WORK;
  for (u32 k = 0; k < N; k++) ACC[k] = 0;
  for (u32 seg = skip; seg < skip + nseg; seg++) {
    const s8 *x = CAP + seg * N * 2;
    // window + bit-reversed load (int8 -> int32, Q15 window, keep 15 bits of fraction off)
    for (u32 n = 0; n < N; n++) {
      s32 h = WIN[n < 128 ? n : 255 - n];
      u32 j = bitrev8(n);
      w[2 * j] = ((s32)x[2 * n] * h) >> 7;      // int8 * Q15 >> 7 = Q8 headroom: |v| <= 2^15
      w[2 * j + 1] = ((s32)x[2 * n + 1] * h) >> 7;
    }
    // radix-2 DIT butterflies; values grow at most 2x per stage: |v| <= 2^15 * 2^8 = 2^23,
    // twiddle products <= 2^23 * 2^15 = 2^38 -> scale inputs to the multiply by >> 8 first
    for (u32 size = 2; size <= N; size <<= 1) {
      u32 half = size >> 1, tstep = N / size;
      for (u32 i = 0; i < N; i += size) {
        for (u32 k = 0; k < half; k++) {
          s32 wr = TW[k * tstep][0], wi = TW[k * tstep][1];
          s32 *a = &w[2 * (i + k)], *b = &w[2 * (i + k + half)];
          s32 br = b[0], bi = b[1];
          // complex multiply b * w in Q15, with 64-bit-free range: split br/bi to keep < 2^31
          s32 tr = (s32)(((long long)br * wr - (long long)bi * wi) >> 15);
          s32 ti = (s32)(((long long)br * wi + (long long)bi * wr) >> 15);
          b[0] = a[0] - tr; b[1] = a[1] - ti;
          a[0] += tr; a[1] += ti;
        }
      }
    }
    // power, scaled to fit 10 segments in u32: |X| <= 2^23 -> |X|^2 <= 2^46 -> >> 16
    for (u32 k = 0; k < N; k++) {
      s32 re = w[2 * k] >> 4, im = w[2 * k + 1] >> 4;     // |.| <= 2^19
      u32 pw = (u32)(((long long)re * re + (long long)im * im) >> 8);
      ACC[k] += pw;
    }
  }
  r->buf[0] = 0; r->buf[1] = (u8)nseg; r->len = 2;
  return 3;
}

// Entry 9: capture + FFT in one command. Params: nseg, skip (as entry 7). Returns 4 bytes:
// segments (u16), capture pointer (u16). 0xFFFF pointer = capture didn't complete.
int capfft_cmd(u32 len, const u8 *p, struct resp *r) {
  u32 polls, v = capture(&polls);
  if (v < CAP_WORDS * 4) {
    r->buf[0] = 0; r->buf[1] = 0; r->buf[2] = v >> 8; r->buf[3] = v; r->len = 4;
    return 3;
  }
  int st = fft_cmd(len, p, r);
  r->buf[2] = v >> 8; r->buf[3] = v; r->len = 4;
  return st;
}
