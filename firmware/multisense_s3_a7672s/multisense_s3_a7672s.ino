/* =====================================================================
   MultiSense Tracker — ESP32-S3 N16R8 + A7672S 4G + MQTT      v3.0.0
   =====================================================================
   AS7341   spectral (10 ch)      I2C 0x39   INT -> GPIO 6
   VL53L5CX 8x8 multizone ToF     I2C 0x29   INT -> GPIO 4, RST -> GPIO 5
   INA226   pack V/I fuel gauge   I2C 0x40   ALE -> GPIO 15
   A7672S   LTE Cat-1 + GNSS      UART1 TX 43 / RX 44, self-powering
   WS2812B  status LED            GPIO 1
   buzzer   fence/find/crash      GPIO 2

   Board: ESP32S3 Dev Module · 16MB flash · OPI PSRAM
          *** Tools > USB CDC On Boot = ENABLED ***
          GPIO 43/44 are the ROM debug UART; Serial must go over native
          USB or the bootloader's chatter lands in the modem's RX.

   Features: adaptive reporting · emergency + last-gasp battery modes ·
   filtered coulomb-counting fuel gauge · trips + odometer · 4-zone
   geofence · accident detection · network time · watchdog · cell-tower
   fallback · SMS fallback and commands · modem temperature ·
   I2C auto-recovery.
   ===================================================================== */
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "config.h"
#include "a7672s.h"
#include "indicators.h"
#include "battery.h"
#include "sensors.h"
#include "tracker.h"

/* ---------------------------------------------------------------- state */
static GnssFix  fix;
static CellInfo cell;
static int      modemTemp = -99;

static bool     emergency = false, lastGasped = false;
static uint32_t reportMs  = REPORT_MS_DEFAULT;
static uint32_t gridSeq   = 0;
static uint32_t tLastReport = 0, tLastSensor = 0, tLastGnss = 0;
static uint32_t tNextRetry = 0, retryMs = MQTT_RETRY_MS_MIN;
static uint32_t tLastTime = 0, tOfflineSince = 0, tLastSms = 0;
static uint32_t tLastFix = 0, tLastLbs = 0;
static bool     lbsValid = false;
static double   lbsLat = 0, lbsLon = 0;
static float    lbsAcc = 0;
static const char* resetReason = "unknown";

/* ---------------------------------------------------------------- replay */
static char*   replay[REPLAY_SLOTS] = {nullptr};
static uint8_t rHead = 0, rCount = 0;

static void replayPush(const char* json) {
  if (!replay[rHead]) return;
  strncpy(replay[rHead], json, REPLAY_SLOT_BYTES - 1);
  replay[rHead][REPLAY_SLOT_BYTES - 1] = 0;
  rHead = (rHead + 1) % REPLAY_SLOTS;
  if (rCount < REPLAY_SLOTS) rCount++;        // else the oldest is overwritten
}

static void replayFlush() {
  while (rCount && modem.connected()) {
    const uint8_t idx = (rHead + REPLAY_SLOTS - rCount) % REPLAY_SLOTS;
    if (!modem.publish(TOPIC_TELEMETRY, replay[idx], 1, false)) return;
    rCount--;
    esp_task_wdt_reset();
  }
}

/* ---------------------------------------------------------------- helpers */
static uint64_t nowMsEpoch() {
  const uint32_t e = modem.epochNow();
  return e ? (uint64_t)e * 1000ULL : 0ULL;    // 0 => backend stamps it
}

static void publishEvent(const char* type, const char* detail,
                         double lat, double lon, float speed) {
  JsonDocument d;
  d["device_id"] = DEVICE_ID;
  d["type"]      = type;
  d["detail"]    = detail;
  d["ts"]        = nowMsEpoch();
  d["lat"]       = lat;
  d["lon"]       = lon;
  d["speed_kmh"] = speed;
  d["soc_pct"]   = battGet().soc;
  d["odo_km"]    = trkOdometerKm();
  char out[380];
  serializeJson(d, out, sizeof out);
  if (!modem.publish(TOPIC_EVENT, out, 1, false))
    Serial.println("event publish failed (offline) — not buffered");
}

