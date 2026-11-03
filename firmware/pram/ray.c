// A first-person raycaster that runs on the LR2021's own CPU and sends each frame over its CW
// carrier as vector graphics (docs/pram-raycaster.md).
//
// The map (16 x 16 cells, in this blob) and the player (in the chip's free RAM) are the chip's.
// Each call takes the keys held down, then for each frame:
//   moves and turns the player, with wall collision;
//   casts one ray per half screen unit (160 rays, DDA on the grid, fixed point, hardware divide);
//   turns the columns into a vector display list: each run of one flat wall face becomes a quad
//   (left edge, top, right edge, bottom), the beam moving between quads with the pen up;
//   sends it by hopping the synthesizer (0xF40144) while the radio sends CW (SetTxTestMode 0x020E
//   02), as the waterfall text blob does:
//
//     SYNC (while rendering) | MARK | [PEN] X Y [PEN] X Y ... | MARK | SYNC
//
//   Each slot is `dwell` cycles at base + level x step. Levels: X = x - 80 (x = 0-160 half
//   units across), Y = y (-62..62 half units, up is +), PEN = -130 (the next point is a move, beam
//   off), SYNC = -220, MARK = +220. The receiver FM-demodulates and feeds the points to a simulated
//   XY oscilloscope: the beam draws what the chip sent.
//
// Entry 1 params (BE): op (1)
//   op 0  place the player: x (2), y (2) (8.8 fixed point, cells), angle (2, 0-1023 = 360 deg);
//         also takes the carrier frequency, so send it after SetTxTestMode
//   op 1  play: keys (1: bit 0 forward, 1 back, 2 turn left, 3 turn right, 4 strafe left, 5 strafe
//         right), frames (1), dwell (4, CPU cycles), step (4, synthesizer word units)
//   op 2  title screen (ray_title.h, from gen_title.py): as op 1, keys ignored
// Response: x (2), y (2), angle (2), frames done (2), points in the last frame (2).
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;
struct resp { u32 len; u8 *buf; };

#define MW 16                               // map is MW x MW cells
#define W 160                               // rays: one per half screen unit (screen is 80 wide)
#define HQ_SCALE (64 * 256 * 16)            // wall half height, sixteenths of a half unit: HQ_SCALE / perp
#define YMAX 62                             // half units
#define FOV 10813                           // camera plane length, Q14 (0.66: ~66 deg)
#define SPEED 22                            // per frame, 8.8 cells
#define TURN 10                             // per frame, 1024 = 360 deg
#define MARGIN 64                           // keep this far from walls, 8.8 cells
#define MAXP 150                            // display list points per frame
#define L_PEN (-130)
#define L_SYNC (-220)
#define L_MARK 220

struct player { s32 x, y; u32 ang; u32 placed; u32 base; };   // base: the carrier's synthesizer word

