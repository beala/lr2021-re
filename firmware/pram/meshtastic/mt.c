// SPDX-License-Identifier: GPL-3.0-only
// A Meshtastic node (CLIENT role, relaying) that runs on the LR2021's own CPU
// (docs/pram-meshtastic.md). Built with ../selfspi.c as mt.bin (build_mt.sh).
//
// The routing rules follow the Meshtastic firmware (https://github.com/meshtastic/firmware, GPL-3.0):
// src/mesh/NextHopRouter.cpp, FloodingRouter.cpp, PacketHistory.cpp and RadioInterface.cpp. This file
// is a port of those rules and is licensed under the GPL-3.0 like them (COPYING in this directory).
//
// The chip can't decrypt (no AES here), so every packet is handled from its 16-byte outer header,
// as Meshtastic's own relay of packets it can't decode (NextHopRouter::relayOpaquePacket), but with
// the managed-flooding rules a CLIENT applies to packets on its channel:
//
//   RxDone, CRC good, >= 16 bytes:
//     header (little endian): to (4), from (4), id (4), flags (hop_limit 2:0, want_ack 3,
//     via_mqtt 4, hop_start 7:5), channel hash, next_hop, relay_node
//     new (from, id), not from or to us, id != 0, hop_limit > 0, next_hop 0 or our last byte:
//       queue a copy with hop_limit - 1 and relay_node = our last byte, sent after
//       (2 CWmax + rand(2^CW)) slots (CLIENT) or rand(2 CW) slots (ROUTER flag), CW = 3 + (SNR + 20) / 6
//     seen before, with more hops left than any copy so far ("upgrade"): requeue ours with the
//       higher hop_limit, or relay it again if ours already went
//     seen before, from the originator again (hop_start == hop_limit): relay again unless still queued
//     seen before, otherwise: someone else relayed it, so drop our queued copy (CLIENT), unless the
//       first copy named us as next hop
//   the RX wait, TX through the chip's own SPI pads, the clock and the queue are as in mesh.c
//
// NodeInfo: phones list a node once they get its NodeInfo, which is AES-CTR encrypted with the
// channel key and the packet id. The host encrypts a few copies with different ids and stores them
// at NI; the chip sends the next one every interval (Meshtastic: 3 h).
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
#define IRQ_GATE    (*(volatile u32 *)0x80060C)
#define RXF_BASE    (*(volatile u32 *)0xF30028)
#define RXF_SIZE    (*(volatile u32 *)0xF30030)
#define TICKS       (*(volatile u32 *)0xF11014)   // free-running 32.768 kHz counter
#define RXF_RAM     0x807800u
#define RXF_LEN     0x100u
#define GP 0x806000u
#define STATE_SLOT (*(volatile u32 *)(GP + 0x34))
#define RX_IDLE (60u * 32768u)

// RAM above the stack:
// 0x807800-0x807903: RX FIFO (RXF_RAM); the received packet is parsed in place
#define QA   ((u8 *)0x807908)               // TX queue arena
#define QA_LEN 0x3F8u                       // up to 0x807D00
#define NI   ((u8 *)0x807D00)               // NodeInfo packets from the host: (len, packet) ...
#define NI_MAX 256u
#define SEEN ((u32 *)0x807F00)              // 64 entries: hash(from, id) | first-copy-named-us << 3 | top hop_limit
#define NSEEN 64
#define CW_MIN 3
#define CW_MAX 8
#define F_ROUTER   1                        // ROUTER timing: rand(2 CW) slots, never drop a queued relay
enum { S_OFF, S_RX, S_TX };
struct rs {
  u32 running, orig, state, gate_saved;
  u32 me, hb, mode, slot;                 // our node number; slot time in ticks
  u32 rx_pre, tx_pre;
  u32 q_n, q_used, now, rng, seen_pos;
  u32 rx, crc, hdr_err, relayed, dup, cancelled, upgraded, ignored, busy, txdone, spi_fail, events, last_len;
  u32 padcfg[3];
  u32 last_tick, frac, secs;
  u32 ni_n, ni_every, ni_next, ni_sent, ni_idx;
  s32 snr;                                // x4
};
#define RS ((struct rs *)0x807E00)
#define RUNNING 0x4D545354u                 // 'MTST'

u32 mt_state(u32 ev);