static const char* evName(TrkEventType t) {
  switch (t) {
    case EV_FENCE_EXIT:       return "geofence_exit";
    case EV_FENCE_ENTER:      return "geofence_enter";
    case EV_TRIP_START:       return "trip_start";
    case EV_TRIP_END:         return "trip_end";
    case EV_ACCIDENT:         return "accident";
    case EV_ACCIDENT_CLEARED: return "accident_cleared";
    default:                  return "event";
  }
}

/* ---------------------------------------------------------------- commands */
static void onMqttMessage(const char* topic, const char* payload) {
  if (strstr(topic, "/cfg/geofence")) { trkApplyFenceJson(payload); return; }

  JsonDocument d;
  if (deserializeJson(d, payload)) { Serial.println("bad JSON on cmd"); return; }
  const char* cmd = d["cmd"] | "";
  Serial.printf("cmd: %s\n", cmd);

  if (!strcmp(cmd, "buzzer") || !strcmp(cmd, "find")) {
    indFind(d["value"].is<int>() ? (uint32_t)d["value"] : FIND_DEFAULT_S);
  } else if (!strcmp(cmd, "set_interval")) {
    const uint32_t s = d["value"] | 0;
    if (s) {
      reportMs = constrain(s * 1000UL, REPORT_MS_MIN, REPORT_MS_MAX);
      Serial.printf("manual report interval -> %lu ms (overrides adaptive "
                    "until reboot)\n", reportMs);
    }
  } else if (!strcmp(cmd, "gnss_restart")) {
    modem.gnssRestart();
  } else if (!strcmp(cmd, "mute")) {
    indMute(d["value"] | true);
  } else if (!strcmp(cmd, "led_test")) {
    indFind(3);
  } else if (!strcmp(cmd, "clear_accident")) {
    indClear(IND_ACCIDENT);
  } else if (!strcmp(cmd, "reboot")) {
    publishEvent("reboot", "requested from dashboard", fix.lat, fix.lon, 0);
    delay(300);
    ESP.restart();
  }
}

/* ---------------------------------------------------------------- SMS */
#if FEAT_SMS
static void smsReply(const char* to) {
  char msg[160];
  const BattState& b = battGet();
  snprintf(msg, sizeof msg,
           "%s %s\nhttps://maps.google.com/?q=%.5f,%.5f\n%d%% %.2fV %s odo %.1fkm",
           DEVICE_ID, fix.valid ? "GNSS" : (lbsValid ? "CELL" : "NOFIX"),
           fix.valid ? fix.lat : lbsLat, fix.valid ? fix.lon : lbsLon,
           b.soc, b.v, b.charging ? "CHG" : "", trkOdometerKm());
  modem.smsSend(to, msg);
}

static void smsService() {
  int idx;
  if (modem.smsPending(idx)) {
    char from[24] = "", body[160] = "";
    if (modem.smsRead(idx, from, sizeof from, body, sizeof body)) {
      Serial.printf("SMS from %s: %s\n", from, body);
      const bool allowed = SMS_ALLOW_ANY_SENDER || !strcmp(from, SMS_OWNER);
      if (!allowed) Serial.println("sender not authorised — ignored");
      else {
        for (char* p = body; *p; p++) *p = toupper(*p);
        if (strstr(body, "FIND") || strstr(body, "BEEP"))      indFind(FIND_DEFAULT_S);
        else if (strstr(body, "WHERE") || strstr(body, "LOC")) smsReply(from);
        else if (strstr(body, "STATUS"))                       smsReply(from);
        else if (strstr(body, "MUTE"))                          indMute(true);
        else if (strstr(body, "UNMUTE"))                        indMute(false);
        else if (strstr(body, "REBOOT"))                      { delay(200); ESP.restart(); }
      }
    }
    modem.smsDelete(idx);
  }

  /* outbound fallback: data has been down a long time, so text a position */
  if (!modem.connected() && tOfflineSince && strlen(SMS_OWNER) &&
      (millis() - tOfflineSince) >= SMS_ON_DATA_DOWN_MS &&
      (!tLastSms || (millis() - tLastSms) >= SMS_MIN_GAP_MS)) {
    Serial.println("data down too long — sending position by SMS");
    smsReply(SMS_OWNER);
    tLastSms = millis();
  }
}
#endif

