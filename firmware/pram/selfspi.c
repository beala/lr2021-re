// WriteTxFifo (0x0002) clocked into the LR2021's own SPI slave through its SPI pads
// (docs/pram-lora-tx.md). Pads 1, 2, 3 = NSS, SCK, MOSI. The pads keep their SPI mux, so the slave
// still listens; the CPU only sets each level with the pad pull (cfg bits 5:4: 2 up, 1 down) with
// the output off. The MCU must leave its NSS/SCK/MOSI pins floating meanwhile; if it drives them,
// the pulls lose and nothing is written (no harm done).
typedef unsigned int u32;
typedef unsigned char u8;
#define PAD_CFG(i) (*(volatile u32 *)(0xF02004 + 4 * (i)))
#define TXF_LEVEL  (*(volatile u32 *)0xF2A0B0)
#define P_NSS  1
#define P_SCK  2
#define P_MOSI 3

static void pad(u32 i, u32 v) { PAD_CFG(i) = (PAD_CFG(i) & ~0x32u) | ((v ? 2u : 1u) << 4); }

__attribute__((noinline)) static void wait(u32 n) {
  if (n) __asm__ volatile("mov lp_count,%0\n\tlp 1f\n\tnop_s\n1:" :: "r"(n) : "lp_count", "memory");
}

// Writes d[0..n) into the TX FIFO. hb = half-bit delay in cycles (6 works). Returns the TX FIFO
// level afterwards (n if the write landed in an empty FIFO).
u32 fifo_write(const u8 *d, u32 n, u32 hb) {
  u32 cfg[4];
  for (u32 i = 1; i <= 3; i++) cfg[i] = PAD_CFG(i);
  pad(P_NSS, 1); pad(P_SCK, 0); pad(P_MOSI, 0);
  wait(hb);
  pad(P_NSS, 0);
  wait(hb);
  for (u32 k = 0; k < n + 2; k++) {
    u32 b = k == 0 ? 0x00 : k == 1 ? 0x02 : d[k - 2];
    for (u32 bit = 0; bit < 8; bit++) {
      pad(P_MOSI, (b >> (7 - bit)) & 1);
      wait(hb);
      pad(P_SCK, 1);
      wait(hb);
      pad(P_SCK, 0);
    }
  }
  wait(hb);
  pad(P_NSS, 1);
  wait(hb);
  for (u32 i = 1; i <= 3; i++) PAD_CFG(i) = cfg[i];
  return TXF_LEVEL & 0x7FF;
}
