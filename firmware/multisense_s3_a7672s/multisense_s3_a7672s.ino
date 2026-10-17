/* =====================================================================
   MultiSense Tracker — ESP32-S3 N16R8 + A7672S 4G + MQTT
   =====================================================================
   AS7341   spectral (10 ch)        I2C 0x39
   VL53L5CX 8x8 multizone ToF       I2C 0x29
   INA226   pack V/I fuel gauge     I2C 0x40   (R010 shunt, 2S Li-ion)
   A7672S   LTE Cat-1 + GNSS        UART1
   WS2812B  status LED              1 pixel
   buzzer   fence + find-device     LEDC tone

   Publishes the SAME JSON schema the dashboard already understands, so
   the existing panels, charts, heatmap and CSV export keep working.

   Board:  "ESP32S3 Dev Module"
   Flash:  16MB (128Mb)      PSRAM: OPI PSRAM        Partition: default 16M
   Libraries: Adafruit AS7341 · SparkFun VL53L5CX · INA226 (Rob Tillaart) ·
              Adafruit NeoPixel · ArduinoJson (v7)
   ===================================================================== */
#include <Wire.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_AS7341.h>
#include <SparkFun_VL53L5CX_Library.h>
#include <INA226.h>

#include "config.h"
#include "a7672s.h"
#include "indicators.h"

Adafruit_AS7341   as7341;
SparkFun_VL53L5CX tof;
VL53L5CX_ResultsData tofData;
INA226            ina(INA226_ADDR);
Preferences       nvs;

/* ---------------------------------------------------------------- state */
static bool     haveTof = false, haveSpec = false, haveIna = false;
static uint32_t reportMs = REPORT_MS_DEFAULT;
static uint32_t tLastReport = 0, tLastSensor = 0, tLastGnss = 0, tNextRetry = 0;
static uint32_t retryMs = MQTT_RETRY_MS_MIN;
static uint32_t gridSeq = 0;

static GnssFix  fix;
static CellInfo cell;

struct Batt { float v = 0, ma = 0; int soc = 0; bool charging = false; } batt;

struct Fence { bool enabled = false; double lat = 0, lon = 0; float r = 0; } fence;
static bool fenceOutside = false, fenceKnown = false;

/* ---------------------------------------------------------------- PSRAM replay */
static char*   replay[REPLAY_SLOTS] = {nullptr};
static uint8_t rHead = 0, rCount = 0;

static void replayPush(const char* json) {
  if (!replay[rHead]) return;
  strncpy(replay[rHead], json, REPLAY_SLOT_BYTES - 1);
  replay[rHead][REPLAY_SLOT_BYTES - 1] = 0;
  rHead = (rHead + 1) % REPLAY_SLOTS;
  if (rCount < REPLAY_SLOTS) rCount++;          // else the oldest is overwritten
}

static void replayFlush() {
  while (rCount && modem.connected()) {
    const uint8_t idx = (rHead + REPLAY_SLOTS - rCount) % REPLAY_SLOTS;
    if (!modem.publish(TOPIC_TELEMETRY, replay[idx], 1, false)) return;  // retry later
    rCount--;
  }
}