/* ---------------------------------------------------------------- packet */
static void buildAndPublish() {
  const BattState& b = battGet();
  JsonDocument doc;

  doc["ts"]        = nowMsEpoch();      // 0 => the backend stamps arrival
  doc["device_id"] = DEVICE_ID;
  doc["uptime_s"]  = millis() / 1000;
  doc["fw"]        = FW_VERSION;
  doc["mode"]      = emergency ? "emergency" : "normal";

  /* ---- position: GNSS, or the cell-tower estimate as a fallback ---- */
  JsonObject g = doc["gnss"].to<JsonObject>();
  if (fix.valid) {
    g["fix"] = true;  g["src"] = "gnss";
    g["lat"] = fix.lat; g["lon"] = fix.lon;
    g["alt_m"] = fix.alt_m; g["speed_kmh"] = fix.speed_kmh;
    g["sats"] = fix.sats;  g["hdop"] = fix.hdop;
  } else if (lbsValid) {
    /* Flagged clearly: this is a town-sized estimate, not a GNSS fix, and
       the dashboard must not draw it as a precise trail point. */
    g["fix"] = true;  g["src"] = "lbs";
    g["lat"] = lbsLat; g["lon"] = lbsLon;
    g["acc_m"] = lbsAcc;
    g["alt_m"] = 0;   g["speed_kmh"] = 0;
    g["sats"] = 0;    g["hdop"] = 99;
  } else {
    g["fix"] = false; g["src"] = "none";
    g["lat"] = 0; g["lon"] = 0; g["alt_m"] = 0;
    g["speed_kmh"] = 0; g["sats"] = 0; g["hdop"] = 99;
  }

  JsonObject c = doc["cell"].to<JsonObject>();
  c["mode"] = cell.mode;        c["operator"] = cell.operator_;
  c["rssi_dbm"] = cell.rssi_dbm; c["band"] = cell.band;
  c["registered"] = cell.registered;

  /* ---- ToF + spectral: skipped entirely in emergency mode, which is
     what makes an emergency packet small and cheap ---- */
  JsonObject t = doc["tof"].to<JsonObject>();
  t["res"] = 8;
  if (!emergency && sensTofOk()) {
    const TofFrame& T = sensTof();
    const bool sendGrid = (gridSeq++ % GRID_EVERY_N) == 0;
    if (sendGrid) {
      JsonArray grid = t["grid"].to<JsonArray>();
      JsonArray zst  = t["zstatus"].to<JsonArray>();
      for (int i = 0; i < 64; i++) { grid.add(T.mm[i]); zst.add(T.st[i]); }
    }
    t["distance_mm"] = T.centre;
    t["min_mm"] = T.minMm;  t["max_mm"] = T.maxMm;
    t["valid_zones"] = T.validZones;
    t["mode"] = "8x8 @ 15 Hz";
  } else {
    t["distance_mm"] = 0; t["min_mm"] = 0; t["max_mm"] = 0;
    t["mode"] = emergency ? "off (emergency)" : "unavailable";
  }

  JsonObject s = doc["spectral"].to<JsonObject>();
  if (!emergency && sensSpecOk() && sensSpec().valid) {
    const SpecFrame& P = sensSpec();
    s["f415"] = P.ch[0]; s["f445"] = P.ch[1]; s["f480"] = P.ch[2];
    s["f515"] = P.ch[3]; s["f555"] = P.ch[6]; s["f590"] = P.ch[7];
    s["f630"] = P.ch[8]; s["f680"] = P.ch[9];
    s["clear"] = P.ch[10]; s["nir"] = P.ch[11];
    s["gain"] = "64x";   s["lux"] = P.lux;
  } else {
    s["f415"]=0; s["f445"]=0; s["f480"]=0; s["f515"]=0; s["f555"]=0;
    s["f590"]=0; s["f630"]=0; s["f680"]=0; s["clear"]=0; s["nir"]=0;
    s["gain"] = emergency ? "off" : "n/a"; s["lux"] = 0;
  }

  /* ---- battery: the filtered answer plus the evidence behind it ---- */
  JsonObject bat = doc["battery"].to<JsonObject>();
  bat["voltage_v"]  = roundf(b.v * 100) / 100.0f;
  bat["current_ma"] = (int)b.ma;
  bat["soc_pct"]    = b.soc;
  bat["charging"]   = b.charging;
  bat["mah"]        = (int)b.mah;
  bat["capacity_mah"] = (int)BATT_CAPACITY_MAH;
  bat["soc_ocv"]    = b.socOcv;        // voltage curve alone, for comparison
  bat["soc_src"]    = b.src;           // "ocv" | "fused"
  bat["resting"]    = b.resting;
  bat["v_raw"]      = roundf(b.vRaw * 100) / 100.0f;

  /* ---- tracker ---- */
  const Trip& tp = trkTrip();
  JsonObject tr = doc["trip"].to<JsonObject>();
  tr["active"]   = tp.active;
  tr["km"]       = roundf(tp.km * 100) / 100.0f;
  tr["max_kmh"]  = tp.maxKmh;
  tr["mins"]     = tp.active ? (millis() - tp.startMs) / 60000 : 0;
  doc["odo_km"]  = roundf(trkOdometerKm() * 100) / 100.0f;

  JsonObject fz = doc["fence"].to<JsonObject>();
  JsonArray zs = fz["zones"].to<JsonArray>();
  uint8_t armed = 0;
  const Zone* Z = trkZones();
  for (int i = 0; i < FENCE_MAX_ZONES; i++) {
    if (!Z[i].enabled) continue;
    armed++;
    JsonObject z = zs.add<JsonObject>();
    z["name"] = Z[i].name;
    z["outside"] = Z[i].outside;
    z["r"] = Z[i].r;
  }
  fz["armed"]   = armed;
  fz["outside"] = trkZonesOutside();
  doc["accident"] = trkAccidentActive();

  /* ---- environment + diagnostics ---- */
  JsonObject env = doc["env"].to<JsonObject>();
  env["modem_temp_c"] = modemTemp;      // die temp: reads 10-20 C above ambient

  JsonObject dg = doc["diag"].to<JsonObject>();
  dg["reset_reason"]    = resetReason;
  dg["i2c_recoveries"]  = sensI2cRecoveries();
  dg["buffered"]        = rCount;
  dg["time_synced"]     = modem.timeValid();
  dg["free_heap"]       = ESP.getFreeHeap();
  dg["report_ms"]       = reportMs;

  static char out[REPLAY_SLOT_BYTES];
  const size_t n = serializeJson(doc, out, sizeof out);
  if (n >= sizeof(out) - 1) {
    Serial.println("packet truncated — raise REPLAY_SLOT_BYTES");
    return;
  }

  if (modem.connected() && modem.publish(TOPIC_TELEMETRY, out, 1, false))
    Serial.printf("published %u bytes (interval %lu ms, mode %s)\n",
                  (unsigned)n, reportMs, emergency ? "emergency" : "normal");
  else {
    replayPush(out);
    Serial.printf("offline — buffered (%u held)\n", rCount);
  }
}