// 16 x 16 cells, loosely after the start of E1M1 (Hangar): the start room with its computer
// panels, the corridor east, the zigzag room and the nukage room beyond. 0 empty, 1 grey tech
// wall, 2 brown, 3 computer panels, 4 door.
static const u8 MAP[MW * MW] = {
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,1,1,0,0,1,1,1,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,1,0,0,0,0,2,2,1,
  1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,
  1,0,0,3,3,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,3,3,0,0,0,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,1,0,0,2,2,0,0,1,
  1,1,1,1,4,0,1,1,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,3,3,3,3,3,0,1,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

#ifdef HOST_SIM
extern struct player PL[1];
extern u16 HQ[W];
extern u8 AT[W];
extern u8 PTS[2 * MAXP];
void emit(u32 word, u32 cycles);
static u32 pll_base(void) { return 0; }
static void irq_off(u32 *st) { (void)st; }
static void irq_on(u32 st) { (void)st; }
#else
#define PLL_FREQ (*(volatile u32 *)0xF40144)
#define PL ((struct player *)0x807900)
#define HQ ((u16 *)0x807920)                // W wall half heights (sixteenths of a half unit)
#define AT ((u8 *)0x807A60)                 // W attributes: wall type x 2 + side
#define PTS ((u8 *)0x807B00)                // the display list: MAXP points, x, (y + 64) | pen << 7
static inline void delay(u32 n) {
  if (!n) return;
  __asm__ volatile("mov lp_count,%0\n\tlp 1f\n\tnop_s\n1:" :: "r"(n) : "lp_count", "memory");
}
static inline void emit(u32 word, u32 cycles) { PLL_FREQ = word; delay(cycles); }
static u32 pll_base(void) { return PLL_FREQ; }
static void irq_off(u32 *st) { __asm__ volatile("clri %0" : "=r"(*st) :: "memory"); }
static void irq_on(u32 st) { __asm__ volatile("seti %0" :: "r"(st) : "memory"); }
#endif

#include "ray_sin.h"
#include "ray_title.h"

static s32 sin_(u32 a) {
  a &= 1023;
  if (a < 256) return SIN_Q[a];
  if (a < 512) return SIN_Q[512 - a];
  if (a < 768) return -SIN_Q[a - 512];
  return -SIN_Q[1024 - a];
}
static s32 cos_(u32 a) { return sin_(a + 256); }

static u32 cell(s32 x, s32 y) {
  if (x < 0 || y < 0 || x >= MW * 256 || y >= MW * 256) return 1;
  return MAP[(y >> 8) * MW + (x >> 8)];
}

// Move by (dx, dy) (8.8), sliding along walls: each axis on its own, MARGIN from the wall.
static void move(struct player *p, s32 dx, s32 dy) {
  s32 mx = dx > 0 ? MARGIN : -MARGIN, my = dy > 0 ? MARGIN : -MARGIN;
  if (dx && !cell(p->x + dx + mx, p->y)) p->x += dx;
  if (dy && !cell(p->x, p->y + dy + my)) p->y += dy;
}

static void step_player(struct player *p, u32 keys) {
  if (keys & 4) p->ang = (p->ang - TURN) & 1023;
  if (keys & 8) p->ang = (p->ang + TURN) & 1023;
  s32 c = cos_(p->ang), s = sin_(p->ang);
  s32 f = (keys & 1 ? SPEED : 0) - (keys & 2 ? SPEED : 0);
  s32 t = (keys & 32 ? SPEED : 0) - (keys & 16 ? SPEED : 0);
  if (f || t) move(p, (c * f - s * t) >> 14, (s * f + c * t) >> 14);
}

// One ray per column (Lode Vandevenne's DDA, in fixed point): HQ = wall half height, AT = attribute.
static void render(const struct player *p) {
  s32 dx = cos_(p->ang), dy = sin_(p->ang);
  s32 plx = (-dy * FOV) >> 14, ply = (dx * FOV) >> 14;
  for (u32 x = 0; x < W; x++) {
    s32 cx = (s32)((2 * x + 1) * 16384 / W) - 16384;             // -1 .. 1, Q14
    s32 rx = dx + ((plx * cx) >> 14), ry = dy + ((ply * cx) >> 14);
    u32 ax = rx < 0 ? -rx : rx, ay = ry < 0 ? -ry : ry;
    u32 ddx = ax > 4 ? (1u << 22) / ax : 1u << 20;              // |1 / ray|, 8.8 cells
    u32 ddy = ay > 4 ? (1u << 22) / ay : 1u << 20;
    s32 mx = p->x >> 8, my = p->y >> 8, sx, sy;
    u32 fx = p->x & 255, fy = p->y & 255, sdx, sdy;
    if (rx < 0) { sx = -1; sdx = (fx * ddx) >> 8; } else { sx = 1; sdx = ((256 - fx) * ddx) >> 8; }
    if (ry < 0) { sy = -1; sdy = (fy * ddy) >> 8; } else { sy = 1; sdy = ((256 - fy) * ddy) >> 8; }
    u32 side = 0, c = 1;
    for (u32 k = 0; k < 2 * MW; k++) {
      if (sdx < sdy) { sdx += ddx; mx += sx; side = 0; } else { sdy += ddy; my += sy; side = 1; }
      if (mx < 0 || my < 0 || mx >= MW || my >= MW) { c = 1; break; }
      c = MAP[my * MW + mx];
      if (c) break;
    }
    u32 perp = side ? sdy - ddy : sdx - ddx;                     // 8.8 cells, along the view
    if (perp < 16) perp = 16;
    u32 h = HQ_SCALE / perp;
    HQ[x] = h > 16 * YMAX + 15 ? 16 * YMAX + 15 : h;
    AT[x] = ((c & 7) << 1) | side;
  }
}

static s32 iabs(s32 v) { return v < 0 ? -v : v; }

static u32 add_pt(u32 n, s32 x, s32 hq, u32 pen, s32 sign) {
  s32 y = (hq + 8) >> 4;
  if (y > YMAX) y = YMAX;
  PTS[2 * n] = x;
  PTS[2 * n + 1] = (sign * y + 64) | (pen << 7);
  return n + 1;
}

// The display list: one quad per run of columns showing one flat wall face. A flat wall's height
// changes by the same amount from column to column, so a run ends where the attribute (wall type,
// side) changes or that rate breaks (a corner, or a nearer wall in front). Corner heights come
// from the run's end columns extrapolated half a column to the edge. Returns the number of points.
static u32 vectorize(void) {
  u32 n = 0, x0 = 0;
  for (u32 x = 1; x <= W && n + 5 <= MAXP; x++) {
    if (x < W && AT[x] == AT[x - 1]) {
      s32 d = (s32)HQ[x] - (s32)HQ[x - 1];
      if (x - x0 < 2) {                     // the run's second column: only a big jump ends it
        if (iabs(d) <= (s32)HQ[x - 1] / 4 + 32) continue;
      } else {                              // later: the rate of change must hold
        s32 dd = d - ((s32)HQ[x - 1] - (s32)HQ[x - 2]);
        if (iabs(dd) <= (s32)HQ[x - 1] / 32 + 24) continue;
      }
    }
    s32 h0 = HQ[x0], h1 = HQ[x - 1];
    if (x - x0 >= 2) {                      // extrapolate to the column edges
      h0 -= ((s32)HQ[x0 + 1] - (s32)HQ[x0]) / 2;
      h1 += ((s32)HQ[x - 1] - (s32)HQ[x - 2]) / 2;
    }
    if (h0 < 0) h0 = 0;
    if (h1 < 0) h1 = 0;
    n = add_pt(n, x0, h0, 1, -1);           // move to the bottom left, beam off
    n = add_pt(n, x0, h0, 0, 1);            // up the left edge
    n = add_pt(n, x, h1, 0, 1);             // along the top
    n = add_pt(n, x, h1, 0, -1);            // down the right edge
    n = add_pt(n, x0, h0, 0, -1);           // back along the bottom
    x0 = x;
  }
  return n;
}

// One frame: SYNC (at least 4 slots), MARK, the points ([PEN] X Y each), MARK.
static void send(u32 base, u32 step, u32 dwell, const u8 *pts, u32 npts) {
  emit(base + L_SYNC * step, 4 * dwell);
  emit(base + L_MARK * step, dwell);
  for (u32 i = 0; i < npts; i++) {
    u32 yb = pts[2 * i + 1];
    if (yb & 0x80) emit(base + L_PEN * step, dwell);
    emit(base + ((s32)pts[2 * i] - 80) * step, dwell);
    emit(base + ((s32)(yb & 0x7F) - 64) * step, dwell);
  }
  emit(base + L_MARK * step, dwell);
}

static void be16(u8 *o, u32 v) { o[0] = v >> 8; o[1] = v; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

int ray_cmd(u32 len, const u8 *p, struct resp *r) {
  struct player *pl = PL;
  if (len < 1) return 1;
  u32 frames = 0, npts = 0;
  if (p[0] == 0) {
    if (len < 7) return 1;
    pl->x = (p[1] << 8) | p[2]; pl->y = (p[3] << 8) | p[4]; pl->ang = ((p[5] << 8) | p[6]) & 1023;
    pl->placed = 1;
    pl->base = pll_base();                  // call after the CW carrier is on: later calls leave it at SYNC
  } else if (p[0] == 1 || p[0] == 2) {
    if (len < 11 || !pl->placed) return 1;
    u32 keys = p[1], n = p[2], dwell = be32(p + 3), step = be32(p + 7), st;
    irq_off(&st);
    for (; frames < n; frames++) {
      if (p[0] == 2) {
        npts = TITLE_N;
        send(pl->base, step, dwell, TITLE, TITLE_N);
        continue;
      }
      emit(pl->base + L_SYNC * step, 0);    // SYNC while rendering
      step_player(pl, keys);
      render(pl);
      npts = vectorize();
      send(pl->base, step, dwell, PTS, npts);
    }
    emit(pl->base + L_SYNC * step, 0);      // the carrier waits at SYNC until the next call
    irq_on(st);
  } else {
    return 1;
  }
  be16(r->buf, pl->x); be16(r->buf + 2, pl->y); be16(r->buf + 4, pl->ang); be16(r->buf + 6, frames);
  be16(r->buf + 8, npts);
  r->len = 10;
  return 3;
}