/* ---------------------------------------------------------------- maths */
static double haversine(double la1, double lo1, double la2, double lo2) {
  const double R = 6371000.0, d2r = PI / 180.0;
  const double dLat = (la2 - la1) * d2r, dLon = (lo2 - lo1) * d2r;
  const double a = sin(dLat / 2) * sin(dLat / 2) +
                   cos(la1 * d2r) * cos(la2 * d2r) * sin(dLon / 2) * sin(dLon / 2);
  return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

/* Per-cell Li-ion open-circuit curve. The pack voltage is divided by
   BATT_CELLS and IR-compensated first, because a 1 A load on a 2S pack
   sags ~120 mV and would otherwise read as 15 % of lost charge.        */
static int socFromCell(float vcell) {
  static const float V[] = {4.20f,4.15f,4.11f,4.08f,4.02f,3.98f,3.95f,3.91f,
                            3.87f,3.85f,3.84f,3.82f,3.80f,3.79f,3.77f,3.75f,
                            3.73f,3.71f,3.69f,3.61f,3.40f,3.00f};
  static const int   P[] = { 100,  95,  90,  85,  75,  70,  65,  60,
                              55,  50,  45,  40,  35,  30,  25,  20,
                              15,  10,   7,   5,   2,   0};
  const int n = sizeof(V) / sizeof(V[0]);
  if (vcell >= V[0]) return 100;
  if (vcell <= V[n - 1]) return 0;
  for (int i = 1; i < n; i++)
    if (vcell >= V[i]) {                        // linear between table rows
      const float f = (vcell - V[i]) / (V[i - 1] - V[i]);
      return (int)(P[i] + f * (P[i - 1] - P[i]) + 0.5f);
    }
  return 0;
}

/* ---------------------------------------------------------------- boot self-check
   Runs on the real board at boot: the two bits of maths that silently
   produce plausible-but-wrong numbers if someone edits them.           */
static void selfCheck() {
  bool ok = true;
  /* SoC must be monotonic and hit the rails */
  if (socFromCell(4.30f) != 100 || socFromCell(2.50f) != 0) ok = false;
  for (float v = 3.0f; v < 4.2f; v += 0.02f)
    if (socFromCell(v) > socFromCell(v + 0.02f)) ok = false;
  if (socFromCell(3.85f) < 45 || socFromCell(3.85f) > 55) ok = false;
  /* TIET Patiala -> Chandigarh is ~57 km; allow 2 % for the earth model */
  const double d = haversine(30.3540, 76.3637, 30.7333, 76.7794) / 1000.0;
  if (d < 56.0 || d > 59.0) ok = false;
  Serial.printf("self-check: %s   (soc@3.85V=%d%%, patiala->chd=%.1f km)\n",
                ok ? "PASS" : "FAIL", socFromCell(3.85f), d);
}

/* ---------------------------------------------------------------- sensors */
static void initSensors() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);

#if PIN_TOF_LPN >= 0
  pinMode(PIN_TOF_LPN, OUTPUT);
  digitalWrite(PIN_TOF_LPN, HIGH);              // enable VL53L5CX comms
  delay(10);
#endif

  haveSpec = as7341.begin();
  if (haveSpec) {
    as7341.setATIME(100);
    as7341.setASTEP(999);
    as7341.setGain(AS7341_GAIN_64X);
  } else Serial.println("AS7341 not found");

  haveTof = tof.begin();
  if (haveTof) {
    tof.setResolution(8 * 8);
    tof.setRangingFrequency(15);
    tof.startRanging();
  } else Serial.println("VL53L5CX not found (it needs ~1.2 s after power-up)");

  haveIna = ina.begin();
  if (haveIna) {
    /* R010 shunt. If this line is wrong, current and SoC are wrong. */
    ina.setMaxCurrentShunt(SHUNT_MAX_AMPS, SHUNT_OHMS);
    ina.setAverage(4);                          // 128 samples: quiet enough
  } else Serial.println("INA226 not found");

  if (!haveSpec || !haveTof || !haveIna) indRaise(IND_FATAL);
}

static void readBattery() {
  if (!haveIna) return;
  batt.v  = ina.getBusVoltage();
  batt.ma = ina.getCurrent_mA() * CURRENT_SIGN;
  batt.charging = batt.ma > CHARGING_MA_MIN;

  /* IR-compensate to open-circuit before reading the curve.
     batt.ma is NEGATIVE while discharging (the dashboard's convention), so
     v - I*R adds the sag back on and subtracts it again while charging.
     BATT_IR_OHMS is the WHOLE pack, so there is no BATT_CELLS factor. */
  const float amps = batt.ma / 1000.0f;
  const float vOc  = batt.v - amps * BATT_IR_OHMS;
  batt.soc = socFromCell(vOc / BATT_CELLS);

  if (batt.soc <= SOC_CRIT_PCT)      { indRaise(IND_BATT_CRIT); indClear(IND_BATT_LOW); }
  else if (batt.soc <= SOC_LOW_PCT)  { indRaise(IND_BATT_LOW);  indClear(IND_BATT_CRIT); }
  else if (batt.soc > SOC_LOW_PCT + 5) { indClear(IND_BATT_LOW | IND_BATT_CRIT); }

  if (batt.charging) indRaise(IND_CHARGING); else indClear(IND_CHARGING);
}

