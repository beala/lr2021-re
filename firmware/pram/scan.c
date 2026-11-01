// On-chip multi-channel LoRa scanner: hop over a channel table running CAD on each channel, and
// stop on the first detection. With exit mode RX the chip is then already receiving the packet on
// that channel; the host just waits for RxDone as usual.
//
// Channel table (written by the host with WriteRegMem32 before the command), at TAB:
//   struct { u32 freq_hz; u8 mod0, mod1, peak, pre; }   x n  (n <= 32)
//   mod0/mod1 = SetLoraModulationParams bytes ((sf << 4) | bw, (cr - 4) << 4 | ldro),
//   peak = CAD detect peak for that SF, pre = RX preamble length for that channel (0 = leave the
//   packet params alone). The RX preamble should match the transmitters' (MeshCore: 32 at SF7-8,
//   16 at SF9+); a mismatch gives header CRC errors.
//
// Entry 1 params (big-endian): n (1), cad symbols (1), exit mode (1: 0 = fallback, 1 = RX),
//   symbols to wait for a preamble after a detection (1), 0 (2), rounds (2). The wait is converted
//   per channel into RX-timeout ticks (32.768 kHz) from that channel's SF and bandwidth, so a slow
//   SF gets enough time and a false alarm on a fast one costs little.
// Response (8 bytes): channel index or 0xFF, round (2), CADs run (2), false alarms (1), polls of
//   the last CAD (2).
//
// False alarms: with exit mode RX, a detection is only reported once the receiver also sees a
// preamble. If the short RX timeout runs out first (the host sets StopTimeoutOnPreamble, so a
// real preamble cancels it), the chip falls back to FS and the scan carries on.
//
// The radio must be in LoRa mode, the fallback mode FS (0x0206 03) so the synthesizer keeps
// running between hops, and the DIO IRQ mask set by the host.
typedef unsigned int u32;
typedef unsigned char u8;

struct resp { u32 len; u8 *buf; };
typedef int (*handler_t)(u32 len, const u8 *p, struct resp *r);
#define H_SET_RF_FREQ ((handler_t)0xFC00)   // 0x0200
#define H_LORA_MOD    ((handler_t)0xF238)   // 0x0220
#define H_CAD_PARAMS  ((handler_t)0xF188)   // 0x0227
#define H_SET_CAD     ((handler_t)0xF178)   // 0x0228
#define H_LORA_PKT    ((handler_t)0xF274)   // 0x0221

#define IRQ (*(volatile u32 *)0xF2A00C)     // IRQ status, write 1 to clear
#define IRQ_CAD_DET  (1u << 7)
#define IRQ_CAD_DONE (1u << 20)
#define IRQ_PREAMBLE (1u << 5)
#define IRQ_HDR      (1u << 6)
#define IRQ_RX_DONE  (1u << 18)
#define IRQ_TIMEOUT  (1u << 21)
#define IRQ_SCAN     ((1u << 5) | (1u << 6) | IRQ_CAD_DET | (1u << 18) | IRQ_CAD_DONE | (1u << 21) | (1u << 16))

// The ROM main loop (0xA1AC) takes the pending-event word set by interrupt handlers, runs host
// commands for bit 0 (that is where this code runs) and hands the other bits to the current
// state handler. CAD completion is processed there, so a command that spins blocks it. yield()
// does one iteration of that dispatch for the radio events, leaving bit 0 alone.
#define GP 0x806000u
#define EV_PENDING (*(volatile u32 *)(GP + 0x44))
#define EV_LEFT    (*(volatile u32 *)(GP + 0x6C))
#define STATE_FN   (*(u32 (**)(u32))(GP + 0x34))
#define NOTIFY_FN  (*(void (**)(u32))(GP + 0x38))
#define ROM_STATUS_UPDATE ((void (*)(void))0x25A4)

static void yield(void) {
  u32 st, ev;
  __asm__ volatile("clri %0" : "=r"(st) :: "memory");
  ev = EV_PENDING;
  EV_PENDING = ev & 1u;
  __asm__ volatile("seti %0" :: "r"(st) : "memory");
  ev &= ~1u;
  if (!ev) return;
  u32 (*h)(u32) = STATE_FN;
  if (h) ev = h(ev);
  if (ev & 0x4000u) NOTIFY_FN(256);
  EV_LEFT |= ev & 0xFFFF9FFFu;
  ROM_STATUS_UPDATE();                  // the loop refreshes the host-visible status after work
}

