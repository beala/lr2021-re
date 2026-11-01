// A simple MeshCore repeater that runs on the LR2021's own CPU (docs/pram-repeater.md).
// Built with selfspi.c as mesh.bin (build_mesh.sh).
//
// It runs in the background from the ROM's radio state handler slot (gp+0x34), like the channel
// scanner (scan.c): the ROM main loop hands every radio event to rpt_state, which first passes it
// to the handler the ROM installed, then looks at the IRQ status and takes the next step itself.
//
//   receive (continuous) -> RxDone, CRC good: parse the MeshCore header
//     flood (route 1), payload types 0-8, not seen before, under the hop limit:
//       append our hash to the path, wait rand(0 .. 5 t), t = airtime x flood factor
//     direct (route 2), we're the next hop, not seen before:
//       remove our hash from the path, wait rand(0 .. 5 t), t = airtime x direct factor
//   wait: an RX with the radio's timeout (32.768 kHz ticks); StopTimeoutOnPreamble lets a packet
//     that starts meanwhile finish (then the wait starts again)
//   timeout: write the packet into the TX FIFO through the chip's own SPI pads (fifo_write in selfspi.c:
//     the MCU must leave NSS/SCK/MOSI floating), SetTx
//   TxDone: back to continuous RX
//
// Dedup: a 32-entry table of FNV-1a hashes over payload type (+ path_len for TRACE) and payload,
// as MeshCore's packet hash (which is SHA-256 over the same fields). Transport-code routes (0 and 3)
// and TRACE aren't forwarded. One packet waits at a time; others that need forwarding meanwhile are
// counted as busy and dropped.
typedef unsigned int u32;
typedef int s32;
typedef unsigned char u8;

struct resp { u32 len; u8 *buf; };
typedef int (*handler_t)(u32 len, const u8 *p, struct resp *r);
#define H_SET_RX      ((handler_t)0xFCFC)   // 0x020C
#define H_SET_TX      ((handler_t)0x101C8)  // 0x020D
#define H_SET_FS      ((handler_t)0xEB40)   // 0x0129
#define H_LORA_PKT    ((handler_t)0xF274)   // 0x0221
#define H_CLR_RX      ((handler_t)0x4034)   // 0x011E ClearRxFifo
#define H_CLR_TX      ((handler_t)0x4054)   // 0x011F ClearTxFifo
#define H_PKT_LEN     ((handler_t)0x5CA8)   // 0x0212 GetRxPktLength (2 bytes BE)
#define H_STOP_TO_PRE ((handler_t)0x1149C)  // 0x0209 StopTimeoutOnPreamble
u32 fifo_write(const u8 *d, u32 n, u32 hb);
void pads_idle(u32 *saved);
void pads_restore(const u32 *saved);

#define IRQ (*(volatile u32 *)0xF2A00C)     // IRQ status, write 1 to clear
#define IRQ_PRE     (1u << 5)
#define IRQ_HDR     (1u << 6)
#define IRQ_HDR_ERR (1u << 9)
#define IRQ_RX_DONE (1u << 18)
#define IRQ_TX_DONE (1u << 19)
#define IRQ_TIMEOUT (1u << 21)
#define IRQ_CRC     (1u << 22)
#define IRQ_ALL     (IRQ_PRE | IRQ_HDR | IRQ_HDR_ERR | IRQ_RX_DONE | IRQ_TX_DONE | IRQ_TIMEOUT | IRQ_CRC)
#define IRQ_GATE    (*(volatile u32 *)0x80060C)   // applied by the ROM before the status
#define RXF_BASE    (*(volatile u32 *)0xF30028)   // RX FIFO base, offset from 0x800000
#define RXF_SIZE    (*(volatile u32 *)0xF30030)   // "full" at size + 4
// The ROM's RX FIFO (0x806140) is a 232-byte ring: a longer packet wraps over its own start. Move
// it to free RAM with room for 255 bytes.
#define RXF_RAM     0x807A00u
#define RXF_LEN     0x1FCu
#define GP 0x806000u
#define STATE_SLOT (*(volatile u32 *)(GP + 0x34))
#define RX_CONT 0xFFFFFFu