/* ---------------------------------------------------------------- geofence */
static void fenceLoad() {
  nvs.begin("tracker", true);
  fence.enabled = nvs.getBool("fe", false);
  fence.lat     = nvs.getDouble("flat", 0);
  fence.lon     = nvs.getDouble("flon", 0);
  fence.r       = nvs.getFloat("fr", 0);
  reportMs      = nvs.getULong("rep", REPORT_MS_DEFAULT);
  nvs.end();
  Serial.printf("fence: %s %.5f,%.5f r=%.0fm   report=%lu ms\n",
                fence.enabled ? "on" : "off", fence.lat, fence.lon, fence.r, reportMs);
}

static void fenceSave() {
  nvs.begin("tracker", false);
  nvs.putBool("fe", fence.enabled);
  nvs.putDouble("flat", fence.lat);
  nvs.putDouble("flon", fence.lon);
  nvs.putFloat("fr", fence.r);
  nvs.putULong("rep", reportMs);
  nvs.end();
}

static void publishEvent(const char* type, const char* detail) {
  JsonDocument d;
  d["device_id"] = DEVICE_ID;
  d["type"]      = type;
  d["detail"]    = detail;
  d["lat"]       = fix.lat;
  d["lon"]       = fix.lon;
  d["soc_pct"]   = batt.soc;
  char out[256];
  serializeJson(d, out, sizeof out);
  modem.publish(TOPIC_EVENT, out, 1, false);
}

static void fenceCheck() {
  if (!fence.enabled || !fix.valid) return;
  if (fix.sats < FENCE_MIN_SATS || fix.hdop > FENCE_MAX_HDOP) return;  // don't trust it

  const double d = haversine(fix.lat, fix.lon, fence.lat, fence.lon);
  /* hysteresis: leave at r+H, return at r-H, so sitting on the line
     does not machine-gun the buzzer and the alerts */
  const bool outside = fenceOutside ? (d > fence.r - FENCE_HYST_M)
                                    : (d > fence.r + FENCE_HYST_M);
  if (!fenceKnown) { fenceKnown = true; fenceOutside = outside; }
  else if (outside != fenceOutside) {
    fenceOutside = outside;
    char msg[64];
    snprintf(msg, sizeof msg, "%.0f m from centre (r=%.0f m)", d, fence.r);
    publishEvent(outside ? "geofence_exit" : "geofence_enter", msg);
    Serial.printf("GEOFENCE %s — %s\n", outside ? "EXIT" : "ENTER", msg);
  }
  if (outside) indRaise(IND_FENCE_OUT); else indClear(IND_FENCE_OUT);
}

/* ---------------------------------------------------------------- commands */
static void onMqttMessage(const char* topic, const char* payload) {
  JsonDocument d;
  if (deserializeJson(d, payload)) { Serial.println("bad JSON on " + String(topic)); return; }

  if (strstr(topic, "/cfg/geofence")) {
    fence.enabled = d["enabled"] | false;
    fence.lat     = d["lat"] | 0.0;
    fence.lon     = d["lon"] | 0.0;
    fence.r       = d["radius_m"] | 0.0f;
    if (fence.r <= 0) fence.enabled = false;
    fenceKnown = false;                          // re-evaluate against the new zone
    if (!fence.enabled) { fenceOutside = false; indClear(IND_FENCE_OUT); }
    fenceSave();
    Serial.printf("fence updated: %s %.5f,%.5f r=%.0f\n",
                  fence.enabled ? "on" : "off", fence.lat, fence.lon, fence.r);
    return;
  }

  const char* cmd = d["cmd"] | "";
  Serial.printf("cmd: %s\n", cmd);

  if (!strcmp(cmd, "buzzer") || !strcmp(cmd, "find")) {
    indFind(d["value"].is<int>() ? (uint32_t)d["value"] : FIND_DEFAULT_S);
  } else if (!strcmp(cmd, "set_interval")) {
    const uint32_t s = d["value"] | 0;
    if (s) {
      reportMs = constrain(s * 1000UL, REPORT_MS_MIN, REPORT_MS_MAX);
      fenceSave();
      Serial.printf("report interval -> %lu ms\n", reportMs);
    }
  } else if (!strcmp(cmd, "gnss_restart")) {
    modem.gnssRestart();
  } else if (!strcmp(cmd, "mute")) {
    indMute(d["value"] | true);
  } else if (!strcmp(cmd, "led_test")) {
    indFind(3);
  } else if (!strcmp(cmd, "reboot")) {
    publishEvent("reboot", "requested from dashboard");
    delay(200);
    ESP.restart();
  }
}

