/* =====================================================================
   MultiSense Tracker — ESP32 FIRMWARE (device side)
   =====================================================================
   Reads   : AS7341 spectral (I2C 0x39), VL53L5CX multizone ToF (I2C 0x29,
             8x8 = 64 zones), INA219 power monitor (I2C 0x40)
   Radio   : SIM7070G on UART2 — GNSS fix via AT+CGNSINF, telemetry
             uploaded as HTTP POST /api/telemetry (JSON, matches the
             dashboard/backend schema exactly)

   Libraries (Library Manager):
     Adafruit AS7341, SparkFun VL53L5CX Arduino Library,
     Adafruit INA219, ArduinoJson

   Wiring:
     I2C: SDA=21, SCL=22 (default Wire pins)
     SIM7070G: ESP32 GPIO16 (RX2) <- SIM TX, GPIO17 (TX2) -> SIM RX,
               PWRKEY on GPIO4 (optional), common GND, module powered
               from a supply able to source 2 A bursts.
   ===================================================================== */

#include <Wire.h>
#include <Adafruit_AS7341.h>
#include <SparkFun_VL53L5CX_Library.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>

/* ------------------------- user configuration ------------------------ */
#define APN            "airtelgprs.com"          // your SIM's APN
#define SERVER_HOST    "192.168.1.50"            // backend IP or domain
#define SERVER_PORT    8000
#define API_PATH       "/api/telemetry"
#define API_KEY        ""                        // set if TRACKER_API_KEY is used
#define DEVICE_ID      "TRK-7070-01"
#define POST_PERIOD_MS 5000                      // upload interval
#define BATT_CAPACITY_MAH 2000.0f

#define SIM_BAUD  115200
#define SIM_RX    16
#define SIM_TX    17
#define SIM_PWRKEY 4
#define BUZZER_PIN 25                            // "find device" buzzer
#define API_CMD_PATH "/api/command/next"

/* ------------------------------ globals ------------------------------ */
Adafruit_AS7341 as7341;
SparkFun_VL53L5CX tof;
VL53L5CX_ResultsData tofData;                 // 64-zone result buffer
Adafruit_INA219 ina219;
HardwareSerial simSerial(2);

uint32_t bootMs;
uint32_t postPeriod = POST_PERIOD_MS;            // adjustable via set_interval command

/* ------------------------- SIM7070G helpers -------------------------- */
String sendAT(const String &cmd, uint32_t timeout = 2000, const char *expect = "OK") {
  simSerial.println(cmd);
  String resp;
  uint32_t t0 = millis();
  while (millis() - t0 < timeout) {
    while (simSerial.available()) resp += (char)simSerial.read();
    if (resp.indexOf(expect) >= 0 || resp.indexOf("ERROR") >= 0) break;
  }
  Serial.printf("[AT] %s -> %s\n", cmd.c_str(), resp.c_str());
  return resp;
}

void simPowerOn() {
  pinMode(SIM_PWRKEY, OUTPUT);
  digitalWrite(SIM_PWRKEY, LOW);  delay(1200);
  digitalWrite(SIM_PWRKEY, HIGH); delay(3000);
}

bool simInit() {
  for (int i = 0; i < 10; i++)
    if (sendAT("AT").indexOf("OK") >= 0) break;
  sendAT("ATE0");                              // echo off
  sendAT("AT+CMEE=2");                         // verbose errors
  sendAT("AT+CNMP=38");                        // LTE only (2=auto if coverage is poor)
  sendAT("AT+CMNB=1");                         // Cat-M1 (2=NB-IoT, 3=both)
  sendAT("AT+CGDCONT=1,\"IP\",\"" APN "\"");
  sendAT("AT+CGATT=1", 10000);
  sendAT("AT+CNCFG=0,1,\"" APN "\"");
  sendAT("AT+CNACT=0,1", 15000, "ACTIVE");     // bring up PDP / get IP
  sendAT("AT+CGNSPWR=1");                      // GNSS on
  return true;
}

/* Parse AT+CGNSINF:
   +CGNSINF: <run>,<fix>,<UTC>,<lat>,<lon>,<alt>,<speed_kmh>,<course>,
             ...,<HDOP>,...,<sats_view>,<sats_used>,... */
struct Gnss { bool fix; double lat, lon; float alt, speed, hdop; int sats; };

