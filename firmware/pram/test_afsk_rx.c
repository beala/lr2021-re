// PC test of afsk_rx.h: feed an int8 I/Q file through the same core the chip runs.
//   cc -O2 -o /tmp/t test_afsk_rx.c && /tmp/t capture.iq8 DEC [block_pairs]
#include <stdio.h>
#include <stdlib.h>
#include "afsk_rx.h"

static void show(const u8 *f, unsigned n) {
  char a[16];
  for (int k = 1; k >= 0; k--) {          /* source, then destination: shifted ASCII, SSID in byte 6 */
    int o = 7 * k, j = 0;
    for (int i = 0; i < 6; i++) if ((f[o + i] >> 1) != ' ') a[j++] = f[o + i] >> 1;
    int ssid = (f[o + 6] >> 1) & 15;
    if (ssid) j += sprintf(a + j, "-%d", ssid);
    a[j] = 0;
    printf("%s%s", a, k ? ">" : "");
  }
  unsigned i = 13;
  while (i < n && !(f[i] & 1)) i += 7;    /* skip any digipeater addresses */
  i += 1 + 2;                             /* last address byte, control, PID */
  printf(":%.*s\n", i < n ? (int)(n - i) : 0, (const char *)f + i);
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s file.iq8 dec [block]\n", argv[0]); return 1; }
  FILE *fp = fopen(argv[1], "rb");
  if (!fp) { perror(argv[1]); return 1; }
  unsigned dec = atoi(argv[2]), blk = argc > 3 ? atoi(argv[3]) : 240;
  static struct arx s; static u8 out[4096]; static s8 buf[1 << 16];
  arx_init(&s, dec, out, sizeof out);
  size_t got;
  while ((got = fread(buf, 2, blk, fp)) > 0) arx_block(&s, buf, got);
  printf("samples %u flags %u frames %u bad_fcs %u lost %u\n", s.nsamp, s.nflags, s.nframes, s.ncrc, s.nlost);
  for (unsigned o = 0; o < s.out_len;) { unsigned n = out[o] | out[o + 1] << 8; show(out + o + 2, n); o += 2 + n; }
  return 0;
}
