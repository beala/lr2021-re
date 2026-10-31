#include "LR2021Board.h"

Board board;
LR2021Dev radio(new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI));

// ---------------------------------------------------------------- PA tables

// Sub-GHz PA table, one entry per dBm from -9 to +22 (RADIOLIB_LR2021_PA_TABLE_LEN entries).
// paVal is TX_POWER in 0.5 dB units.
// +10..+22 dBm come from Semtech's LR2021/22/12 datasheet Rev 2.2, Table 7-19 (915 MHz reference
// design). RadioLib's default table tops out at TX_POWER = 17.5 dBm, so output stops rising above
// ~14 dBm (https://github.com/jgromes/RadioLib/pull/1909). -9..+9 dBm are RadioLib's defaults.
static LR2021PaTableEntry_t paTableLf[RADIOLIB_LR2021_PA_TABLE_LEN] = {
  { 1, 1, 8 },  { 2, 2, 1 },  { 2, 2, 3 },  { 2, 2, 5 },  { 1, 2, 13 }, { 2, 1, 13 },  // -9..-4
  { 2, 2, 11 }, { 2, 2, 13 }, { 3, 1, 12 }, { 1, 1, 18 }, { 1, 1, 20 }, { 1, 1, 23 },  // -3..+2
  { 1, 1, 27 }, { 1, 1, 33 }, { 1, 2, 26 }, { 1, 2, 31 }, { 1, 3, 27 }, { 1, 1, 37 },  // +3..+8
  { 1, 2, 40 },                                                                          // +9
  { 1, 2, 32 }, { 2, 4, 30 }, { 4, 2, 30 }, { 4, 2, 32 }, { 4, 2, 34 }, { 4, 5, 34 },  // +10..+15
  { 7, 3, 34 }, { 7, 3, 36 }, { 5, 7, 38 }, { 5, 7, 40 }, { 5, 6, 42 }, { 5, 6, 44 },  // +16..+21
  { 7, 7, 44 },                                                                          // +22
};

// 2.4 GHz PA table, one entry per dBm from -19 to +12. paDutyCycle is PA_HF_DUTY_CYCLE - 16
// (RadioLib adds 16 back). 0..+12 dBm from datasheet Rev 2.2 Table 7-21 (2445 MHz reference
// design); -19..-1 dBm are RadioLib's defaults.
#define HF(duty, half_dbm) { (duty) - 16, RADIOLIB_LR2021_PA_LF_SLICES_UNUSED, (half_dbm) }
static LR2021PaTableEntry_t paTableHf[RADIOLIB_LR2021_PA_TABLE_LEN] = {
  HF(16, -38), HF(16, -36), HF(16, -34), HF(16, -32), HF(16, -30), HF(16, -28), HF(16, -26),
  HF(16, -24), HF(16, -22), HF(16, -20), HF(16, -18), HF(16, -16), HF(16, -14), HF(16, -12),
  HF(16, -10), HF(16, -8),  HF(16, -6),  HF(16, -4),  HF(16, -2),                // -19..-1
  HF(28, 2), HF(28, 4), HF(27, 6), HF(27, 8), HF(27, 10), HF(27, 12), HF(27, 14),  // 0..+6
  HF(27, 16), HF(30, 20), HF(30, 22), HF(30, 24), HF(27, 24), HF(16, 24),          // +7..+12
};
#undef HF

