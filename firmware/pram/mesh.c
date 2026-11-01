// A simple MeshCore repeater that runs on the LR2021's own CPU (docs/pram-repeater.md).
// Built with selfspi.c as mesh.bin (build_mesh.sh).
//
// It runs in the background from the ROM's radio state handler slot (gp+0x34), like the channel
// scanner (scan.c): the ROM main loop hands every radio event to rpt_state, which first passes it
// to the handler the ROM installed, then looks at the IRQ status and takes the next step itself.
//
//   receive (60 s at a time) -> RxDone, CRC good: parse the MeshCore header
//     flood (route 1), payload types 0-8, not seen before, under the hop limit:
//       append our hash to the path, wait rand(0 .. 5 t), t = airtime x flood factor
//     direct (route 2), we're the next hop, not seen before:
//       remove our hash from the path, wait rand(0 .. 5 t), t = airtime x direct factor
//     TRACE (direct), the next hash in its list is ours, not seen before:
//       append our SNR (x4, int8) to the path, wait as for direct
//   wait: an RX with the radio's timeout (32.768 kHz ticks); StopTimeoutOnPreamble lets a packet
//     that starts meanwhile finish (then the wait starts again)
//   timeout: write the packet into the TX FIFO through the chip's own SPI pads (fifo_write in
//     selfspi.c: the MCU must leave NSS/SCK/MOSI floating), SetTx
//   TxDone: back to RX
//   idle timeout (60 s, no packet waiting): send the stored advert if it's due, then RX again
//
// Clock: the radio's timer block has a free-running 32.768 kHz counter at 0xF11014. Every event
// adds the ticks since the last one (masked to 24 bits; the 60 s RX timeout guarantees an event
// well within 512 s), so the chip keeps its own uptime with nobody watching.
//
// Advert: MeshCore adverts are Ed25519-signed, which the chip can't do. The host signs one and
// stores the whole packet at ADV (op 4); the chip sends it every interval and marks it seen so
// echoes from other repeaters aren't forwarded again. Its timestamp is fixed, so nodes that already
// have it ignore the repeats; the host should store a fresh one now and then.
//
// Dedup: a 32-entry table of FNV-1a hashes over payload type (+ path_len for TRACE) and payload,
// as MeshCore's packet hash (which is SHA-256 over the same fields). Transport-code routes (0 and 3)
// aren't forwarded.
//
// TX queue: packets to send sit back to back in a 1128-byte arena, each with its own due time on the
// chip's clock (8 typical MeshCore packets, 4 of maximum size). The chip receives until the earliest
// is due, sends it, then schedules the next. A packet that doesn't fit is dropped (counted as busy).
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
#define H_PKT_STATUS  ((handler_t)0x57E8)   // 0x022A GetLoraPacketStatus (10 bytes; [2] = SNR x4)
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
#define TICKS       (*(volatile u32 *)0xF11014)   // free-running 32.768 kHz counter
// The ROM's RX FIFO (0x806140) is a 232-byte ring: a longer packet wraps over its own start. Move
// it to free RAM with room for 255 bytes.
#define RXF_RAM     0x807800u
#define RXF_LEN     0x100u                  // 255-byte packets fit
#define GP 0x806000u
#define STATE_SLOT (*(volatile u32 *)(GP + 0x34))
#define RX_IDLE (60u * 32768u)              // idle RX timeout: the clock ticks at least this often

// RAM above the stack (docs/lr2021-cpu.md#memory):
// 0x807800-0x807903: RX FIFO (RXF_RAM); the received packet is parsed in place
#define QA   ((u8 *)0x807908)               // TX queue arena
#define QA_LEN 0x468u                       // up to 0x807D70
#define ADV  ((u8 *)0x807D70)               // stored advert packet (host-signed), <= 144
#define ADV_MAX 144u
#define SEEN ((u32 *)0x807F00)              // 64 hashes (after the state at 0x807E00)
#define NSEEN 64
enum { S_OFF, S_RX, S_TX };
struct rs {
  u32 running, orig, state, gate_saved;
  u8 id[4];
  u32 rx_pre, tx_pre, hb, sf, bwc, cr, ffac, dfac, maxhops;
  u32 q_n, q_used, now, rng, seen_pos;     // queue entries, bytes; clock in ticks
  u32 rx, crc, hdr_err, fwd_flood, fwd_direct, dup, ignored, busy, txdone, spi_fail, events, last_len;
  u32 padcfg[3];                       // SPI pads as found at start
  u32 last_tick, frac, secs;           // clock
  u32 adv_len, adv_every, adv_next, adv_sent, fwd_trace;
  s32 snr;
};
#define RS ((struct rs *)0x807E00)
#define RUNNING 0x4D455348u                 // RAM isn't cleared at load: a magic, not just nonzero

