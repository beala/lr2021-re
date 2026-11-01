// AFSK 1200 (Bell 202, APRS) receiver core: integer only, no library calls, so the same code runs
// in PRAM on the LR2021 and on a PC (test_afsk_rx.c). Input: blocks of int8 I/Q pairs. Output:
// AX.25 frames whose FCS checks, written as {u16 length (LE), bytes without the FCS}.
//
//   decimate by `dec` to 31.25 kS/s (sum of dec pairs)
//   FM discriminator: cross product of consecutive samples, scaled by a peak-following shift
//   mark / space detection: sliding correlation with 1200 and 2200 Hz over one bit (26 samples)
//   bit clock: phase accumulator nudged toward each tone transition
//   NRZI (same tone = 1), HDLC flags, bit unstuffing, CRC-16/X.25
#ifndef AFSK_RX_H
#define AFSK_RX_H

typedef unsigned int u32;
typedef signed char s8;
typedef unsigned char u8;

#define ARX_FS     31250u
#define ARX_N      26                     /* correlator window, ~1 bit */
#define ARX_MAXF   336                    /* longest frame kept, bytes with FCS */
#define ARX_INC_M  164926744u             /* 1200 / 31250 * 2^32 (also the bit clock) */
#define ARX_INC_S  302365696u             /* 2200 / 31250 * 2^32 */

struct arx {
  u32 dec, dcnt;
  int si, sq, pi, pq;                     /* decimator sums, previous decimated sample */
  u32 peak, shift;                        /* discriminator scaling */
  u32 phm, phs, ri;                       /* oscillator phases, ring index */
  int amc, ams, asc, ass;                 /* sliding sums: mark cos/sin, space cos/sin */
  int pll;
  u32 tone, prev_tone, last_bit_tone;
  u32 acc, ones, nbits, len, in_frame, cur;
  u32 nsamp, nflags, nframes, ncrc, nlost; /* stats: decimated samples, flags, good frames, bad FCS, frames not stored */
  u8 *out; u32 out_len, out_max, dbg;    /* dbg: store decimated I/Q (>> 3, int8) in out instead */
  int rmc[ARX_N], rms[ARX_N], rsc[ARX_N], rss[ARX_N];
  u8 frame[ARX_MAXF];
};

static const s8 ARX_SINE[256] = {
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

static void arx_init(struct arx *s, u32 dec, u8 *out, u32 out_max) {
  u8 *b = (u8 *)s;
  for (u32 i = 0; i < sizeof *s; i++) b[i] = 0;
  s->dec = dec; s->out = out; s->out_max = out_max; s->peak = 1;
}

static void arx_frame_end(struct arx *s) {
  u32 crc = 0xFFFF;
  for (u32 i = 0; i < s->len; i++) {
    crc ^= s->frame[i];
    for (u32 k = 0; k < 8; k++) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
  }
  if (crc != 0xF0B8) { s->ncrc++; return; }
  s->nframes++;
  u32 n = s->len - 2;
  if (s->out_len + 2 + n > s->out_max) { s->nlost++; return; }
  u8 *o = s->out + s->out_len;
  o[0] = n; o[1] = n >> 8;
  for (u32 i = 0; i < n; i++) o[2 + i] = s->frame[i];
  s->out_len += 2 + n;
}

static void arx_bit(struct arx *s, u32 bit) {
  s->acc = ((s->acc >> 1) | (bit << 7)) & 0xFF;
  if (s->acc == 0x7E) {
    if (s->in_frame && s->nbits == 7 && s->len >= 18) arx_frame_end(s);
    s->nflags++;
    s->in_frame = 1; s->len = 0; s->nbits = 0; s->ones = 0;
    return;
  }
  if (bit) {
    if (++s->ones >= 7) s->in_frame = 0;            /* abort */
  } else {
    if (s->ones == 5) { s->ones = 0; return; }      /* stuffed zero */
    s->ones = 0;
  }
  if (!s->in_frame) return;
  s->cur = (s->cur >> 1) | (bit << 7);
  if (++s->nbits == 8) {
    if (s->len < ARX_MAXF) s->frame[s->len++] = s->cur;
    else s->in_frame = 0;
    s->nbits = 0;
  }
}

static inline int arx_abs(int v) { return v < 0 ? -v : v; }
static inline int arx_mag(int a, int b) {           /* |a + jb|, max + min / 2 */
  a = arx_abs(a); b = arx_abs(b);
  return a > b ? a + (b >> 1) : b + (a >> 1);
}

static void arx_sample(struct arx *s, int i, int q) {
  if (s->dbg && s->out_len + 2 <= s->out_max) { s->out[s->out_len++] = i >> 3; s->out[s->out_len++] = q >> 3; }
  int cross = q * s->pi - i * s->pq;
  s->pi = i; s->pq = q;
  u32 a = (u32)arx_abs(cross);
  if (a > s->peak) s->peak = a; else s->peak -= s->peak >> 9;
  if ((s->peak >> s->shift) > 511) s->shift++;
  else if (s->shift && (s->peak >> s->shift) < 128) s->shift--;
  int d = cross >> s->shift;

  int mc = d * ARX_SINE[((s->phm >> 24) + 64) & 255], ms = d * ARX_SINE[s->phm >> 24];
  int sc = d * ARX_SINE[((s->phs >> 24) + 64) & 255], ss = d * ARX_SINE[s->phs >> 24];
  s->phm += ARX_INC_M; s->phs += ARX_INC_S;
  u32 r = s->ri;
  s->amc += mc - s->rmc[r]; s->rmc[r] = mc;
  s->ams += ms - s->rms[r]; s->rms[r] = ms;
  s->asc += sc - s->rsc[r]; s->rsc[r] = sc;
  s->ass += ss - s->rss[r]; s->rss[r] = ss;
  s->ri = r + 1 == ARX_N ? 0 : r + 1;
  s->nsamp++;

  u32 tone = arx_mag(s->amc, s->ams) > arx_mag(s->asc, s->ass);
  if (tone != s->prev_tone) { s->pll -= s->pll >> 2; s->prev_tone = tone; }
  int prev = s->pll;
  s->pll = (int)((u32)s->pll + ARX_INC_M);
  if (prev >= 0 && s->pll < 0) {
    arx_bit(s, tone == s->last_bit_tone);
    s->last_bit_tone = tone;
  }
}

static void arx_block(struct arx *s, const s8 *iq, u32 pairs) {
  u32 dec = s->dec, c = s->dcnt;
  int si = s->si, sq = s->sq;
  for (u32 k = 0; k < pairs; k++) {
    si += iq[2 * k]; sq += iq[2 * k + 1];
    if (++c == dec) { arx_sample(s, si, sq); si = sq = 0; c = 0; }
  }
  s->si = si; s->sq = sq; s->dcnt = c;
}
#endif