/* ---------------------------------------------------------------- packet */
static void buildAndPublish() {
  JsonDocument doc;
  doc["ts"]       = 0;                 // backend stamps it; device has no RTC
  doc["device_id"] = DEVICE_ID;
  doc["uptime_s"] = millis() / 1000;
  doc["fw"]       = FW_VERSION;

  JsonObject g = doc["gnss"].to<JsonObject>();
  g["fix"]       = fix.valid;
  g["lat"]       = fix.lat;
  g["lon"]       = fix.lon;
  g["alt_m"]     = fix.alt_m;
  g["speed_kmh"] = fix.speed_kmh;
  g["sats"]      = fix.sats;
  g["hdop"]      = fix.hdop;

  JsonObject c = doc["cell"].to<JsonObject>();
  c["mode"]       = cell.mode;
  c["operator"]   = cell.operator_;
  c["rssi_dbm"]   = cell.rssi_dbm;
  c["band"]       = cell.band;
  c["registered"] = cell.registered;

  JsonObject t = doc["tof"].to<JsonObject>();
  t["res"] = 8;
  uint16_t cmin = 65535, cmax = 0, centre = 0;
  if (haveTof) {
    const bool sendGrid = (gridSeq++ % GRID_EVERY_N) == 0;
    JsonArray grid = sendGrid ? t["grid"].to<JsonArray>()    : JsonArray();
    JsonArray zst  = sendGrid ? t["zstatus"].to<JsonArray>() : JsonArray();
    for (int i = 0; i < 64; i++) {
      const uint16_t mm = tofData.distance_mm[i];
      const uint8_t  st = tofData.target_status[i];
      if (sendGrid) { grid.add(mm); zst.add(st); }
      if (st == 5 || st == 6 || st == 9) {       // 5/6/9 = valid measurement
        if (mm < cmin) cmin = mm;
        if (mm > cmax) cmax = mm;
      }
    }
    /* "centre of view" = mean of the middle 2x2 zones */
    centre = (tofData.distance_mm[27] + tofData.distance_mm[28] +
              tofData.distance_mm[35] + tofData.distance_mm[36]) / 4;
  }
  t["distance_mm"] = centre;
  t["min_mm"]      = cmin == 65535 ? 0 : cmin;
  t["max_mm"]      = cmax;
  t["mode"]        = "8x8 @ 15 Hz";

  JsonObject s = doc["spectral"].to<JsonObject>();
  if (haveSpec) {
    uint16_t ch[12];
    if (as7341.readAllChannels(ch)) {
      s["f415"] = ch[0]; s["f445"] = ch[1]; s["f480"] = ch[2]; s["f515"] = ch[3];
      s["f555"] = ch[6]; s["f590"] = ch[7]; s["f630"] = ch[8]; s["f680"] = ch[9];
      s["clear"] = ch[10]; s["nir"] = ch[11];
      s["gain"]  = "64x";
      /* rough lux: CLEAR scaled for ATIME/ASTEP/gain above.
         ponytail: single scale factor, not a full AS7341 lux model —
         calibrate it against a reference meter, see docs/SETUP.md. */
      s["lux"]   = ch[10] * 0.164f;
    }
  } else {
    s["f415"]=0; s["f445"]=0; s["f480"]=0; s["f515"]=0; s["f555"]=0;
    s["f590"]=0; s["f630"]=0; s["f680"]=0; s["clear"]=0; s["nir"]=0;
    s["gain"]="n/a"; s["lux"]=0;
  }

  JsonObject b = doc["battery"].to<JsonObject>();
  b["voltage_v"] = roundf(batt.v * 100) / 100.0f;
  b["current_ma"] = (int)batt.ma;
  b["soc_pct"]   = batt.soc;
  b["charging"]  = batt.charging;

  JsonObject fz = doc["fence"].to<JsonObject>();   // extra: dashboard ignores it
  fz["enabled"] = fence.enabled;
  fz["outside"] = fenceOutside;

  static char out[REPLAY_SLOT_BYTES];
  const size_t n = serializeJson(doc, out, sizeof out);
  if (n >= sizeof(out) - 1) { Serial.println("packet truncated — raise REPLAY_SLOT_BYTES"); return; }

  if (modem.connected() && modem.publish(TOPIC_TELEMETRY, out, 1, false)) {
    Serial.printf("published %u bytes\n", (unsigned)n);
  } else {
    replayPush(out);
    Serial.printf("offline — buffered (%u held)\n", rCount);
  }
}