/* ---------------------------------------------------------------- setup */
static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "other_wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deep_sleep";
    case ESP_RST_EXT:      return "external";
    default:               return "unknown";
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(400);                            // let native USB CDC come up
  resetReason = resetReasonStr();
  Serial.println("\n=== MultiSense Tracker " FW_VERSION " / " DEVICE_ID " ===");
  Serial.printf("last reset: %s\n", resetReason);

  indBegin();
  indRaise(IND_BOOT);

  const bool okB = battSelfCheck();
  const bool okT = trkSelfCheck();
  if (!okB || !okT)
    Serial.println("*** a self-check FAILED — fix the maths before trusting "
                   "any reading from this build ***");

#if FEAT_WATCHDOG
  /* The Arduino core may already have initialised the TWDT; re-initialising
     then returns INVALID_STATE, which is harmless — we only need to be
     subscribed to it. */
  #if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wcfg = {
      .timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_init(&wcfg);
  #else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
  #endif
  esp_task_wdt_add(NULL);
  Serial.printf("watchdog armed at %d s\n", WDT_TIMEOUT_S);
#endif

  for (int i = 0; i < REPLAY_SLOTS; i++)
    replay[i] = (char*)heap_caps_malloc(REPLAY_SLOT_BYTES, MALLOC_CAP_SPIRAM);
  if (!replay[0])
    Serial.println("PSRAM not available — offline buffering disabled "
                   "(check Tools > PSRAM = OPI PSRAM)");

  if (!sensBegin()) indRaise(IND_FATAL);
  battBegin();
  trkBegin();

  modem.onMessage(onMqttMessage);
  indClear(IND_BOOT);
  indRaise(IND_OFFLINE);
  esp_task_wdt_reset();

  if (!modem.begin()) {
    Serial.println("modem bring-up failed");
    indRaise(IND_FATAL);
  } else if (!modem.netAttach()) {
    Serial.println("LTE attach failed — will retry");
  } else {
    modem.gnssOn();
#if FEAT_TIME_SYNC
    if (modem.timeSync()) tLastTime = millis();
#endif
#if FEAT_SMS
    modem.smsBegin();
#endif
    if (modem.sslSetup() && modem.mqttConnect()) {
      indClear(IND_OFFLINE);
      char d[96];
      snprintf(d, sizeof d, "booted, fw %s, reset=%s", FW_VERSION, resetReason);
      publishEvent("boot", d, 0, 0, 0);
      replayFlush();
    }
  }

  indChirpOk();
  tOfflineSince = modem.connected() ? 0 : millis();
  tLastFix = millis();
  reportMs = trkReportInterval(false);
  tLastReport = millis() - reportMs;      // publish one packet immediately
}