struct chan { u32 freq; u8 mod0, mod1, peak, pre; };

// RX timeout ticks (32.768 kHz) for `sym` symbols of this channel: sym * 2^sf / bw * 32768
static u32 wait_ticks(u32 mod0, u32 sym) {
  u32 sf = mod0 >> 4, bwc = mod0 & 0x0F, bw_div;
  switch (bwc) {
    case 2: bw_div = 31250; break;
    case 3: bw_div = 62500; break;
    case 4: bw_div = 125000; break;
    case 5: bw_div = 250000; break;
    case 6: bw_div = 500000; break;
    default: bw_div = 125000; break;
  }
  u32 chips = sym << sf;                              // <= 255 * 4096
  u32 q = chips / bw_div, rem = chips % bw_div;       // chips * 32768 / bw without overflowing 32 bits
  u32 t = (q << 15) + ((((rem << 8) / bw_div)) << 7) + 1;
  return t > 0xFFFFFFu ? 0xFFFFFFu : t;
}
#define TAB ((const struct chan *)0x807800)
#define POLL_MAX 200000u

int scan_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 8) return 1;
  u32 n = p[0], nsym = p[1], exitm = p[2];
  u32 rounds = (p[6] << 8) | p[7];
  if (n == 0 || n > 32 || rounds == 0) return 1;
  u32 wsym = p[3];
  u8 cadp[7] = { (u8)nsym, 0, (u8)exitm, 0, 0, 0, 0 };
  u8 fb[4], mod[2], pkt[4] = { 0, 0, 0, 0x02 };   // explicit header, CRC on, standard IQ
  u32 last_pre = 0x100, last_mod = 0xFFFFFFFFu, last_peak = 0x100, cads = 0, polls = 0, hit = 0xFF, rnd = 0, falses = 0;
  for (rnd = 0; rnd < rounds && hit == 0xFF; rnd++) {
    for (u32 c = 0; c < n; c++) {
      const struct chan *ch = &TAB[c];
      u32 f = ch->freq;
      fb[0] = f >> 24; fb[1] = f >> 16; fb[2] = f >> 8; fb[3] = f;
      H_SET_RF_FREQ(4, fb, 0);
      u32 m = ch->mod0 | (ch->mod1 << 8);
      if (m != last_mod) { mod[0] = ch->mod0; mod[1] = ch->mod1; H_LORA_MOD(2, mod, 0); last_mod = m; }
      if (ch->pre && ch->pre != last_pre) { pkt[1] = ch->pre; H_LORA_PKT(4, pkt, 0); last_pre = ch->pre; }
      if (ch->peak != last_peak) { cadp[6] = ch->peak; last_peak = ch->peak; }
      u32 wt = wait_ticks(ch->mod0, wsym);
      cadp[3] = wt >> 16; cadp[4] = wt >> 8; cadp[5] = wt;
      H_CAD_PARAMS(7, cadp, 0);
      IRQ = IRQ_SCAN;
      H_SET_CAD(0, 0, 0);
      cads++;
      u32 k = 0, s;
      while (!((s = IRQ) & IRQ_CAD_DONE)) {
        yield();
        if (++k > POLL_MAX) { hit = 0xFE; polls = k; goto out; }
      }
      polls = k;
      if (!(s & IRQ_CAD_DET)) continue;
      if (exitm != 1) { hit = c; goto out; }
      // CAD fired and the chip is now receiving on channel c: wait for a preamble or the timeout
      k = 0;
      while (!((s = IRQ) & (IRQ_PREAMBLE | IRQ_HDR | IRQ_RX_DONE | IRQ_TIMEOUT))) {
        yield();
        if (++k > POLL_MAX) { hit = 0xFD; goto out; }
      }
      if (s & (IRQ_PREAMBLE | IRQ_HDR | IRQ_RX_DONE)) { hit = c; goto out; }
      falses++;                         // timeout: false alarm, the chip is back in FS
      last_mod = 0xFFFFFFFFu;           // be safe: re-send modulation and packet params after an RX
      last_pre = 0x100;
    }
  }
out:
  r->buf[0] = hit;
  r->buf[1] = rnd >> 8; r->buf[2] = rnd;
  r->buf[3] = cads >> 8; r->buf[4] = cads;
  r->buf[5] = falses > 255 ? 255 : falses; r->buf[6] = polls >> 8; r->buf[7] = polls;
  r->len = 8;
  return 3;
}