LR2021Dev::LR2021Dev(Module* mod) : LR2021(mod) {
  irqDioNum = LR2021_IRQ_DIO;
#if defined(BOARD_RAK3401_NICERF_LR2021)
  // NiceRF's demo PA setup (LF PA, duty cycle 7, 6 slices, TxPower = 2 * dBm). Their datasheet
  // power table was measured with it, so "drive N dBm" == "register value 2N" there:
  //   drive 22 -> 29.8 dBm 724 mA, 19 -> ~29.3 dBm, 16 -> ~28.3 dBm, 13 -> ~26.5 dBm,
  //   7 -> ~21.4 dBm, 1 -> ~16.2 dBm (868/915 MHz). Above ~19 the PA is compressed.
  for (int i = 0; i < RADIOLIB_LR2021_PA_TABLE_LEN; i++) {  // entry i is -9 + i dBm
    paTableLf[i].paDutyCycle = 7;
    paTableLf[i].paSlices = 6;
    paTableLf[i].paVal = 2 * (i - 9);
  }
#endif
  setPaTable(paTableLf, false);  // before begin(), which applies the initial TX power
  setPaTable(paTableHf, true);
}

// ---------------------------------------------------------------- RF switches

#if defined(BOARD_RAK3401_NICERF_LR2021)
// LoRa2021F33-2G4 front-end control, from NiceRF's block diagram. The LR2021 drives these itself
// in RF-switch mode, per radio state:
//   DIO5: 2.4 GHz FEM LNA_EN   (HIGH = LNA on; LOW = bypass, -12 dB sensitivity)
//   DIO6: sub-GHz TXEN         (antenna switch to the 5 V 2 W PA)
//   DIO7: 2.4 GHz Power_EN     (PMOS switch feeding VCC to the 2.4 GHz FEM)
//   DIO8: 2.4 GHz TXEN         (FEM PA)
static const uint32_t rfswitchDios[Module::RFSWITCH_MAX_PINS] = {
  RADIOLIB_LR2021_DIO5, RADIOLIB_LR2021_DIO6, RADIOLIB_LR2021_DIO7, RADIOLIB_LR2021_DIO8, RADIOLIB_NC,
};
static const Module::RfSwitchMode_t rfswitchTable[] = {
  //                    DIO5  DIO6  DIO7  DIO8
  { LR2021::MODE_STBY,  { LOW,  LOW,  LOW,  LOW  } },  // everything off
  { LR2021::MODE_RX,    { LOW,  LOW,  LOW,  LOW  } },  // sub-GHz RX path, 2.4 GHz FEM unpowered
  { LR2021::MODE_TX,    { LOW,  HIGH, LOW,  LOW  } },  // sub-GHz TX through the 2 W PA
  { LR2021::MODE_RX_HF, { HIGH, LOW,  HIGH, LOW  } },  // 2.4 GHz FEM powered, LNA on
  { LR2021::MODE_TX_HF, { LOW,  LOW,  HIGH, HIGH } },  // 2.4 GHz FEM powered, PA on
  END_OF_MODE_TABLE,
};
#endif

// ---------------------------------------------------------------- init

bool LR2021Dev::init(float mhz, float bwKhz, uint8_t sf, uint8_t cr, int8_t dbm, uint16_t preamble) {
  float tcxo = LR2021_TCXO_VOLTAGE;
  int16_t st = begin(mhz, bwKhz, sf, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, dbm, preamble, tcxo);
  if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
    // no TCXO fitted: retry with the crystal
    st = begin(mhz, bwKhz, sf, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, dbm, preamble, 0.0f);
  }
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("# radio begin failed: %d\n", st);
    return false;
  }
#ifdef LR2021_USE_SIMO
  if (setRegModeSimo() != RADIOLIB_ERR_NONE) Serial.println("# SetRegMode(SIMO) failed, staying on LDO");
#endif
#ifdef LR2021_OCP_LF
  if (setOcpLf(LR2021_OCP_LF) != RADIOLIB_ERR_NONE) Serial.println("# setting OCP LF failed");
#endif
#if defined(BOARD_RAK3401_NICERF_LR2021)
  setRfSwitchTable(rfswitchDios, rfswitchTable);
#endif
  setCRC(2);
  explicitHeader();
  return true;
}

bool radio_init() {
  SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
  SPI.begin();
  return radio.init(DEFAULT_FREQ_MHZ, DEFAULT_BW_KHZ, DEFAULT_SF, DEFAULT_CR, DEFAULT_TX_DBM);
}