#define TXB  ((u8 *)0x807800)               // packet waiting to go out
#define RXB  ((u8 *)0x807900)               // last received packet
// 0x807A00-0x807BFF: RX FIFO (RXF_RAM)
#define SEEN ((u32 *)0x807F00)              // 32 hashes (after the state at 0x807E00)
#define NSEEN 32
enum { S_OFF, S_RX, S_TX };
struct rs {
  u32 running, orig, state, gate_saved;
  u8 id[4];
  u32 rx_pre, tx_pre, hb, sf, bwc, cr, ffac, dfac, maxhops;
  u32 pending, txlen, delay, rng, seen_pos;
  u32 rx, crc, hdr_err, fwd_flood, fwd_direct, dup, ignored, busy, txdone, spi_fail, events, last_len;
  u32 padcfg[3];                       // SPI pads as found at start
};
#define RS ((struct rs *)0x807E00)
#define RUNNING 0x4D455348u                 // RAM isn't cleared at load: a magic, not just nonzero

u32 rpt_state(u32 ev);

static void hook_in(struct rs *b) {
  u32 cur = STATE_SLOT;
  if (cur != (u32)rpt_state) { b->orig = cur; STATE_SLOT = (u32)rpt_state; }
}

static u32 rnd(struct rs *b) {
  u32 x = b->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return b->rng = x;
}

static void rx_start(struct rs *b, u32 ticks) {
  u8 t[4] = { 0, (u8)b->rx_pre, 255, 0x02 };   // explicit header, CRC on
  H_SET_FS(0, 0, 0);
  H_LORA_PKT(4, t, 0);
  RXF_BASE = RXF_RAM - 0x800000;
  RXF_SIZE = RXF_LEN;
  H_CLR_RX(0, 0, 0);
  IRQ = IRQ_ALL;
  t[0] = ticks >> 16; t[1] = ticks >> 8; t[2] = ticks;
  H_SET_RX(3, t, 0);
  b->state = S_RX;
  hook_in(b);
}

static void do_tx(struct rs *b) {
  u8 t[4] = { 0, (u8)b->tx_pre, (u8)b->txlen, 0x02 };
  b->pending = 0;
  H_SET_FS(0, 0, 0);
  H_CLR_TX(0, 0, 0);
  if (fifo_write(TXB, b->txlen, b->hb) != b->txlen) {   // the MCU is driving the SPI pins
    b->spi_fail++;
    H_CLR_TX(0, 0, 0);
    rx_start(b, RX_CONT);
    return;
  }
  H_LORA_PKT(4, t, 0);
  IRQ = IRQ_ALL;
  t[0] = t[1] = t[2] = 0;
  H_SET_TX(3, t, 0);
  b->state = S_TX;
  hook_in(b);
}

// Airtime in RX-timeout ticks (32.768 kHz): explicit header, CRC on, no LDRO.
static u32 airtime(struct rs *b, u32 len) {
  u32 sf = b->sf;
  s32 num = 8 * (s32)len - 4 * (s32)sf + 44;
  u32 k = num > 0 ? ((u32)num + 4 * sf - 1) / (4 * sf) : 0;
  u32 q4 = 4 * b->tx_pre + 17 + 4 * (8 + k * (b->cr + 4));       // quarter symbols
  u32 st16 = ((1u << sf) << 19) / (62500u << (b->bwc - 3));      // 16 x ticks per symbol
  return q4 * st16 / 64;
}

