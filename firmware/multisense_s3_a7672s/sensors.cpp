#include "sensors.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AS7341.h>
#include <SparkFun_VL53L5CX_Library.h>

static Adafruit_AS7341    as7341;
static SparkFun_VL53L5CX  tof;
static VL53L5CX_ResultsData raw;

static TofFrame  T;
static SpecFrame P;
static bool tofOk = false, specOk = false;
static uint16_t recoveries = 0;
static uint8_t  tofFails = 0;

/* ---------------------------------------------------------------- ISRs
   Interrupt handlers do nothing but set a flag — all I2C work happens
   in sensTick() on the main loop, because the Wire driver is not
   interrupt-safe. */
static volatile bool fTof = false, fLight = false, fIna = false;
static bool lightPending = false;   // latched for the main loop to consume

static void IRAM_ATTR isrTof()   { fTof   = true; }
static void IRAM_ATTR isrLight() { fLight = true; }
static void IRAM_ATTR isrIna()   { fIna   = true; }

/* ---------------------------------------------------------------- INA226 alert
   Written as raw register pokes rather than through the library so this
   does not depend on which INA226 library version you have installed.
   Reg 0x06 = Mask/Enable, bit 12 (0x1000) = Bus Under-Voltage.
   Reg 0x07 = Alert Limit, bus-voltage LSB = 1.25 mV.                  */
static void inaAlertConfig(float underVolts) {
  const uint16_t limit = (uint16_t)(underVolts / 0.00125f);
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(0x07);
  Wire.write(limit >> 8); Wire.write(limit & 0xFF);
  Wire.endTransmission();

  Wire.beginTransmission(INA226_ADDR);
  Wire.write(0x06);
  Wire.write(0x10); Wire.write(0x00);      // BUL, latching off
  Wire.endTransmission();
}

/* ---------------------------------------------------------------- ToF reset
   The VL53L5CX's own pin is I2C_RST: a HIGH pulse resets its I2C
   interface and it idles LOW. Some breakouts instead bring out an
   active-LOW enable, where idling LOW holds the sensor OFF and the
   sensor never answers. If your board is one of those, set
   TOF_RST_ACTIVE_HIGH to 0 in config.h and this inverts. */
static void tofHwReset() {
#if PIN_TOF_RST >= 0
  #if TOF_RST_ACTIVE_HIGH
    digitalWrite(PIN_TOF_RST, HIGH); delay(20);
    digitalWrite(PIN_TOF_RST, LOW);
  #else
    digitalWrite(PIN_TOF_RST, LOW);  delay(20);
    digitalWrite(PIN_TOF_RST, HIGH);
  #endif
  delay(1300);                       // datasheet boot time, don't rush it
#endif
}

/* ---------------------------------------------------------------- I2C rescue
   A slave that was interrupted mid-byte can hold SDA low forever. The
   only way out is to clock SCL by hand until it lets go, then re-init. */
static void i2cRecover() {
  recoveries++;
  Serial.printf("I2C wedged — recovery #%u\n", recoveries);

  Wire.end();
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
    digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
  }
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);

#if PIN_TOF_RST >= 0
  tofHwReset();                      // reset the likely culprit
#endif

  tofOk = tof.begin();
  if (tofOk) {
    tof.setResolution(8 * 8);
    tof.setRangingFrequency(15);
    tof.startRanging();
  }
  specOk = as7341.begin();
  if (specOk) {
    as7341.setATIME(100);
    as7341.setASTEP(999);
    as7341.setGain(AS7341_GAIN_64X);
  }
  inaAlertConfig(BATT_CELLS * 3.1f);
  tofFails = 0;
  Serial.printf("recovery done: tof=%d spectral=%d\n", tofOk, specOk);
}

/* ---------------------------------------------------------------- begin */
bool sensBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);

#if PIN_TOF_RST >= 0
  pinMode(PIN_TOF_RST, OUTPUT);
  tofHwReset();
