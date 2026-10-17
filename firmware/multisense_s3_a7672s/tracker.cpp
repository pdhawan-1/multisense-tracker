#include "tracker.h"
#include "indicators.h"
#include <Arduino.h>
#include <stdarg.h>          // push() is variadic
#include <ArduinoJson.h>
#include <Preferences.h>

static Preferences tnvs;
static Zone  Z[FENCE_MAX_ZONES];
static Trip  TR;
static double odoKm = 0;

/* ---------------------------------------------------------------- events */
static TrkEvent evQ[8];
static uint8_t  evHead = 0, evCount = 0;

static void push(TrkEventType t, const GnssFix& f, const char* fmt, ...) {
  TrkEvent& e = evQ[(evHead + evCount) % 8];
  e.type = t;
  e.lat = f.lat; e.lon = f.lon; e.speed_kmh = f.speed_kmh;
  va_list ap; va_start(ap, fmt);
  vsnprintf(e.detail, sizeof e.detail, fmt, ap);
  va_end(ap);
  if (evCount < 8) evCount++; else evHead = (evHead + 1) % 8;  // drop oldest
  Serial.printf("EVENT %u: %s\n", t, e.detail);
}

bool trkTakeEvent(TrkEvent& out) {
  if (!evCount) return false;
  out = evQ[evHead];
  evHead = (evHead + 1) % 8;
  evCount--;
  return true;
}

