#include "indicators.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

static Adafruit_NeoPixel px(LED_COUNT, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
static uint16_t conds   = IND_BOOT;
static bool     muted   = false;
static uint32_t findUntil = 0;

/* ---------------------------------------------------------------- LED */
struct Pattern { uint8_t r, g, b; uint16_t onMs, offMs; bool breathe; };

/* index must follow the IndCond bit order */
static const Pattern PATTERNS[] = {
  {255,   0,   0, 150,  150, false},  // FATAL      red, non-stop
  {255,   0, 255, 200,  200, false},  // FENCE_OUT  magenta
  {255, 255, 255,  60,   60, false},  // FIND       white strobe
  {255,   0,   0, 250,  250, false},  // BATT_CRIT  red fast
  {255,   0,   0, 120,  880, false},  // BATT_LOW   red once per second
  {255, 140,   0, 120,  880, false},  // OFFLINE    amber once per second
  {  0, 200, 255, 120, 1880, false},  // NO_FIX     cyan once per 2 s
  {  0, 255,  80,   0,    0, true },  // CHARGING   green breathe
  {255, 255, 255,   0,    0, true },  // BOOT       white breathe
};
static const Pattern PAT_OK = {0, 255, 80, 60, 2940, false};  // healthy heartbeat

static const Pattern& activePattern() {
  for (uint8_t i = 0; i < sizeof(PATTERNS) / sizeof(PATTERNS[0]); i++)
    if (conds & (1u << i)) return PATTERNS[i];
  return PAT_OK;
}

/* ---------------------------------------------------------------- buzzer
   A tone sequence = n bursts of (freq, onMs) separated by offMs, then a
   gap before the sequence repeats. freq is ignored on an active buzzer. */
struct Tone { uint16_t freq, onMs, offMs; uint8_t bursts; uint16_t gapMs; };

static const Tone TONE_FENCE = {2700, 120, 100, 3, 1800};  // 3 beeps / 2 s
static const Tone TONE_FIND  = {4000, 600, 250, 2,  400};  // near-continuous warble
static const Tone TONE_FATAL = {1000, 300, 200, 2, 1200};  // two low beeps
static const Tone TONE_BATT  = {3000, 80,    0, 1, BATT_CHIRP_MS};
static const Tone TONE_NONE  = {0, 0, 0, 0, 0};

static const Tone& activeTone() {
  if (conds & IND_FATAL)     return TONE_FATAL;
  if (conds & IND_FENCE_OUT) return TONE_FENCE;
  if (conds & IND_FIND)      return TONE_FIND;
  if ((conds & (IND_BATT_LOW | IND_BATT_CRIT)) && BATT_CHIRP_MS) return TONE_BATT;
  return TONE_NONE;
}

static void buzzOn(uint16_t freq) {
  if (muted) return;
#if BUZZER_PASSIVE
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(PIN_BUZZER, freq);
  #else
    ledcWriteTone(0, freq);
  #endif
#else
  (void)freq;
  digitalWrite(PIN_BUZZER, HIGH);
#endif
}
static void buzzOff() {
#if BUZZER_PASSIVE
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(PIN_BUZZER, 0);
  #else
    ledcWriteTone(0, 0);
  #endif
#else
  digitalWrite(PIN_BUZZER, LOW);
#endif
}

/* ---------------------------------------------------------------- api */
void indBegin() {
  px.begin();
  px.setBrightness(LED_BRIGHTNESS);
  px.clear();
  px.show();
#if BUZZER_PASSIVE
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_BUZZER, 2700, 10);
  #else
    ledcSetup(0, 2700, 10);
    ledcAttachPin(PIN_BUZZER, 0);
  #endif
#else
  pinMode(PIN_BUZZER, OUTPUT);
#endif
  buzzOff();
}

void indRaise(uint16_t c) { conds |= c; }
void indClear(uint16_t c) { conds &= ~c; }
bool indActive(uint16_t c) { return conds & c; }
void indMute(bool on) { muted = on; if (on) buzzOff(); }

void indFind(uint32_t seconds) {
  if (!seconds) { findUntil = 0; indClear(IND_FIND); return; }
  findUntil = millis() + seconds * 1000UL;
  indRaise(IND_FIND);
}

/* one blocking chirp is acceptable exactly once, at boot */
void indChirpOk() {
  if (muted) return;
  buzzOn(3000); delay(70); buzzOff();
}

void indTick() {
  const uint32_t now = millis();

  if ((conds & IND_FIND) && now > findUntil) indClear(IND_FIND);

  /* ---- LED ---- */
  static uint32_t ledMark = 0;
  static bool     ledOn   = false;
  const Pattern&  p       = activePattern();

  if (p.breathe) {
    if (now - ledMark >= 20) {               // ~50 Hz refresh, not every loop
      ledMark = now;
      /* 2 s triangle wave; reads as "alive" rather than "alarm" */
      const uint16_t phase = now % 2000;
      const uint8_t  k     = phase < 1000 ? phase * 255 / 1000
                                          : (2000 - phase) * 255 / 1000;
      px.setPixelColor(0, px.Color(p.r * k / 255, p.g * k / 255, p.b * k / 255));
      px.show();
      ledOn = false;
    }
  } else {
    const uint16_t want = ledOn ? p.onMs : p.offMs;
    if (now - ledMark >= want) {
      ledMark = now;
      ledOn   = !ledOn;
      px.setPixelColor(0, ledOn ? px.Color(p.r, p.g, p.b) : 0);
      px.show();
    }
  }

  /* ---- buzzer ---- */
  static const Tone* curTone = &TONE_NONE;
  static uint32_t    bzMark  = 0;
  static uint8_t     burst   = 0;
  static bool        bzOn    = false;
  const Tone*        t       = &activeTone();

  if (t != curTone) {                       // pattern changed: restart cleanly
    curTone = t; burst = 0; bzOn = false; buzzOff();
    bzMark  = now - t->gapMs;              // fire the first burst immediately
  }
  if (!t->bursts) { if (bzOn) { buzzOff(); bzOn = false; } return; }

  if (bzOn) {
    if (now - bzMark >= t->onMs) {
      buzzOff(); bzOn = false; bzMark = now;
      if (++burst >= t->bursts) burst = 0;   // 0 => next wait is the long gap
    }
  } else {
    const uint16_t wait = burst ? t->offMs : t->gapMs;
    if (now - bzMark >= wait) { buzzOn(t->freq); bzOn = true; bzMark = now; }
  }
}