#endif

  specOk = as7341.begin();
  if (specOk) {
    as7341.setATIME(100);
    as7341.setASTEP(999);
    as7341.setGain(AS7341_GAIN_64X);
    /* INT when CLEAR leaves this window — i.e. the light changed a lot.
       Useful on its own, and the hook a tamper feature would use. */
    as7341.setLowThreshold(200);
    as7341.setHighThreshold(30000);
    as7341.setSpectralThresholdChannel(AS7341_ADC_CHANNEL_4);
    as7341.enableSpectralInterrupt(true);
  } else Serial.println("AS7341 not found");

  tofOk = tof.begin();
  if (tofOk) {
    tof.setResolution(8 * 8);
    tof.setRangingFrequency(15);
    tof.startRanging();
  } else Serial.println("VL53L5CX not found");

  inaAlertConfig(BATT_CELLS * 3.1f);   // ~6.2 V on a 2S pack

#if PIN_TOF_INT >= 0
  pinMode(PIN_TOF_INT, INPUT_PULLUP);
  attachInterrupt(PIN_TOF_INT, isrTof, FALLING);
#endif
#if PIN_SPEC_INT >= 0
  pinMode(PIN_SPEC_INT, INPUT_PULLUP);
  attachInterrupt(PIN_SPEC_INT, isrLight, FALLING);
#endif
#if PIN_INA_ALERT >= 0
  pinMode(PIN_INA_ALERT, INPUT_PULLUP);
  attachInterrupt(PIN_INA_ALERT, isrIna, FALLING);
#endif

  return tofOk && specOk;
}

/* ---------------------------------------------------------------- tick */
static void readTof() {
  if (!tof.getRangingData(&raw)) {
    if (++tofFails >= 5) i2cRecover();
    return;
  }
  tofFails = 0;

  uint16_t lo = 65535, hi = 0;
  uint8_t  valid = 0;
  for (int i = 0; i < 64; i++) {
    T.mm[i] = raw.distance_mm[i];
    T.st[i] = raw.target_status[i];
    if (T.st[i] == 5 || T.st[i] == 6 || T.st[i] == 9) {   // 5/6/9 = valid
      valid++;
      const uint16_t mm = (uint16_t)T.mm[i];
      if (mm < lo) lo = mm;
      if (mm > hi) hi = mm;
    }
  }
  T.validZones = valid;
  T.minMm = (lo == 65535) ? 0 : lo;
  T.maxMm = hi;
  /* centre of view = mean of the middle 2x2 zones of the 8x8 grid */
  T.centre = (uint16_t)((raw.distance_mm[27] + raw.distance_mm[28] +
                         raw.distance_mm[35] + raw.distance_mm[36]) / 4);
  T.fresh = true;
}

void sensTick() {
  if (tofOk) {
    /* INT is the normal path; the isDataReady() poll is a safety net in
       case the INT line is missed or not actually connected. */
    if (fTof) { fTof = false; readTof(); }
    else {
      static uint32_t tPoll = 0;
      if (millis() - tPoll > 200) { tPoll = millis(); if (tof.isDataReady()) readTof(); }
    }
  }

  if (specOk) {
    static uint32_t tSpec = 0;
    if (millis() - tSpec >= SENSOR_MS) {
      tSpec = millis();
      if (as7341.readAllChannels(P.ch)) {
        /* ponytail: one scale factor, not a full AS7341 lux model —
           calibrate against a reference meter, see docs/SETUP.md */
        P.lux   = P.ch[10] * 0.164f;
        P.valid = true;
      } else P.valid = false;
    }
  }

  if (fLight) {
    fLight = false;
    lightPending = true;              // latch BEFORE clearing the device flag
    as7341.clearInterruptStatus();
  }
}

bool sensTofOk()  { return tofOk; }
bool sensSpecOk() { return specOk; }
const TofFrame&  sensTof()  { return T; }
const SpecFrame& sensSpec() { return P; }

bool sensTakeLightAlert() {
  if (!lightPending) return false;
  lightPending = false;
  return true;
}

bool sensTakeInaAlert() {
  if (!fIna) return false;
  fIna = false;
  return true;
}

uint16_t sensI2cRecoveries() { return recoveries; }