void Board::begin() {
  // nRF52 DC/DC converter (no SoftDevice running in these programs)
  NRF_POWER->DCDCEN = 1;
#if defined(BOARD_RAK3401_NICERF_LR2021)
  // WB_IO2 is both the base board's 3V3_S enable and the carrier's BOOST_EN (TPS61023,
  // VBAT -> ~5 V for the module's VCC / PAs). Keep it on.
  pinMode(PIN_3V3_EN, OUTPUT);
  digitalWrite(PIN_3V3_EN, HIGH);
  // CE enables the module's internal 3.3 V LDO, which powers the LR2021. It has an internal
  // pull-up, but drive it so we control the power sequence.
  pinMode(P_LORA_CE, OUTPUT);
  digitalWrite(P_LORA_CE, HIGH);
  delay(10);  // let the boost, LDO and LR2021 start before RadioLib resets it
#endif
}

// ---------------------------------------------------------------- raw access

int16_t LR2021Dev::memRead(uint32_t addr, uint32_t* out, uint8_t n) {
  if (n == 0 || n > 32) return RADIOLIB_ERR_SPI_CMD_INVALID;
  uint8_t req[4] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, n };
  uint8_t rpl[128];
  int16_t state = SPIcommand(RADIOLIB_LR2021_CMD_READ_REG_MEM_32, false, rpl, 4 * n, req, sizeof(req));
  for (uint8_t i = 0; i < n; i++)
    out[i] = ((uint32_t)rpl[4 * i] << 24) | ((uint32_t)rpl[4 * i + 1] << 16) | ((uint32_t)rpl[4 * i + 2] << 8) | rpl[4 * i + 3];
  return state;
}

int16_t LR2021Dev::memWrite(uint32_t addr, uint32_t val) {
  return writeCommon(RADIOLIB_LR2021_CMD_WRITE_REG_MEM_32, addr, &val, 1, false);
}

int16_t LR2021Dev::rawCmd(uint16_t op, const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
  if (outLen == 0) return SPIcommand(op, true, (uint8_t*)in, inLen);
  return SPIcommand(op, false, out, outLen, in, inLen);
}

int16_t LR2021Dev::getChipErrors(uint16_t* err) {
  uint8_t buff[2] = { 0 };
  int16_t state = SPIcommand(RADIOLIB_LR2021_CMD_GET_ERRORS, false, buff, sizeof(buff));
  if (err) *err = ((uint16_t)buff[0] << 8) | buff[1];
  return state;
}

int16_t LR2021Dev::clearChipErrors() {
  return SPIcommand(RADIOLIB_LR2021_CMD_CLEAR_ERRORS, true, NULL, 0);
}

int16_t LR2021Dev::writeRegMask32(uint32_t addr, uint32_t mask, uint32_t data) {
  uint8_t buff[11] = {
    (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
    (uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8), (uint8_t)mask,
    (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8), (uint8_t)data,
  };
  return SPIcommand(RADIOLIB_LR2021_CMD_WRITE_REG_MEM_MASK_32, true, buff, sizeof(buff));
}

int16_t LR2021Dev::setOcpLf(uint8_t level) {
  int16_t state = writeRegMask32(0x00F40338, 0x0000FFFF, 0xC0DE);
  if (state != RADIOLIB_ERR_NONE) return state;
  return writeRegMask32(0x00F40300, 0x3FUL << 19, ((uint32_t)(level & 0x3F)) << 19);
}

int16_t LR2021Dev::setRegModeSimo() {
  int16_t state = standby(RADIOLIB_LR2021_STANDBY_RC);
  if (state != RADIOLIB_ERR_NONE) return state;
  uint8_t simo = RADIOLIB_LR2021_REG_MODE_SIMO_NORMAL;
  return SPIcommand(RADIOLIB_LR2021_CMD_SET_REG_MODE, true, &simo, 1);
}