Gnss readGnss() {
  Gnss g = {false, 0, 0, 0, 0, 9.9f, 0};
  String r = sendAT("AT+CGNSINF", 3000);
  int p = r.indexOf("+CGNSINF:");
  if (p < 0) return g;
  r = r.substring(p + 9);
  float f[21] = {0};
  int field = 0, from = 0;
  for (int i = 0; i <= (int)r.length() && field < 21; i++) {
    if (i == (int)r.length() || r[i] == ',') {
      f[field++] = r.substring(from, i).toFloat();
      from = i + 1;
    }
  }
  g.fix   = f[1] == 1;
  g.lat   = f[3];  g.lon = f[4];  g.alt = f[5];
  g.speed = f[6];  g.hdop = f[10] > 0 ? f[10] : 9.9f;
  g.sats  = (int)f[15];                        // satellites in view
  return g;
}

int readRssiDbm() {                            // AT+CSQ -> dBm
  String r = sendAT("AT+CSQ");
  int p = r.indexOf("+CSQ:");
  if (p < 0) return -113;
  int csq = r.substring(p + 5).toInt();
  return (csq >= 0 && csq <= 31) ? -113 + 2 * csq : -113;
}

/* HTTP POST via the SIM7070G's built-in HTTP(S) stack */
bool postJson(const String &json) {
  sendAT("AT+SHDISC", 1000);                   // close any stale session
  sendAT("AT+SHCONF=\"URL\",\"http://" SERVER_HOST ":" + String(SERVER_PORT) + "\"");
  sendAT("AT+SHCONF=\"BODYLEN\",2048");     // multizone grid enlarges the payload
  sendAT("AT+SHCONF=\"HEADERLEN\",350");
  if (sendAT("AT+SHCONN", 15000).indexOf("OK") < 0) return false;

  sendAT("AT+SHCHEAD");
  sendAT("AT+SHAHEAD=\"Content-Type\",\"application/json\"");
  if (strlen(API_KEY))
    sendAT("AT+SHAHEAD=\"X-API-Key\",\"" API_KEY "\"");

  sendAT("AT+SHBOD=" + String(json.length()) + ",10000", 2000, ">");
  simSerial.print(json);                       // raw body after '>' prompt
  delay(100);

  String resp = sendAT("AT+SHREQ=\"" API_PATH "\",3", 15000, "+SHREQ:");
  bool ok = resp.indexOf("+SHREQ:") >= 0 && resp.indexOf(",200,") >= 0;
  if (ok) pollCommand();                        // drain one queued command per cycle
  sendAT("AT+SHDISC", 1000);
  return ok;
}

/* Fetch and execute one queued dashboard command (same HTTP session). */
void pollCommand() {
  sendAT("AT+SHCHEAD");
  String r = sendAT("AT+SHREQ=\"" API_CMD_PATH "?device_id=" DEVICE_ID "\",1",
                    10000, "+SHREQ:");
  int p = r.indexOf(",200,");
  if (p < 0) return;
  int len = r.substring(p + 5).toInt();
  if (len < 12) return;                         // {"cmd":null}
  String body = sendAT("AT+SHREAD=0," + String(len), 5000, "OK");
  Serial.println("[CMD] " + body);
  if (body.indexOf("set_interval") >= 0) {
    int i = body.indexOf("\"value\":");
    int v = i < 0 ? 0 : body.substring(i + 8).toInt();
    if (v >= 2 && v <= 600) { postPeriod = (uint32_t)v * 1000UL; Serial.printf("[CMD] interval=%d s\n", v); }
  } else if (body.indexOf("buzzer") >= 0) {
    for (int i = 0; i < 8; i++) { digitalWrite(BUZZER_PIN, HIGH); delay(120); digitalWrite(BUZZER_PIN, LOW); delay(120); }
  } else if (body.indexOf("gnss_restart") >= 0) {
    sendAT("AT+CGNSCOLD");                      // cold restart the GNSS engine
  }
}

/* --------------------------- sensor helpers -------------------------- */
float socFromVoltage(float v) {                // crude 1S Li-ion OCV curve
  const float pts[][2] = {{3.30,0},{3.50,5},{3.60,10},{3.70,25},{3.80,50},
                          {3.90,70},{4.00,85},{4.10,95},{4.20,100}};
  if (v <= pts[0][0]) return 0;
  for (int i = 1; i < 9; i++)
    if (v < pts[i][0]) {
      float t = (v - pts[i-1][0]) / (pts[i][0] - pts[i-1][0]);
      return pts[i-1][1] + t * (pts[i][1] - pts[i-1][1]);
    }
  return 100;
}

/* ------------------------------- setup ------------------------------- */
void setup() {
  Serial.begin(115200);
  Wire.begin();
  bootMs = millis();

  if (!as7341.begin())  Serial.println("AS7341 not found!");
  as7341.setATIME(100);
  as7341.setASTEP(999);
  as7341.setGain(AS7341_GAIN_64X);

  if (!tof.begin()) Serial.println("VL53L5CX not found!");
  tof.setResolution(8*8);                     // 64 zones
  tof.setRangingFrequency(15);                // max for 8x8
  tof.startRanging();

  if (!ina219.begin()) Serial.println("INA219 not found!");
  pinMode(BUZZER_PIN, OUTPUT);

  simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX, SIM_TX);
  simPowerOn();
  simInit();
}

