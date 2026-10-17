/* =====================================================================
   battery.h — filtered 2S LiPo fuel gauge.

   A voltage-only SoC on a 2S pack swings ~15 % every time the modem
   transmits, because a 2 A burst sags the pack ~240 mV. So the reading
   goes through four stages:

     1. median-of-5   kills single-sample spikes from the INA226
     2. EMA           smooths what's left
     3. coulomb count integrates current against a known 2500 mAh
     4. OCV fusion    nudges the integral back toward the voltage curve,
                      but ONLY while the pack is resting

   Coulomb counting alone drifts (every mA of offset error accumulates
   forever); OCV alone is noisy under load. Each fixes the other's
   failure mode, which is why both are here.
   ===================================================================== */
#pragma once
#include <stdint.h>

struct BattState {
  float v        = 0;      // filtered pack volts
  float ma       = 0;      // filtered current, +ve = charging
  float vRaw     = 0;      // unfiltered, published for diagnostics
  float maRaw    = 0;
  float mah      = 0;      // coulomb-counted charge remaining
  int   soc      = 0;      // 0..100, the fused answer
  int   socOcv   = 0;      // what the voltage curve alone says
  bool  charging = false;
  bool  resting  = false;  // quiet enough to trust the OCV curve
  bool  present  = false;  // INA226 answered
  const char* src = "init"; // "init" | "ocv" | "fused"
};

bool  battBegin();                 // INA226 + restore state from NVS
void  battTick();                  // sample, filter, integrate, correct
void  battSave();                  // persist (called automatically too)
const BattState& battGet();

/* pure helpers — exposed so selfCheck (and a host compiler) can test them */
int   socFromCell(float vcell);
float medianOf(const float* buf, int n);
float coulombStep(float mah, float ma, uint32_t dtMs, float capMah);
bool  battSelfCheck();             // prints PASS/FAIL over serial