/* ---------------------------------------------------------------- maths */
static double haversine(double la1, double lo1, double la2, double lo2) {
  const double R = 6371000.0, d2r = PI / 180.0;
  const double dLat = (la2 - la1) * d2r, dLon = (lo2 - lo1) * d2r;
  const double a = sin(dLat / 2) * sin(dLat / 2) +
                   cos(la1 * d2r) * cos(la2 * d2r) * sin(dLon / 2) * sin(dLon / 2);
  return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

/* km/h lost per second, expressed in g. 1 g = 35.3 km/h/s. */
static float decelG(float kmhFrom, float kmhTo, uint32_t dtMs) {
  if (dtMs == 0 || kmhTo >= kmhFrom) return 0;
  const float dv_kmh_per_s = (kmhFrom - kmhTo) / (dtMs / 1000.0f);
  return dv_kmh_per_s / 35.3f;
}

static bool fixTrusted(const GnssFix& f) {
  return f.valid && f.sats >= FENCE_MIN_SATS && f.hdop <= FENCE_MAX_HDOP;
}

/* ---------------------------------------------------------------- NVS */
static void save() {
  tnvs.begin("trk", false);
  tnvs.putDouble("odo", odoKm);
  for (int i = 0; i < FENCE_MAX_ZONES; i++) {
    char k[8];
    snprintf(k, sizeof k, "z%de", i); tnvs.putBool(k, Z[i].enabled);
    snprintf(k, sizeof k, "z%da", i); tnvs.putDouble(k, Z[i].lat);
    snprintf(k, sizeof k, "z%do", i); tnvs.putDouble(k, Z[i].lon);
    snprintf(k, sizeof k, "z%dr", i); tnvs.putFloat(k, Z[i].r);
    snprintf(k, sizeof k, "z%dn", i); tnvs.putString(k, Z[i].name);
  }
  tnvs.end();
}

void trkBegin() {
  tnvs.begin("trk", true);
  odoKm = tnvs.getDouble("odo", 0);
  for (int i = 0; i < FENCE_MAX_ZONES; i++) {
    char k[8];
    snprintf(k, sizeof k, "z%de", i); Z[i].enabled = tnvs.getBool(k, false);
    snprintf(k, sizeof k, "z%da", i); Z[i].lat = tnvs.getDouble(k, 0);
    snprintf(k, sizeof k, "z%do", i); Z[i].lon = tnvs.getDouble(k, 0);
    snprintf(k, sizeof k, "z%dr", i); Z[i].r   = tnvs.getFloat(k, 0);
    snprintf(k, sizeof k, "z%dn", i);
    String nm = tnvs.getString(k, "");
    strncpy(Z[i].name, nm.c_str(), sizeof(Z[i].name) - 1);
  }
  tnvs.end();

  Serial.printf("tracker: odometer %.2f km, zones:", odoKm);
  for (int i = 0; i < FENCE_MAX_ZONES; i++)
    if (Z[i].enabled) Serial.printf(" [%s %.0fm]", Z[i].name[0] ? Z[i].name : "zone", Z[i].r);
  Serial.println();
}

/* ---------------------------------------------------------------- fence cfg
   Accepts either the single-zone form (back-compatible) or an array:
     {"zones":[{"name":"home","lat":..,"lon":..,"radius_m":200}, ...]}
   An absent or empty array disarms everything.                       */
bool trkApplyFenceJson(const char* json) {
  JsonDocument d;
  if (deserializeJson(d, json)) return false;

  for (int i = 0; i < FENCE_MAX_ZONES; i++) {
    Z[i].enabled = false;
    Z[i].known   = false;
    Z[i].outside = false;
  }

  if (d["zones"].is<JsonArray>()) {
    int i = 0;
    for (JsonObject z : d["zones"].as<JsonArray>()) {
      if (i >= FENCE_MAX_ZONES) break;
      const float r = z["radius_m"] | 0.0f;
      if (r <= 0) continue;
      Z[i].enabled = true;
      Z[i].lat = z["lat"] | 0.0;
      Z[i].lon = z["lon"] | 0.0;
      Z[i].r   = r;
      strncpy(Z[i].name, z["name"] | "zone", sizeof(Z[i].name) - 1);
      Z[i].name[sizeof(Z[i].name) - 1] = 0;
      i++;
    }
  } else if ((d["enabled"] | false) && (d["radius_m"] | 0.0f) > 0) {
    Z[0].enabled = true;
    Z[0].lat = d["lat"] | 0.0;
    Z[0].lon = d["lon"] | 0.0;
    Z[0].r   = d["radius_m"] | 0.0f;
    strncpy(Z[0].name, "zone 1", sizeof(Z[0].name) - 1);
  }

  indClear(IND_FENCE_OUT);
  save();
  Serial.print("fence config applied:");
  for (int i = 0; i < FENCE_MAX_ZONES; i++)
    if (Z[i].enabled)
      Serial.printf(" [%s %.5f,%.5f r=%.0f]", Z[i].name, Z[i].lat, Z[i].lon, Z[i].r);
  Serial.println();
  return true;
}

/* ---------------------------------------------------------------- geofence */
static void fenceCheck(const GnssFix& f) {
  if (!fixTrusted(f)) return;
  uint8_t outside = 0;

  for (int i = 0; i < FENCE_MAX_ZONES; i++) {
    if (!Z[i].enabled) continue;
    const double dist = haversine(f.lat, f.lon, Z[i].lat, Z[i].lon);
    /* hysteresis: leave at r+H, return at r-H, so sitting on the line
       does not machine-gun the buzzer and the alerts */
    const bool out = Z[i].outside ? (dist > Z[i].r - FENCE_HYST_M)
                                  : (dist > Z[i].r + FENCE_HYST_M);
    if (!Z[i].known) { Z[i].known = true; Z[i].outside = out; }
    else if (out != Z[i].outside) {
      Z[i].outside = out;
      push(out ? EV_FENCE_EXIT : EV_FENCE_ENTER, f,
           "%s zone '%s' (%.0f m from centre, r=%.0f m)",
           out ? "LEFT" : "entered", Z[i].name, dist, Z[i].r);
    }
    if (Z[i].outside) outside++;
  }

  /* The alarm means "outside EVERY armed zone" — with several zones,
     being inside any one of them counts as being somewhere allowed. */
  uint8_t armed = 0;
  for (int i = 0; i < FENCE_MAX_ZONES; i++) if (Z[i].enabled) armed++;
  if (armed && outside == armed) indRaise(IND_FENCE_OUT);
  else                           indClear(IND_FENCE_OUT);
}

uint8_t trkZonesOutside() {
  uint8_t n = 0;
  for (int i = 0; i < FENCE_MAX_ZONES; i++) if (Z[i].enabled && Z[i].outside) n++;
  return n;
}

/* ---------------------------------------------------------------- trips */
static uint32_t tAbove = 0, tBelow = 0, tLastMove = 0;
static double   lastLat = 0, lastLon = 0;
static bool     haveLast = false;

static void tripCheck(const GnssFix& f, uint32_t now) {
#if FEAT_TRIPS
  if (!fixTrusted(f)) return;

  if (f.speed_kmh >= TRIP_START_KMH) {
    if (!tAbove) tAbove = now;
    tBelow = 0;
    if (!TR.active && (now - tAbove) >= TRIP_START_MS) {
      TR = Trip();
      TR.active = true;
      TR.startMs = now;
      TR.lat0 = f.lat; TR.lon0 = f.lon;
      push(EV_TRIP_START, f, "trip started at %.1f km/h", f.speed_kmh);
    }
  } else {
    tAbove = 0;
    if (TR.active && f.speed_kmh <= TRIP_STOP_KMH) {
      if (!tBelow) tBelow = now;
      if ((now - tBelow) >= TRIP_STOP_MS) {
        push(EV_TRIP_END, f, "trip ended: %.2f km in %lu min, max %.1f km/h",
             TR.km, (unsigned long)((now - TR.startMs) / 60000), TR.maxKmh);
        TR.active = false;
        save();
      }
    } else tBelow = 0;
  }

  /* distance: gate out GNSS jitter, which would otherwise accumulate
     kilometres while the device sits on a desk */
  if (haveLast) {
    const double step = haversine(lastLat, lastLon, f.lat, f.lon);
    if (step >= 5.0 && step < 500.0) {        // 500 m in 1 s = bad fix
      odoKm += step / 1000.0;
      if (TR.active) TR.km += step / 1000.0;
      lastLat = f.lat; lastLon = f.lon;
      tLastMove = now;
    }
  } else { lastLat = f.lat; lastLon = f.lon; haveLast = true; tLastMove = now; }

  if (TR.active && f.speed_kmh > TR.maxKmh) TR.maxKmh = f.speed_kmh;
#endif
}

/* ---------------------------------------------------------------- accident */
static float    prevKmh = 0, peakKmh = 0;
static uint32_t tPrev = 0, tDecel = 0, tStill = 0, tRearm = 0, tCrash = 0;
static bool     crashActive = false, decelSeen = false;
static float    crashFrom = 0, crashTo = 0, crashG = 0;

static void crashCheck(const GnssFix& f, uint32_t now) {
#if FEAT_CRASH
  if (!f.valid) return;
  const float kmh = f.speed_kmh;

  if (tPrev && now > tPrev) {
    const float g = decelG(prevKmh, kmh, now - tPrev);

    /* stage 1: were we actually travelling, and did the speed collapse
       harder than a human brakes? */
    if (!decelSeen && !crashActive && now >= tRearm &&
        prevKmh >= CRASH_MIN_SPEED_KMH && g >= CRASH_DECEL_G) {
      decelSeen = true;
      tDecel    = now;
      tStill    = 0;
      crashFrom = prevKmh;
      crashTo   = kmh;
      crashG    = g;
      Serial.printf("harsh decel: %.1f -> %.1f km/h = %.2f g, watching for standstill\n",
                    crashFrom, crashTo, g);
    }
  }

  /* stage 2: did it then stay stopped? A car that brakes hard and drives
     on was an emergency stop, not a crash. */
  if (decelSeen) {
    if (kmh <= CRASH_STILL_KMH) {
      if (!tStill) tStill = now;
      if ((now - tStill) >= CRASH_STILL_MS) {
        crashActive = true;
        decelSeen   = false;
        tCrash      = now;
        tRearm      = now + CRASH_REARM_MS;
        indRaise(IND_ACCIDENT);
        push(EV_ACCIDENT, f,
             "possible accident: %.1f -> %.1f km/h at %.2f g, stopped %lu s; "
             "sats=%d hdop=%.1f",
             crashFrom, crashTo, crashG,
             (unsigned long)(CRASH_STILL_MS / 1000), f.sats, f.hdop);
      }
    } else {
      tStill = 0;
      if ((now - tDecel) > CRASH_WINDOW_MS) {
        decelSeen = false;                 // moved on: emergency stop only
        Serial.println("decel window expired without a standstill — no alert");
      }
    }
  }

  /* stage 3: if it drives off soon after, say so — a stuck "accident"
     banner on the dashboard is worse than no banner. */
  if (crashActive && kmh > CRASH_STILL_KMH * 2 && (now - tCrash) <= CRASH_CANCEL_MS) {
    crashActive = false;
    indClear(IND_ACCIDENT);
    push(EV_ACCIDENT_CLEARED, f, "moving again at %.1f km/h — alert cleared", kmh);
  }

  prevKmh = kmh;
  tPrev   = now;
#endif
}

bool trkAccidentActive() { return crashActive; }

/* ---------------------------------------------------------------- public */
void trkOnFix(const GnssFix& f) {
  const uint32_t now = millis();
  fenceCheck(f);
  tripCheck(f, now);
  crashCheck(f, now);
}

bool trkMoving() { return TR.active || (millis() - tLastMove) < PARKED_AFTER_MS / 10; }
bool trkParked() { return tLastMove && (millis() - tLastMove) >= PARKED_AFTER_MS; }
double trkOdometerKm() { return odoKm; }
const Trip& trkTrip() { return TR; }
const Zone* trkZones() { return Z; }

uint32_t trkReportInterval(bool emergency) {
#if FEAT_EMERGENCY
  if (emergency) return REPORT_MS_EMERGENCY;
#endif
#if FEAT_ADAPTIVE
  if (crashActive)  return REPORT_MS_MOVING;      // stay chatty after a crash
  if (TR.active)    return REPORT_MS_MOVING;
  if (trkParked())  return REPORT_MS_PARKED;
  return REPORT_MS_IDLE;
#else
  return REPORT_MS_DEFAULT;
#endif
}

/* ---------------------------------------------------------------- self-check */
bool trkSelfCheck() {
  bool ok = true;

  /* haversine: TIET Patiala -> Chandigarh is ~57 km */
  const double d = haversine(30.3540, 76.3637, 30.7333, 76.7794) / 1000.0;
  if (d < 56.0 || d > 59.0) ok = false;

  /* the crash discriminator is the whole feature. These three MUST
     classify correctly or the device cries wolf at every traffic light. */
  const float gLight = decelG(40.0f, 0.0f, 5000);   // red light, 5 s
  const float gHard  = decelG(40.0f, 0.0f, 1000);   // one GNSS sample
  const float gCrash = decelG(60.0f, 0.0f, 1000);
  if (gLight >= CRASH_DECEL_G) ok = false;          // must NOT trigger
  if (gHard  <  CRASH_DECEL_G) ok = false;          // must trigger
  if (gCrash <  CRASH_DECEL_G) ok = false;

  Serial.printf("tracker self-check: %s  (patiala->chd=%.1f km, "
                "red-light=%.2fg ignored, hard-stop=%.2fg fires, threshold=%.2fg)\n",
                ok ? "PASS" : "FAIL", d, gLight, gHard, CRASH_DECEL_G);
  return ok;
}
