/* =====================================================================
   sensors.h — AS7341 + VL53L5CX + I2C health.

   Three things this adds over polling everything in loop():
     · the ToF's INT pin signals data-ready, so we stop asking
     · the AS7341's INT pin reports a light-threshold crossing
     · a wedged I2C bus is recovered instead of being fatal till reboot
       (the VL53L5CX in particular can hang its I2C state machine, which
        is why its RST line on GPIO 5 is worth having)
   ===================================================================== */
#pragma once
#include <stdint.h>

struct TofFrame {
  int16_t  mm[64]  = {0};
  uint8_t  st[64]  = {0};
  uint16_t centre = 0, minMm = 0, maxMm = 0;
  uint8_t  validZones = 0;
  bool     fresh = false;      // a new frame arrived since the last packet
};

struct SpecFrame {
  uint16_t ch[12] = {0};       // F1..F4, Clear0, NIR0, F5..F8, Clear, NIR
  float    lux    = 0;
  bool     valid  = false;
};

bool sensBegin();
void sensTick();                    // service interrupts and read data
bool sensTofOk();
bool sensSpecOk();
const TofFrame&  sensTof();
const SpecFrame& sensSpec();

bool sensTakeLightAlert();          // true once per AS7341 threshold crossing
bool sensTakeInaAlert();            // true once per INA226 under-voltage alert
uint16_t sensI2cRecoveries();       // how many times the bus needed rescuing