static u32 fnv(u32 h, const u8 *p, u32 n) {
  for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

// Seen before? Records it if not.
static u32 seen(struct rs *b, u32 h) {
  for (u32 i = 0; i < NSEEN; i++) if (SEEN[i] == h) return 1;
  SEEN[b->seen_pos++ % NSEEN] = h;
  return 0;
}

// Decide what to do with RXB[0..len). Sets pending / TXB / txlen / delay when it forwards.
static void handle(struct rs *b, u32 len) {
  const u8 *p = RXB;
  b->last_len = len;
  if (len < 3 || p[0] == 0xFF) { b->ignored++; return; }
  u32 route = p[0] & 3, ptype = (p[0] >> 2) & 15;
  if (route == 0 || route == 3 || ptype == 9) { b->ignored++; return; }   // transport codes, TRACE
  u32 pl = p[1], hsz = (pl >> 6) + 1, cnt = pl & 63;
  u32 pay = 2 + cnt * hsz;
  if (hsz == 4 || cnt * hsz > 64 || pay >= len) { b->ignored++; return; }
  u8 t = ptype;
  u32 h = fnv(fnv(2166136261u, &t, 1), p + pay, len - pay);
  if (!h) h = 1;
  b->rng ^= h;
  u32 factor;
  if (route == 1) {                                   // flood
    if (ptype > 8 || cnt + 1 > b->maxhops || (cnt + 1) * hsz > 64 || len + hsz > 255) { b->ignored++; return; }
    if (seen(b, h)) { b->dup++; return; }
    if (b->pending) { b->busy++; return; }
    for (u32 i = 0; i < pay; i++) TXB[i] = p[i];
    for (u32 i = 0; i < hsz; i++) TXB[pay + i] = b->id[i];
    for (u32 i = pay; i < len; i++) TXB[i + hsz] = p[i];
    TXB[1] = pl + 1;
    b->txlen = len + hsz;
    b->fwd_flood++;
    factor = b->ffac;
  } else {                                            // direct: are we the next hop?
    if (cnt == 0) { b->ignored++; return; }
    for (u32 i = 0; i < hsz; i++) if (p[2 + i] != b->id[i]) { b->ignored++; return; }
    if (seen(b, h)) { b->dup++; return; }
    if (b->pending) { b->busy++; return; }
    TXB[0] = p[0];
    TXB[1] = pl - 1;
    for (u32 i = 2 + hsz; i < len; i++) TXB[i - hsz] = p[i];
    b->txlen = len - hsz;
    b->fwd_direct++;
    factor = b->dfac;
  }
  u32 tw = airtime(b, b->txlen) * factor / 10;
  b->delay = rnd(b) % (5 * tw + 1);
  b->pending = 1;
}

static void after_rx(struct rs *b) {
  if (!b->pending) rx_start(b, RX_CONT);
  else if (b->delay == 0) do_tx(b);
  else rx_start(b, b->delay);
}

u32 rpt_state(u32 ev) {
  struct rs *b = RS;
  u32 (*h)(u32) = (u32 (*)(u32))b->orig;
  if (h) ev = h(ev);
  if (b->running != RUNNING) {          // stopped: step aside
    if (STATE_SLOT == (u32)rpt_state) STATE_SLOT = b->orig;
    return ev;
  }
  hook_in(b);                           // the ROM handler may have installed its successor
  b->events++;
  u32 s = IRQ;
  if (b->state == S_TX) {
    if (s & (IRQ_TX_DONE | IRQ_TIMEOUT)) {
      if (s & IRQ_TX_DONE) b->txdone++;
      rx_start(b, RX_CONT);
    }
  } else if (s & IRQ_RX_DONE) {
    if (s & IRQ_CRC) b->crc++;
    else {
      u8 lb[2];
      struct resp r = { 0, lb };
      H_PKT_LEN(0, 0, &r);
      u32 len = (lb[0] << 8) | lb[1];
      const u8 *f = (const u8 *)(0x800000 + (RXF_BASE & 0xFFFF));
      if (len > 255) len = 255;
      for (u32 i = 0; i < len; i++) RXB[i] = f[i];
      b->rx++;
      handle(b, len);
    }
    after_rx(b);
  } else if (s & IRQ_TIMEOUT) {
    if (b->pending) do_tx(b);
    else rx_start(b, RX_CONT);
  } else if (s & IRQ_HDR_ERR) {
    b->hdr_err++;
    after_rx(b);                        // a preamble stopped the wait's timer: start it again
  }
  return ev;
}

static void put32(u8 *o, u32 v) { o[0] = v; o[1] = v >> 8; o[2] = v >> 16; o[3] = v >> 24; }

// Entry 1: repeater. Params: op (1), then for op 1 (start): our ID (4: the first 1-3 bytes are the
// path hash), RX preamble (1), TX preamble (1), half-bit delay for the SPI pads (1, cycles), SF (1),
// bw code (1, 3-6), CR (1, 1-4), flood and direct delay factors (1 each, tenths of the airtime),
// flood hop limit (1).
//   op 1  start receiving and forwarding; the SPI pads idle with NSS pulled up, SCK and MOSI down
//   op 2  status
//   op 3  inject: handle RXB[0..len) as if received (len (1)); reports what it would send
//   op 0  stop: give the state slot back, restore the SPI pads (send SetStandby next)
// Response, 16 LE words: state | pending << 8 | hooked << 16, received, CRC errors, header errors,
// floods forwarded, directs forwarded, duplicates, ignored, busy, TX done, SPI write failures,
// events, last length, length waiting, delay waiting (ticks), IRQ status.
int rpt_cmd(u32 len, const u8 *p, struct resp *r) {
  struct rs *b = RS;
  if (len < 1) return 1;
  u32 op = p[0];
  if (op == 1) {
    if (len < 14 || p[8] < 5 || p[8] > 12 || p[9] < 3 || p[9] > 6 || p[10] < 1 || p[10] > 4 || p[7] == 0) return 1;
    if (b->running != RUNNING || STATE_SLOT != (u32)rpt_state) {   // not a restart
      b->gate_saved = IRQ_GATE;
      pads_idle(b->padcfg);
    }
    IRQ_GATE |= IRQ_ALL;
    for (u32 i = 0; i < 4; i++) b->id[i] = p[1 + i];
    b->rx_pre = p[5]; b->tx_pre = p[6]; b->hb = p[7]; b->sf = p[8]; b->bwc = p[9]; b->cr = p[10];
    b->ffac = p[11]; b->dfac = p[12]; b->maxhops = p[13];
    b->rng = 0x9E3779B9u ^ (p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4]);
    b->pending = b->seen_pos = 0;
    b->rx = b->crc = b->hdr_err = b->fwd_flood = b->fwd_direct = b->dup = b->ignored = b->busy = 0;
    b->txdone = b->spi_fail = b->events = b->last_len = 0;
    for (u32 i = 0; i < NSEEN; i++) SEEN[i] = 0;
    static const u8 on[1] = { 1 };
    H_STOP_TO_PRE(1, on, 0);
    b->orig = 0;
    b->running = RUNNING;
    rx_start(b, RX_CONT);
  } else if (op == 3) {
    if (len < 2) return 1;
    b->pending = 0;
    handle(b, p[1]);
  } else if (op == 0) {
    if (b->running == RUNNING) { IRQ_GATE = b->gate_saved; pads_restore(b->padcfg); }
    b->running = 0;
    b->state = S_OFF;
    if (STATE_SLOT == (u32)rpt_state) STATE_SLOT = b->orig;
  } else if (op != 2) {
    return 1;
  }
  u32 v[16] = { b->state | (b->pending << 8) | ((STATE_SLOT == (u32)rpt_state) << 16), b->rx, b->crc,
                b->hdr_err, b->fwd_flood, b->fwd_direct, b->dup, b->ignored, b->busy, b->txdone,
                b->spi_fail, b->events, b->last_len, b->txlen, b->delay, IRQ };
  for (u32 i = 0; i < 16; i++) put32(r->buf + 4 * i, v[i]);
  r->len = 64;
  return 3;
}
