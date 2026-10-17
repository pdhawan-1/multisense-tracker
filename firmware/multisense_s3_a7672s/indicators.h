/* =====================================================================
   indicators.h — WS2812B status LED + buzzer, both fully non-blocking.
   Nothing here ever calls delay(): the MQTT keepalive and the ToF
   ranging loop must keep running while the thing is beeping.

   One rule: conditions are raised/cleared by the sketch, and tick()
   renders only the HIGHEST-PRIORITY active one. See the table in
   docs/SETUP.md for the maker-facing legend.
   ===================================================================== */
#pragma once
#include <stdint.h>

/* highest priority first — the order of this enum IS the priority */
enum IndCond : uint16_t {
  IND_FATAL     = 1 << 0,  // sensor/modem init failed: red, non-stop 150 ms
  IND_ACCIDENT  = 1 << 1,  // crash suspected: red/white strobe + siren
  IND_FENCE_OUT = 1 << 2,  // outside every armed zone: magenta + triple beep
  IND_FIND      = 1 << 3,  // find-device: white strobe + warble
  IND_BATT_CRIT = 1 << 4,  // <10 %: red 250 ms
  IND_BATT_LOW  = 1 << 5,  // <20 %: red 1 s
  IND_OFFLINE   = 1 << 6,  // no MQTT/network: amber 1 s
  IND_NO_FIX    = 1 << 7,  // online but no GNSS fix: cyan 2 s
  IND_CHARGING  = 1 << 8,  // green breathe
  IND_BOOT      = 1 << 9,  // white breathe
};

void indBegin();
void indRaise(uint16_t cond);
void indClear(uint16_t cond);
bool indActive(uint16_t cond);
void indFind(uint32_t seconds);   // arms IND_FIND for n seconds (0 = cancel)
void indMute(bool on);            // silences the buzzer, LED unaffected
void indBrightness(uint8_t b);    // dimmed in emergency mode to save power
void indChirpOk();                // one short confirmation chirp
void indTick();                   // call every loop()
