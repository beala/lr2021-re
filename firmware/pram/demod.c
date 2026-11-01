// On-chip demodulation (docs/pram-demod.md). The radio streams raw I/Q (source 0, int8 I/Q at
// 250 kS/s with the 62.5 kHz LoRa bandwidth setting, packet type 0x0B) into the RX FIFO, which is
// placed in free RAM, and this code demodulates it as it arrives. The core (afsk_rx.h) is an
// AFSK 1200 / APRS receiver.
//
// Reading the FIFO from the CPU [HW]: the read pointer belongs to the SPI block and isn't visible,
// so the CPU can't consume bytes the way ReadRxFifo does, and a full FIFO stops. But:
//   0xF30028  FIFO base, as an offset from 0x800000
//   0xF30030  FIFO size: the FIFO is full at size + 4 bytes
//   0xF30038  bytes written to RAM since the last clear
//   0xF3001C  bit 2: clear (ClearRxFifo, 0x6478, does this among other things)
//   0xF30004  flags, write 1 to clear: bit 3 full, bit 4 overflow (the next sample after full)
//   0xF30008  which flags interrupt the CPU (IRQ 42); 0xF30010 = flags & 0xF30008
// Writing a new base and then setting the clear bit restarts the FIFO at the new base without
// losing a sample: the ~24 bytes still in flight land at the new base. So two buffers can be used
// in turn, and the full one is demodulated while the other fills.
//
// Two ways to run it:
//   entry 4  a blocking command that polls the FIFO for a given number of buffers;
//   entry 6  background: the FIFO size is set to one buffer, its "full" flag interrupts the CPU,
//            and the handler (isr42 in demod_hdr.S -> bg_isr) switches buffers and demodulates.
//            The command returns at once; a good frame raises RxDone on the host's IRQ lines.
#include "afsk_rx.h"
struct resp { u32 len; u8 *buf; };

#define FIFO_BASE (*(volatile u32 *)0xF30028)
#define FIFO_SIZE (*(volatile u32 *)0xF30030)
#define FIFO_RAM  (*(volatile u32 *)0xF30038)
#define FIFO_CTL  (*(volatile u32 *)0xF3001C)
#define FIFO_FLAG (*(volatile u32 *)0xF30004)
#define FIFO_CPU  (*(volatile u32 *)0xF30008)
#define FIFO_STAT (*(volatile u32 *)0xF30010)
#define FIFO_FULL 0x08u
#define FIFO_OVF  0x10u

#define BUF_A     0x807800u                    // free RAM: two FIFO buffers
#define BUF_B     0x807A00u
#define BUF_ROOM  0x200u
#define ARX_ST    ((struct arx *)0x807C00)     // receiver state, then the driver state
#define ARX_OUT   ((u8 *)0x101A00)             // frame output (PRAM after the code; the build checks)
#define ARX_OUTN  0x200
#define VEC       ((volatile u32 *)0x101C00)   // copy of the vector table (1 KB aligned)
#define NVEC      52
struct drv { u32 cur, oth, t, bufs, maxl, overruns, isr_max, running; };
#define DRV       ((struct drv *)(0x807C00 + ((sizeof(struct arx) + 3) & ~3u)))
typedef char state_fits[(((sizeof(struct arx) + 3) & ~3u) + sizeof(struct drv) <= 0x400) ? 1 : -1];

#define EV_PENDING   (*(volatile u32 *)0x806044)   // ROM main loop events (gp + 0x44)
#define ROM_HOST_IRQ ((void (*)(u32))0x6ADC)       // raise host IRQs (masked by the DIO config)
#define IRQ_RX_DONE  (1u << 18)

static void put32(u8 *b, u32 v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static u32 be32p(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline u32 lr_aux(u32 a) { u32 v; __asm__ volatile("lr %0,[%1]" : "=r"(v) : "r"(a)); return v; }
static inline void sr_aux(u32 a, u32 v) { __asm__ volatile("sr %0,[%1]" :: "r"(v), "r"(a) : "memory"); }
#define CLRI(st) do { u32 _s; __asm__ volatile("clri %0" : "=r"(_s) :: "memory"); st = _s; } while (0)
#define SETI(st) do { u32 _s = st; __asm__ volatile("seti %0" :: "r"(_s) : "memory"); } while (0)

// 1: peek. Params: addr (4), nwords (1, <= 16). Returns the words, little-endian.
int peek_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 5) return 1;
  u32 a = be32p(p), n = p[4] > 16 ? 16 : p[4];
  for (u32 i = 0; i < n; i++) put32(r->buf + 4 * i, *(volatile u32 *)(a + 4 * i));
  r->len = 4 * n;
  return 3;
}

// 3: poke. Params: addr (4), value (4). Returns the value read back.
int poke_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 8) return 1;
  volatile u32 *a = (volatile u32 *)be32p(p);
  *a = be32p(p + 4);
  put32(r->buf, *a);
  r->len = 4;
  return 3;
}

static void stats(struct resp *r, u32 extra) {
  struct arx *s = ARX_ST;
  struct drv *d = DRV;
  u32 v[11] = { d->bufs, d->maxl, extra, d->isr_max, d->overruns, s->nsamp, s->nflags, s->nframes, s->ncrc, s->nlost, s->out_len };
  for (u32 i = 0; i < 11; i++) put32(r->buf + 4 * i, v[i]);
  r->len = 44;
}

static void start(u32 t, u32 dbg) {
  struct arx *s = ARX_ST;
  struct drv *d = DRV;
  arx_init(s, 8, ARX_OUT, ARX_OUTN);
  s->dbg = dbg;
  d->cur = BUF_A; d->oth = BUF_B; d->t = t;
  d->bufs = d->maxl = d->overruns = d->isr_max = 0;
  FIFO_BASE = BUF_A - 0x800000; FIFO_CTL |= 4;
}