static void hook_in(struct rs *b) {
  u32 cur = STATE_SLOT;
  if (cur != (u32)mt_state) { b->orig = cur; STATE_SLOT = (u32)mt_state; }
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

static u32 get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

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

// The queued packet with this (from, id), if any.
static struct qe *q_find(struct rs *b, u32 from, u32 id) {
  for (u32 o = 0; o < b->q_used;) {
    struct qe *e = (struct qe *)(QA + o);
    const u8 *p = (const u8 *)(e + 1);
    if (get32(p + 4) == from && get32(p + 8) == id) return e;
    o += qe_size(e->len);
  }
  return 0;
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
  q_remove(b, e);
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

static void nodeinfo_due(struct rs *b);

static void schedule(struct rs *b) {
  nodeinfo_due(b);
  u32 wait = RX_IDLE;
  if (b->ni_n && b->ni_every && b->ni_next - b->secs <= 60) {
    u32 t = (b->ni_next - b->secs) * 32768u - b->frac;
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

// RadioInterface::getTxDelayMsecWeighted: CW = map(SNR, -20..10 dB, 3..8), not clamped at the top.
static u32 relay_delay(struct rs *b) {
  s32 cw = (b->snr / 4 + 20) * (CW_MAX - CW_MIN) / 30 + CW_MIN;
  if (cw < 0) cw = 0;
  if (cw > 10) cw = 10;
  if (b->mode & F_ROUTER) return cw ? rnd(b) % (2 * cw) * b->slot : 0;
  return (2 * CW_MAX + rnd(b) % (1u << cw)) * b->slot;
}

static u32 fnv(u32 h, const u8 *p, u32 n) {
  for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

static u32 *seen_find(u32 key) {
  for (u32 i = 0; i < NSEEN; i++) if ((SEEN[i] & ~15u) == key) return &SEEN[i];
  return 0;
}

// Queue (or requeue, when e is our queued copy) a relay of p with one hop less.
static void relay(struct rs *b, const u8 *p, u32 len, struct qe *e) {
  u32 due = b->now + relay_delay(b);
  u8 *o;
  if (e) { e->due = due; o = (u8 *)(e + 1); }
  else if (!(o = q_alloc(b, len, due))) return;
  else for (u32 i = 0; i < len; i++) o[i] = p[i];
  o[12] = (p[12] & ~7u) | ((p[12] & 7) - 1);
  o[15] = (u8)b->me;
  if (!e) b->relayed++;
}

static void handle(struct rs *b, const u8 *p, u32 len) {
  b->last_len = len;
  if (len < 16) { b->ignored++; return; }
  u32 to = get32(p), from = get32(p + 4), id = get32(p + 8);
  u32 hop = p[12] & 7, hop_start = p[12] >> 5, next = p[14], mine = b->me & 0xFF;
  if (from == b->me || id == 0) { b->ignored++; return; }
  u32 key = fnv(2166136261u, p + 4, 8) & ~15u;
  if (!key) key = 16;
  b->rng ^= key;
  u32 ok = to != b->me && to != 1 && (next == 0 || next == mine);   // 1: NODENUM_BROADCAST_NO_LORA
  u32 *s = seen_find(key);
  if (!s) {
    SEEN[b->seen_pos++ % NSEEN] = key | (next == mine) << 3 | hop;
    if (ok && hop > 0) relay(b, p, len, 0);
    else b->ignored++;
    return;
  }
  b->dup++;
  struct qe *e = q_find(b, from, id);
  if (hop > (*s & 7)) {                       // more hops left than any copy so far
    *s = (*s & ~7u) | hop;
    b->upgraded++;
    if (ok) relay(b, p, len, e);
  } else if (hop_start && hop_start == hop) { // the originator sent it again
    if (ok && !e) relay(b, p, len, 0);
  } else if (e && !(b->mode & F_ROUTER) && !(*s & 8)) {
    q_remove(b, e);                         // someone else relayed it
    b->cancelled++;
  }
}

// Queue the next stored NodeInfo if it's due.
static void nodeinfo_due(struct rs *b) {
  if (!b->ni_n || !b->ni_every || (s32)(b->secs - b->ni_next) < 0) return;
  b->ni_next = b->secs + b->ni_every;
  const u8 *p = NI;
  for (u32 i = 0; i < b->ni_idx % b->ni_n; i++) p += 1 + p[0];
  b->ni_idx++;
  u8 *o = q_alloc(b, p[0], b->now);
  if (!o) return;
  for (u32 i = 0; i < p[0]; i++) o[i] = p[1 + i];
  b->ni_sent++;
}

u32 mt_state(u32 ev) {
  struct rs *b = RS;
  u32 (*h)(u32) = (u32 (*)(u32))b->orig;
  if (h) ev = h(ev);
  if (b->running != RUNNING) {
    if (STATE_SLOT == (u32)mt_state) STATE_SLOT = b->orig;
    return ev;
  }
  hook_in(b);
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
    schedule(b);
  }
  return ev;
}

static void put32(u8 *o, u32 v) { o[0] = v; o[1] = v >> 8; o[2] = v >> 16; o[3] = v >> 24; }

// Entry 1: node. Params: op (1), then
//   op 1  start: node number (4, BE), RX preamble (1), TX preamble (1), SPI pad half-bit delay (1),
//         SF (1), bw code (1, 3-6 = 62.5-500 kHz), mode (1, bit 0 = ROUTER timing). The modulation
//         and sync word (0x2B) are the host's: the chip keeps what it was set to.
//   op 2  status
//   op 3  inject: handle 0x807800[0..len) as if received, with the node stopped; len (1), SNR x4 (1)
//   op 4  NodeInfo: packets at NI (0x807D00, (len, packet) each, <= 256 bytes in all); count (1,
//         0 = none), interval (2, minutes), first one after (2, seconds)
//   op 0  stop
// Response, 20 LE words: state | queued << 8 | hooked << 16, received, CRC errors, header errors,
// relayed, duplicates, relays dropped (heard from another node), upgrades, ignored, busy (queue
// full), TX done, SPI write failures, events, last length, queue bytes, ticks until the next TX,
// IRQ status, uptime (s), NodeInfos sent, last SNR (x4).
int mt_cmd(u32 len, const u8 *p, struct resp *r) {
  struct rs *b = RS;
  if (len < 1) return 1;
  u32 op = p[0];
  if (op == 1) {
    if (len < 11 || p[8] < 5 || p[8] > 12 || p[9] < 3 || p[9] > 6 || p[7] == 0) return 1;
    if (b->running != RUNNING || STATE_SLOT != (u32)mt_state) {
      b->gate_saved = IRQ_GATE;
      pads_idle(b->padcfg);
      b->ni_n = 0;
    }
    IRQ_GATE |= IRQ_ALL;
    b->me = (u32)p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
    b->rx_pre = p[5]; b->tx_pre = p[6]; b->hb = p[7]; b->mode = p[10];
    // computeSlotTimeMsec: 4.5 symbols (4-symbol CAD + 0.5) + 7.6 ms, in 32.768 kHz ticks
    u32 sym16 = ((1u << p[8]) << 19) / (62500u << (p[9] - 3));
    b->slot = sym16 * 9 / 32 + 249;
    b->rng = 0x9E3779B9u ^ b->me ^ TICKS;
    b->q_n = b->q_used = b->seen_pos = b->now = 0;
    b->rx = b->crc = b->hdr_err = b->relayed = b->dup = b->cancelled = b->upgraded = b->ignored = 0;
    b->busy = b->txdone = b->spi_fail = b->events = b->last_len = b->ni_sent = 0;
    b->last_tick = TICKS; b->frac = b->secs = 0; b->snr = 0;
    for (u32 i = 0; i < NSEEN; i++) SEEN[i] = 0;
    static const u8 on[1] = { 1 };
    H_STOP_TO_PRE(1, on, 0);
    b->orig = 0;
    b->running = RUNNING;
    rx_start(b, RX_IDLE);
  } else if (op == 3) {
    if (len < 3) return 1;
    clock(b);
    b->snr = (signed char)p[2];
    handle(b, (const u8 *)RXF_RAM, p[1]);
  } else if (op == 4) {
    if (len < 6) return 1;
    u32 tot = 0;
    for (u32 i = 0; i < p[1]; i++) { tot += 1 + NI[tot]; if (tot > NI_MAX) return 1; }
    clock(b);
    b->ni_n = p[1];
    b->ni_idx = 0;
    b->ni_every = ((p[2] << 8) | p[3]) * 60;
    b->ni_next = b->secs + ((p[4] << 8) | p[5]);
    if (b->running == RUNNING && b->state == S_RX) schedule(b);
  } else if (op == 0) {
    if (b->running == RUNNING) { IRQ_GATE = b->gate_saved; pads_restore(b->padcfg); }
    b->running = 0;
    b->state = S_OFF;
    if (STATE_SLOT == (u32)mt_state) STATE_SLOT = b->orig;
  } else if (op != 2) {
    return 1;
  }
  if (b->running == RUNNING) clock(b);
  struct qe *nx = q_first(b);
  u32 v[20] = { b->state | (b->q_n << 8) | ((STATE_SLOT == (u32)mt_state) << 16), b->rx, b->crc,
                b->hdr_err, b->relayed, b->dup, b->cancelled, b->upgraded, b->ignored, b->busy,
                b->txdone, b->spi_fail, b->events, b->last_len, b->q_used, nx ? nx->due - b->now : 0,
                IRQ, b->secs, b->ni_sent, (u32)b->snr };
  for (u32 i = 0; i < 20; i++) put32(r->buf + 4 * i, v[i]);
  r->len = 80;
  return 3;
}