/* -------------------------------- loop ------------------------------- */
void loop() {
  static uint32_t lastPost = 0;
  if (millis() - lastPost < postPeriod) return;
  lastPost = millis();

  /* ---- AS7341 ---- */
  uint16_t ch[12];
  as7341.readAllChannels(ch);                  // F1..F8, CLEAR, NIR
  float lux = ch[10] * 0.30f;                  // rough scale — calibrate!

  /* ---- VL53L5CX: 8x8 depth frame ---- */
  static int16_t grid[64] = {0};
  static uint8_t zst[64]  = {0};
  if (tof.isDataReady() && tof.getRangingData(&tofData)) {
    for (int i = 0; i < 64; i++) {
      grid[i] = tofData.distance_mm[i];
      zst[i]  = tofData.target_status[i];     // 5/6/9 = usable
    }
  }
  int32_t dMin = 0, dMax = 0, cSum = 0; int cN = 0;
  const int cIdx[4] = {27, 28, 35, 36};       // centre 2x2 of the 8x8 grid
  for (int i = 0; i < 64; i++) {
    bool ok = grid[i] > 0 && (zst[i]==5 || zst[i]==6 || zst[i]==9);
    if (!ok) continue;
    if (!dMin || grid[i] < dMin) dMin = grid[i];
    if (grid[i] > dMax)          dMax = grid[i];
  }
  for (int j = 0; j < 4; j++) {
    int i = cIdx[j];
    if (grid[i] > 0 && (zst[i]==5 || zst[i]==6 || zst[i]==9)) { cSum += grid[i]; cN++; }
  }
  uint16_t dist = cN ? cSum / cN : 0;
  float sig = tofData.signal_per_spad[27] / 2048.0f;   // rough aggregate, kcps/SPAD → MCPS-ish

  /* ---- INA219 ---- */
  float busV = ina219.getBusVoltage_V();
  float curr = ina219.getCurrent_mA();         // + = charging into the pack
  float soc  = socFromVoltage(busV);

  /* ---- SIM7070G ---- */
  Gnss g = readGnss();
  int rssi = readRssiDbm();

  /* ---- build packet (matches dashboard schema) ---- */
  JsonDocument doc;
  doc["ts"] = 0;                               // backend stamps server time
  doc["device_id"] = DEVICE_ID;
  doc["uptime_s"] = (millis() - bootMs) / 1000;

  JsonObject gnss = doc["gnss"].to<JsonObject>();
  gnss["fix"] = g.fix; gnss["lat"] = g.lat; gnss["lon"] = g.lon;
  gnss["alt_m"] = g.alt; gnss["speed_kmh"] = g.speed;
  gnss["sats"] = g.sats; gnss["hdop"] = g.hdop;

  JsonObject cell = doc["cell"].to<JsonObject>();
  cell["mode"] = "LTE-M"; cell["operator"] = "";   // AT+COPS? to fill in
  cell["rssi_dbm"] = rssi; cell["band"] = ""; cell["registered"] = true;

  JsonObject tofO = doc["tof"].to<JsonObject>();
  tofO["res"] = 8;
  tofO["distance_mm"] = dist;
  tofO["min_mm"] = dMin; tofO["max_mm"] = dMax;
  tofO["signal_mcps"] = sig; tofO["mode"] = "8x8 @ 15 Hz";
  JsonArray ga = tofO["grid"].to<JsonArray>();
  JsonArray za = tofO["zstatus"].to<JsonArray>();
  for (int i = 0; i < 64; i++) { ga.add(grid[i]); za.add(zst[i]); }

  JsonObject sp = doc["spectral"].to<JsonObject>();
  const char *k[8] = {"f415","f445","f480","f515","f555","f590","f630","f680"};
  for (int i = 0; i < 8; i++) sp[k[i]] = ch[i < 4 ? i : i + 2]; // skip dup CLEAR/NIR slots
  sp["clear"] = ch[10]; sp["nir"] = ch[11];
  sp["gain"] = "64x"; sp["lux"] = lux;

  JsonObject bat = doc["battery"].to<JsonObject>();
  bat["voltage_v"] = busV; bat["current_ma"] = curr;
  bat["soc_pct"] = soc;    bat["charging"] = curr > 20;

  String body;
  serializeJson(doc, body);
  Serial.println(body);

  if (!postJson(body)) Serial.println("POST failed — will retry next cycle");
}