// The full buffer: switch the FIFO to the other one first, then demodulate.
static void take(struct drv *d, struct arx *s) {
  u32 n = FIFO_RAM;                      // bytes already in RAM; the ones in flight go to the next buffer
  FIFO_BASE = d->oth - 0x800000; FIFO_CTL |= 4;
  if (n > d->maxl) d->maxl = n;
  if (n > BUF_ROOM) { d->overruns++; n = BUF_ROOM; }
  d->bufs++;
  arx_block(s, (const s8 *)d->cur, n >> 1);
  u32 x = d->cur; d->cur = d->oth; d->oth = x;
}

// 4: APRS receiver, blocking. Params: buffers (2), T (2), flags (1):
//   bit0  start: reset the receiver state, point the FIFO at buffer A and clear it
//   bit1  debug: store the decimated I/Q (int8, >> 3) in the output area instead of frames
//   bit2  empty the frame output first
//   bit3  return as soon as a frame is in the output
// The host streams the FIFO with a size of at most BUF_ROOM - 28. Without bit0 the FIFO keeps
// filling between calls, so a host that calls again before it fills loses nothing.
// Response (LE words): buffers, max bytes at a switch, timeout, 0, overruns, then the core's
// samples, flags, frames, bad FCS, frames lost, output bytes.
int aprs_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 5) return 1;
  u32 nbuf = (p[0] << 8) | p[1], t = (p[2] << 8) | p[3], fl = p[4], tmo = 0;
  struct arx *s = ARX_ST;
  struct drv *d = DRV;
  volatile u32 st;
  if (d->running) return 1;              // the background receiver owns the FIFO
  CLRI(st);
  if (fl & 1) start(t, (fl >> 1) & 1);
  s->dbg = (fl >> 1) & 1;
  if (fl & 4) s->out_len = 0;
  for (u32 b = 0; b < nbuf; b++) {
    u32 k = 0;
    while (FIFO_RAM < t) if (++k > 1000000) { tmo = 1; goto out; }
    take(d, s);
    if ((fl & 8) && s->out_len && !s->dbg) break;
  }
out:
  SETI(st);
  stats(r, tmo);
  return 3;
}

// IRQ 42 in the background mode (called from isr42, demod_hdr.S, which saves the registers).
void bg_isr(void) {
  u32 f = FIFO_STAT;
  if (f & (FIFO_FULL | FIFO_OVF)) {
    struct drv *d = DRV;
    struct arx *s = ARX_ST;
    u32 out0 = s->out_len, t0 = FIFO_RAM;
    take(d, s);
    FIFO_FLAG = FIFO_FULL | FIFO_OVF;     // after the clear, so they don't come straight back
    if (FIFO_RAM > d->isr_max) d->isr_max = FIFO_RAM;  // bytes into the next buffer when done
    (void)t0;
    if (s->out_len != out0 && !s->dbg) ROM_HOST_IRQ(IRQ_RX_DONE);
  }
  f &= ~(FIFO_FULL | FIFO_OVF);
  if (f) {                               // what the ROM's own handler (0x78C4) does
    FIFO_FLAG = f;
    if (f & (1u << 24)) { *(volatile u32 *)0xF3000C &= ~(1u << 24); EV_PENDING |= 1u << 25; }
    if (f & (1u << 16)) EV_PENDING |= 1u << 23;
  }
}

extern void isr42(void);

// 6: APRS receiver in the background. Params: op (1), then for op 1: T (2), flags (1, bit1 debug).
//   op 1  start: FIFO size T at buffer A, IRQ 42 on "full" through a copy of the vector table with
//         isr42 in slot 42 (priority 1, so it runs on the normal stack), and return
//   op 0  stop: back to the ROM's vector table
//   op 2  status
//   op 3  n (2): drop the first n bytes of the frame output (the frames the host has read; any
//         decoded since stay)
// The host streams the FIFO first (packet type 0x0B, source 0 with FIFO on, SetRx). Response: as
// entry 4, with the word "timeout" replaced by INT_VECTOR_BASE.
int bg_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 1) return 1;
  struct drv *d = DRV;
  volatile u32 st;
  u32 op = p[0];
  CLRI(st);
  if (op == 1 && len >= 4) {
    u32 t = (p[1] << 8) | p[2];
    if (t < 64 || t + 28 > BUF_ROOM) { SETI(st); return 1; }
    for (u32 i = 0; i < NVEC; i++) VEC[i] = *(volatile u32 *)(4 * i);
    VEC[42] = (u32)isr42;
    start(t, (p[3] >> 1) & 1);
    FIFO_SIZE = t;
    FIFO_FLAG = FIFO_FULL | FIFO_OVF;
    sr_aux(0x40B, 42); sr_aux(0x206, 1);   // IRQ_SELECT, IRQ_PRIORITY
    sr_aux(0x25, (u32)VEC);                // INT_VECTOR_BASE
    FIFO_CPU |= FIFO_FULL;
    d->running = 1;
  } else if (op == 0) {
    FIFO_CPU &= ~FIFO_FULL;
    sr_aux(0x25, 0);
    sr_aux(0x40B, 42); sr_aux(0x206, 0);
    d->running = 0;
  } else if (op == 3 && len >= 3) {      // drop the first n bytes of output (what the host has read)
    struct arx *s = ARX_ST;
    u32 n = (p[1] << 8) | p[2], i;
    if (n >= s->out_len) n = s->out_len;
    for (i = 0; i + n < s->out_len; i++) ARX_OUT[i] = ARX_OUT[i + n];
    s->out_len -= n;
  }
  SETI(st);
  stats(r, lr_aux(0x25));
  return 3;
}
