// Host build of ray.c for testing (like test_afsk_rx.c): the synthesizer writes go to stdout as
// (word, cycles) pairs instead of the radio.
//   gcc -O2 -DHOST_SIM -o ray_sim ray_sim.c && ./ray_sim x y ang keys frames [title] > log.bin
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ray.c"
struct player PL[1];
u16 HQ[W];
u8 AT[W];
u8 PTS[2 * MAXP];
// On the chip each emit also costs ~31 cycles of loop and register writes; model that too.
void emit(u32 word, u32 cycles) { u32 v[2] = { word, cycles + 31 }; fwrite(v, 4, 2, stdout); }
int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: ray_sim x y ang keys frames [title]\n"); return 1; }
  u8 o[10];
  struct resp r = { 0, o };
  u32 x = strtoul(argv[1], 0, 0), y = strtoul(argv[2], 0, 0), a = strtoul(argv[3], 0, 0);
  u8 p0[7] = { 0, x >> 8, x, y >> 8, y, a >> 8, a };
  ray_cmd(7, p0, &r);
  u32 dwell = 2560, step = 1049, n = strtoul(argv[5], 0, 0);   // 80 us, 1 kHz (as tools/raycaster.py)
  u8 p1[11] = { argc > 6 ? 2 : 1, strtoul(argv[4], 0, 0), n, dwell >> 24, dwell >> 16, dwell >> 8, dwell,
                step >> 24, step >> 16, step >> 8, step };
  if (ray_cmd(11, p1, &r) != 3) return 1;
  fprintf(stderr, "x %d y %d ang %d frames %d points %d\n", (o[0] << 8) | o[1], (o[2] << 8) | o[3],
          (o[4] << 8) | o[5], (o[6] << 8) | o[7], (o[8] << 8) | o[9]);
  return 0;
}