/* ---------------------------------------------------------------- loop */
void loop() {
  esp_task_wdt_reset();
  indTick();                              // LED + buzzer, every pass
  modem.poll();                           // inbound cmd / cfg / SMS / URCs

  const uint32_t now = millis();

  if (now - tLastSensor >= SENSOR_MS) {
    tLastSensor = now;
    sensTick();
    battTick();

    const BattState& b = battGet();
    if (b.soc <= SOC_CRIT_PCT)        { indRaise(IND_BATT_CRIT); indClear(IND_BATT_LOW); }
    else if (b.soc <= SOC_LOW_PCT)    { indRaise(IND_BATT_LOW);  indClear(IND_BATT_CRIT); }
    else if (b.soc > SOC_LOW_PCT + 5) { indClear(IND_BATT_LOW | IND_BATT_CRIT); }
    if (b.charging) indRaise(IND_CHARGING); else indClear(IND_CHARGING);

#if FEAT_EMERGENCY
    /* Hysteresis both ways, or the device would flap in and out of
       emergency mode every time the modem transmits. */
    if (!emergency && b.soc <= EMERGENCY_SOC_PCT) {
      emergency = true;
      indBrightness(LED_BRIGHTNESS / 3);
      publishEvent("emergency_mode", "battery low: position-only reports, "
                   "sensors off", fix.lat, fix.lon, fix.speed_kmh);
      Serial.println("ENTERING emergency mode");
    } else if (emergency && b.soc >= EMERGENCY_EXIT_PCT) {
      emergency = false;
      indBrightness(LED_BRIGHTNESS);
      publishEvent("emergency_clear", "battery recovered", fix.lat, fix.lon, 0);
      Serial.println("leaving emergency mode");
    }
#endif

#if FEAT_LAST_GASP
    if (!lastGasped && (b.soc <= LAST_GASP_SOC_PCT || b.v <= LAST_GASP_VOLTS)) {
      lastGasped = true;
      Serial.println("LAST GASP — final position publish");
      battSave();
      buildAndPublish();
      publishEvent("last_gasp", "pack nearly empty, this is the final "
                   "known position", fix.lat, fix.lon, fix.speed_kmh);
#if FEAT_SMS
      if (strlen(SMS_OWNER)) smsReply(SMS_OWNER);
#endif
    }
#endif
    if (sensTakeInaAlert())
      publishEvent("undervoltage_alert", "INA226 bus under-voltage alert",
                   fix.lat, fix.lon, 0);
    if (sensTakeLightAlert())
      publishEvent("light_change", "ambient light crossed the AS7341 threshold",
                   fix.lat, fix.lon, 0);
  }

  /* ---- position ---- */
  if (now - tLastGnss >= GNSS_POLL_MS) {
    tLastGnss = now;
    modem.gnssRead(fix);
    if (fix.valid) {
      indClear(IND_NO_FIX);
      tLastFix = now;
      lbsValid = false;                   // a real fix beats a cell estimate
      trkOnFix(fix);
    } else {
      indRaise(IND_NO_FIX);
#if FEAT_LBS_FALLBACK
      if ((now - tLastFix) >= LBS_AFTER_NO_FIX_MS &&
          (!tLastLbs || (now - tLastLbs) >= LBS_RETRY_MS) && modem.connected()) {
        tLastLbs = now;
        lbsValid = modem.lbsRead(lbsLat, lbsLon, lbsAcc);
      }
#endif
    }
  }

  /* ---- drain tracker events ---- */
  TrkEvent e;
  while (trkTakeEvent(e)) {
    publishEvent(evName(e.type), e.detail, e.lat, e.lon, e.speed_kmh);
    tLastReport = now - reportMs;         // send a full packet with the event
  }

#if FEAT_SMS
  smsService();
#endif

  /* ---- reconnect ---- */
  if (!modem.connected()) {
    indRaise(IND_OFFLINE);
    if (!tOfflineSince) tOfflineSince = now;
    if (now >= tNextRetry) {
      Serial.printf("MQTT reconnect (backoff %lu ms)\n", retryMs);
      modem.mqttStop();
      if (!modem.mqttConnect()) {
        retryMs = min(retryMs * 2, (uint32_t)MQTT_RETRY_MS_MAX);
        if (!modem.netAttach()) Serial.println("still no LTE");
        tNextRetry = millis() + retryMs;
      } else {
        retryMs = MQTT_RETRY_MS_MIN;
        tOfflineSince = 0;
        indClear(IND_OFFLINE);
        replayFlush();
      }
    }
  } else {
    indClear(IND_OFFLINE);
    tOfflineSince = 0;
    if (rCount) replayFlush();
  }

#if FEAT_TIME_SYNC
  if (modem.connected() && (!modem.timeValid() ||
      (tLastTime && now - tLastTime >= TIME_RESYNC_MS))) {
    if (modem.timeSync()) tLastTime = now;
  }
#endif

  /* ---- report ---- */
  reportMs = trkReportInterval(emergency);
  if (now - tLastReport >= reportMs) {
    tLastReport = now;
    modem.cellRead(cell);
#if FEAT_MODEM_TEMP
    if (!emergency) modemTemp = modem.chipTempC();
#endif
    buildAndPublish();
  }
}