/* ---------------------------------------------------------------- setup */
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println("\n=== MultiSense Tracker " FW_VERSION " / " DEVICE_ID " ===");

  indBegin();
  indRaise(IND_BOOT);
  selfCheck();

  for (int i = 0; i < REPLAY_SLOTS; i++)
    replay[i] = (char*)heap_caps_malloc(REPLAY_SLOT_BYTES, MALLOC_CAP_SPIRAM);
  if (!replay[0]) Serial.println("PSRAM not available — offline buffering disabled");

  initSensors();
  fenceLoad();

  modem.onMessage(onMqttMessage);
  indClear(IND_BOOT);
  indRaise(IND_OFFLINE);

  if (!modem.begin())     { Serial.println("modem bring-up failed"); indRaise(IND_FATAL); }
  else if (!modem.netAttach()) Serial.println("LTE attach failed — will retry");
  else {
    modem.gnssOn();
    if (modem.sslSetup() && modem.mqttConnect()) {
      indClear(IND_OFFLINE);
      replayFlush();
    }
  }
  indChirpOk();
  tLastReport = millis() - reportMs;              // publish one packet immediately
}

/* ---------------------------------------------------------------- loop */
void loop() {
  indTick();                                     // LED + buzzer, every pass
  modem.poll();                                  // inbound cmd / cfg / URCs

  const uint32_t now = millis();

  if (now - tLastSensor >= SENSOR_MS) {
    tLastSensor = now;
    if (haveTof && tof.isDataReady()) tof.getRangingData(&tofData);
    readBattery();
  }

  if (now - tLastGnss >= GNSS_POLL_MS) {
    tLastGnss = now;
    modem.gnssRead(fix);
    if (fix.valid) indClear(IND_NO_FIX); else indRaise(IND_NO_FIX);
    fenceCheck();
  }

  if (!modem.connected()) {
    indRaise(IND_OFFLINE);
    if (now >= tNextRetry) {
      Serial.printf("MQTT reconnect (backoff %lu ms)\n", retryMs);
      modem.mqttStop();
      if (!modem.mqttConnect()) {
        retryMs = min(retryMs * 2, (uint32_t)MQTT_RETRY_MS_MAX);
        if (!modem.netAttach()) Serial.println("still no LTE");
        tNextRetry = millis() + retryMs;
      } else {
        retryMs = MQTT_RETRY_MS_MIN;
        indClear(IND_OFFLINE);
        replayFlush();
      }
    }
  } else {
    indClear(IND_OFFLINE);
    if (rCount) replayFlush();
  }

  if (now - tLastReport >= reportMs) {
    tLastReport = now;
    modem.cellRead(cell);
    buildAndPublish();
  }
}