u32 rpt_state(u32 ev);

static void hook_in(struct rs *b) {
  u32 cur = STATE_SLOT;
  if (cur != (u32)rpt_state) { b->orig = cur; STATE_SLOT = (u32)rpt_state; }
}

static void clock(struct rs *b) {
  u32 t = TICKS, d = (t - b->last_tick) & 0xFFFFFFu;
  b->last_tick = t;
  b->now += d;
  b->frac += d;
  b->secs += b->frac >> 15;
  b->frac &= 0x7FFF;
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

struct qe { u32 due, len; };                // then the packet, padded to 4 bytes

static u32 qe_size(u32 len) { return 8 + ((len + 3) & ~3u); }

// Room for a len-byte packet due at `due`; returns where to write it, or 0 when the queue is full.
static u8 *q_alloc(struct rs *b, u32 len, u32 due) {
  u32 sz = qe_size(len);
  if (b->q_used + sz > QA_LEN) { b->busy++; return 0; }
  struct qe *e = (struct qe *)(QA + b->q_used);
  e->due = due; e->len = len;
  b->q_used += sz; b->q_n++;
  return (u8 *)(e + 1);
}

static struct qe *q_first(struct rs *b) {
  struct qe *best = 0;
  for (u32 o = 0; o < b->q_used;) {
    struct qe *e = (struct qe *)(QA + o);
    if (!best || (s32)(e->due - best->due) < 0) best = e;
    o += qe_size(e->len);
  }
  return best;
}

static void q_remove(struct rs *b, struct qe *e) {
  u32 o = (u8 *)e - QA, sz = qe_size(e->len);
  u32 *d = (u32 *)(QA + o);
  for (u32 i = 0; i < (b->q_used - o - sz) / 4; i++) d[i] = d[i + sz / 4];
  b->q_used -= sz; b->q_n--;
}

static void do_tx(struct rs *b, struct qe *e) {
  u32 len = e->len;
  u8 t[4] = { 0, (u8)b->tx_pre, (u8)len, 0x02 };
  H_SET_FS(0, 0, 0);
  H_CLR_TX(0, 0, 0);
  u32 lvl = fifo_write((const u8 *)(e + 1), len, b->hb);
  q_remove(b, e);                       // it's in the TX FIFO now (or lost)
  if (lvl != len) {                     // the MCU is driving the SPI pins
    b->spi_fail++;
    H_CLR_TX(0, 0, 0);
    rx_start(b, RX_IDLE);
    return;
  }
  H_LORA_PKT(4, t, 0);
  IRQ = IRQ_ALL;
  t[0] = t[1] = t[2] = 0;
  H_SET_TX(3, t, 0);
  b->state = S_TX;
  hook_in(b);
}

static void advert_due(struct rs *b);

// Send what's due, or receive until the next packet or advert is due (at most RX_IDLE).
static void schedule(struct rs *b) {
  advert_due(b);
  u32 wait = RX_IDLE;
  if (b->adv_len && b->adv_every && b->adv_next - b->secs <= 60) {   // not due yet: advert_due ran
    u32 t = (b->adv_next - b->secs) * 32768u - b->frac;
    if (t < wait) wait = t;
  }
  struct qe *e = q_first(b);
  if (e) {
    s32 d = (s32)(e->due - b->now);
    if (d <= 0) { do_tx(b, e); return; }
    if ((u32)d < wait) wait = d;
  }
  rx_start(b, wait ? wait : 1);
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

// MeshCore's packet identity: payload type, path_len for TRACE, payload. pay = payload offset.
static u32 pkt_hash(const u8 *p, u32 len, u32 pay) {
  u8 t[2] = { (u8)((p[0] >> 2) & 15), p[1] };
  u32 h = fnv(fnv(2166136261u, t, t[0] == 9 ? 2 : 1), p + pay, len - pay);
  return h ? h : 1;
}

// Seen before? Records it if not.
static u32 seen(struct rs *b, u32 h) {
  for (u32 i = 0; i < NSEEN; i++) if (SEEN[i] == h) return 1;
  SEEN[b->seen_pos++ % NSEEN] = h;
  return 0;
}

// Decide what to do with p[0..len) (the packet in the RX FIFO); queue it when it forwards.
static void handle(struct rs *b, const u8 *p, u32 len) {
  b->last_len = len;
  if (len < 3 || p[0] == 0xFF) { b->ignored++; return; }
  u32 route = p[0] & 3, ptype = (p[0] >> 2) & 15;
  if (route == 0 || route == 3) { b->ignored++; return; }   // transport codes
  u32 pl = p[1], hsz = (pl >> 6) + 1, cnt = pl & 63;
  u32 pay = 2 + cnt * hsz;
  if (hsz == 4 || cnt * hsz > 64 || pay >= len) { b->ignored++; return; }
  u32 h = pkt_hash(p, len, pay);
  b->rng ^= h;
  u32 factor = b->dfac, kind, txlen;
  if (ptype == 9) {                                   // TRACE: the path holds SNRs, the hashes are in the payload
    if (route != 2 || pl >= 63 || len - pay < 10 || len + 1 > 255) { b->ignored++; return; }
    u32 sz = p[pay + 8] & 3, n = 1u << sz, off = pl << sz;
    const u8 *hs = p + pay + 9;
    if (sz > 2 || off + n > len - pay - 9) { b->ignored++; return; }   // end of the trace, or bad
    for (u32 i = 0; i < n; i++) if (hs[off + i] != b->id[i]) { b->ignored++; return; }
    kind = 2; txlen = len + 1;
  } else if (route == 1) {                            // flood
    if (ptype > 8 || cnt + 1 > b->maxhops || (cnt + 1) * hsz > 64 || len + hsz > 255) { b->ignored++; return; }
    kind = 0; txlen = len + hsz; factor = b->ffac;
  } else {                                            // direct: are we the next hop?
    if (cnt == 0) { b->ignored++; return; }
    for (u32 i = 0; i < hsz; i++) if (p[2 + i] != b->id[i]) { b->ignored++; return; }
    kind = 1; txlen = len - hsz;
  }
  if (seen(b, h)) { b->dup++; return; }
  u32 tw = airtime(b, txlen) * factor / 10;
  u8 *o = q_alloc(b, txlen, b->now + rnd(b) % (5 * tw + 1));
  if (!o) return;
  if (kind == 2) {                                    // TRACE: + our SNR
    for (u32 i = 0; i < pay; i++) o[i] = p[i];
    o[pay] = (u8)b->snr;
    for (u32 i = pay; i < len; i++) o[i + 1] = p[i];
    o[1] = pl + 1;
    b->fwd_trace++;
  } else if (kind == 0) {                             // flood: + our hash
    for (u32 i = 0; i < pay; i++) o[i] = p[i];
    for (u32 i = 0; i < hsz; i++) o[pay + i] = b->id[i];
    for (u32 i = pay; i < len; i++) o[i + hsz] = p[i];
    o[1] = pl + 1;
    b->fwd_flood++;
  } else {                                            // direct: - our hash
    o[0] = p[0];
    o[1] = pl - 1;
    for (u32 i = 2 + hsz; i < len; i++) o[i - hsz] = p[i];
    b->fwd_direct++;
  }
}

// Queue the stored advert if it's due.
static void advert_due(struct rs *b) {
  if (!b->adv_len || !b->adv_every || (s32)(b->secs - b->adv_next) < 0) return;
  b->adv_next = b->secs + b->adv_every;
  u8 *o = q_alloc(b, b->adv_len, b->now);
  if (!o) return;
  for (u32 i = 0; i < b->adv_len; i++) o[i] = ADV[i];
  b->adv_sent++;
  seen(b, pkt_hash(ADV, b->adv_len, 2 + (ADV[1] & 63) * ((ADV[1] >> 6) + 1)));
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
  clock(b);
  u32 s = IRQ;
  if (b->state == S_TX) {
    if (s & (IRQ_TX_DONE | IRQ_TIMEOUT)) {
      if (s & IRQ_TX_DONE) b->txdone++;
      schedule(b);
    }
  } else if (s & IRQ_RX_DONE) {
    if (s & IRQ_CRC) b->crc++;
    else {
      u8 st[10];
      struct resp r = { 0, st };
      H_PKT_LEN(0, 0, &r);
      u32 len = (st[0] << 8) | st[1];
      H_PKT_STATUS(0, 0, &r);
      b->snr = (signed char)st[2];
      b->rx++;
      handle(b, (const u8 *)(0x800000 + (RXF_BASE & 0xFFFF)), len > 255 ? 255 : len);
    }
    schedule(b);
  } else if (s & IRQ_TIMEOUT) {
    schedule(b);
  } else if (s & IRQ_HDR_ERR) {
    b->hdr_err++;
    schedule(b);                        // a preamble stopped the wait's timer: start it again
  }
  return ev;
}

static void put32(u8 *o, u32 v) { o[0] = v; o[1] = v >> 8; o[2] = v >> 16; o[3] = v >> 24; }

// Entry 1: repeater. Params: op (1), then for op 1 (start): our ID (4: the first 1-3 bytes are the
// path hash; with an advert, the first bytes of its public key), RX preamble (1), TX preamble (1),
// half-bit delay for the SPI pads (1, cycles), SF (1), bw code (1, 3-6), CR (1, 1-4), flood and
// direct delay factors (1 each, tenths of the airtime), flood hop limit (1).
//   op 1  start receiving and forwarding; the SPI pads idle with NSS pulled up, SCK and MOSI down
//   op 2  status
//   op 3  inject: handle 0x807800[0..len) as if received (len (1)), with the repeater stopped;
//         the result is queued at 0x807908
//   op 4  advert: the packet is at ADV (0x807D70, <= 144 bytes); len (1, 0 = none), interval (2, minutes),
//         first one after (2, seconds)
//   op 0  stop: give the state slot back, restore the SPI pads (send SetStandby next)
// Response, 20 LE words: state | queued packets << 8 | hooked << 16, received, CRC errors, header errors,
// floods forwarded, directs forwarded, duplicates, ignored, busy, TX done, SPI write failures,
// events, last length, queue bytes used, ticks until the next is due, IRQ status, uptime (s), adverts
// sent, TRACEs forwarded, last SNR (x4).
int rpt_cmd(u32 len, const u8 *p, struct resp *r) {
  struct rs *b = RS;
  if (len < 1) return 1;
  u32 op = p[0];
  if (op == 1) {
    if (len < 14 || p[8] < 5 || p[8] > 12 || p[9] < 3 || p[9] > 6 || p[10] < 1 || p[10] > 4 || p[7] == 0) return 1;
    if (b->running != RUNNING || STATE_SLOT != (u32)rpt_state) {   // not a restart
      b->gate_saved = IRQ_GATE;
      pads_idle(b->padcfg);
      b->adv_len = 0;
    }
    IRQ_GATE |= IRQ_ALL;
    for (u32 i = 0; i < 4; i++) b->id[i] = p[1 + i];
    b->rx_pre = p[5]; b->tx_pre = p[6]; b->hb = p[7]; b->sf = p[8]; b->bwc = p[9]; b->cr = p[10];
    b->ffac = p[11]; b->dfac = p[12]; b->maxhops = p[13];
    b->rng = 0x9E3779B9u ^ (p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4]) ^ TICKS;
    b->q_n = b->q_used = b->seen_pos = b->now = 0;
    b->rx = b->crc = b->hdr_err = b->fwd_flood = b->fwd_direct = b->dup = b->ignored = b->busy = 0;
    b->txdone = b->spi_fail = b->events = b->last_len = b->adv_sent = b->fwd_trace = 0;
    b->last_tick = TICKS; b->frac = b->secs = 0; b->snr = 0;
    for (u32 i = 0; i < NSEEN; i++) SEEN[i] = 0;
    static const u8 on[1] = { 1 };
    H_STOP_TO_PRE(1, on, 0);
    b->orig = 0;
    b->running = RUNNING;
    rx_start(b, RX_IDLE);
  } else if (op == 3) {
    if (len < 2) return 1;
    clock(b);
    handle(b, (const u8 *)RXF_RAM, p[1]);
  } else if (op == 4) {
    if (len < 6 || p[1] > ADV_MAX) return 1;
    clock(b);
    b->adv_len = p[1];
    b->adv_every = ((p[2] << 8) | p[3]) * 60;
    b->adv_next = b->secs + ((p[4] << 8) | p[5]);
    if (b->running == RUNNING && b->state == S_RX) schedule(b);   // shorten the current RX wait
  } else if (op == 0) {
    if (b->running == RUNNING) { IRQ_GATE = b->gate_saved; pads_restore(b->padcfg); }
    b->running = 0;
    b->state = S_OFF;
    if (STATE_SLOT == (u32)rpt_state) STATE_SLOT = b->orig;
  } else if (op != 2) {
    return 1;
  }
  if (b->running == RUNNING) clock(b);
  struct qe *nx = q_first(b);
  u32 v[20] = { b->state | (b->q_n << 8) | ((STATE_SLOT == (u32)rpt_state) << 16), b->rx, b->crc,
                b->hdr_err, b->fwd_flood, b->fwd_direct, b->dup, b->ignored, b->busy, b->txdone,
                b->spi_fail, b->events, b->last_len, b->q_used, nx ? nx->due - b->now : 0, IRQ,
                b->secs, b->adv_sent, b->fwd_trace, (u32)b->snr };
  for (u32 i = 0; i < 20; i++) put32(r->buf + 4 * i, v[i]);
  r->len = 80;
  return 3;
}
