#pragma once

// Board support for the two LR2021 test boards: pins, power-up, and a RadioLib LR2021 subclass
// with the raw-access helpers the test firmware uses. Pick the board with a build flag:
//   -D BOARD_XIAO_WIO_LR2021        Seeed XIAO nRF52840 + Seeed Wio-LR2021 carrier
//   -D BOARD_RAK3401_NICERF_LR2021  RAK3401 core + RAK19007 base + NiceRF LoRa2021F33-2G4 carrier
//
// Everything a program needs: board.begin(), then radio_init(), then use `radio`.

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#if defined(BOARD_XIAO_WIO_LR2021)
  #define P_LORA_BUSY  D1
  #define P_LORA_NSS   D2
  #define P_LORA_SCLK  D3
  #define P_LORA_MOSI  D4
  #define P_LORA_MISO  D5
  #define P_LORA_RESET D6
  #define P_LORA_DIO_1 D8          // IRQ line, wired to LR2021 DIO8 (D7 -> DIO7 and D9 -> DIO11 unused)
  #define LR2021_IRQ_DIO 8
  #define LR2021_TCXO_VOLTAGE 1.6f
  #define LR2021_USE_SIMO 1        // run the radio from its SIMO DC-DC instead of the LDOs
  // Sub-GHz PA over-current threshold, 0..63 (chip default 41, ~164 mA). At 22 dBm this board
  // needs 42 into a 50 ohm load; 52 adds Semtech's recommended 25% margin.
  #define LR2021_OCP_LF 52
  #define BOARD_NAME "XIAO nRF52840 + Wio-LR2021"
#elif defined(BOARD_RAK3401_NICERF_LR2021)
  // P_LORA_* pins, P_LORA_CE and PIN_3V3_EN come from variants/rak3401_nicerf_lr2021/variant.h.
  // The module's IRQ pin (P0.10 / WB_IO6) is LR2021 DIO9.
  #define LR2021_IRQ_DIO 9
  // The module has a TCXO (it won't start at 0 V). NiceRF's demo uses 3.3 V, but the LR2021 runs
  // from the module's 3.3 V LDO, so 3.3 V has no headroom. 1.6/1.8/3.0 V all start cleanly.
  #define LR2021_TCXO_VOLTAGE 1.8f
  #define BOARD_NAME "RAK3401 + NiceRF LoRa2021F33-2G4"
  // Needs a battery: on USB power alone the RAK19007 charger can't supply the 2 W PA (the LR2021
  // browns out mid-TX at drive >= ~7 dBm and stops responding).
#else
  #error "define BOARD_XIAO_WIO_LR2021 or BOARD_RAK3401_NICERF_LR2021"
#endif

// Boot defaults: receive-only settings used throughout the reverse-engineering work (US MeshCore
// channel). Nothing is transmitted at boot.
#ifndef DEFAULT_FREQ_MHZ
  #define DEFAULT_FREQ_MHZ 910.525f
#endif
#ifndef DEFAULT_BW_KHZ
  #define DEFAULT_BW_KHZ 62.5f
#endif
#ifndef DEFAULT_SF
  #define DEFAULT_SF 7
#endif
#ifndef DEFAULT_CR
  #define DEFAULT_CR 5             // 4/5
#endif
#ifndef DEFAULT_TX_DBM
  #define DEFAULT_TX_DBM (-9)
#endif

class LR2021Dev : public LR2021 {
 public:
  explicit LR2021Dev(Module* mod);

  // begin() plus the board's regulator / OCP / RF-switch setup. Resets the chip, so it also undoes
  // raw-mode experiments (packet type 0x0B, FIFO routes, manual gain).
  bool init(float mhz, float bwKhz, uint8_t sf, uint8_t cr, int8_t dbm, uint16_t preamble = 16);

  // Raw access. memRead: n <= 32 words (reply words are big-endian on the wire).
  int16_t memRead(uint32_t addr, uint32_t* out, uint8_t n);
  int16_t memWrite(uint32_t addr, uint32_t val);
  // outLen == 0: write in[] as parameters; else send in[] then read outLen bytes.
  int16_t rawCmd(uint16_t op, const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen);
  int16_t getChipErrors(uint16_t* err);
  int16_t clearChipErrors();
  int16_t writeRegMask32(uint32_t addr, uint32_t mask, uint32_t data);
  // Sub-GHz PA over-current threshold (datasheet 23.7): unlock with 0xC0DE at 0xF40338, then write
  // bits [24:19] of 0xF40300. level 0..63; power-on default 41 (OCPLF typ. 164 mA, Table 3-22).
  int16_t setOcpLf(uint8_t level);
  int16_t setRegModeSimo();

  float getFreqMHz() const { return freqMHz; }
  float getBandwidthKHz() const { return bandwidthKhz; }
  uint8_t getSF() const { return spreadingFactor; }
  uint8_t getCR() const { return codingRate; }
};

struct Board {
  void begin();
  const char* getManufacturerName() const { return BOARD_NAME; }
};

extern Board board;
extern LR2021Dev radio;
bool radio_init();  // init() with the boot defaults above

#define RADIO_CLASS LR2021Dev
