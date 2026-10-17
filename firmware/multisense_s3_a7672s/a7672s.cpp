#include "a7672s.h"
#include "config.h"
#include "indicators.h"

A7672S modem;
static HardwareSerial& M = Serial1;

#if AT_DEBUG
  #define DBG(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
#endif

/* ================================================================= plumbing */
String A7672S::readLine(uint32_t timeoutMs) {
  String s;
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (M.available()) {
      const char c = (char)M.read();
      if (c == '\r') continue;
      if (c == '\n') { if (s.length()) return s; continue; }
      s += c;
      /* the ">" data prompt arrives with no newline behind it */
      if (s == ">") return s;
      if (s.length() > 1600) return s;
    }
    indTick();                           // AT waits must not stall the buzzer
    delay(1);
  }
  return s;                              // empty == timeout
}

size_t A7672S::readExact(char* dst, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  const uint32_t t0 = millis();
  while (got < n && millis() - t0 < timeoutMs) {
    if (M.available()) dst[got++] = (char)M.read();
    else { indTick(); delay(1); }
  }
  dst[got] = 0;
  return got;
}

void A7672S::drainTo(uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) { while (M.available()) M.read(); delay(1); }
}

/* Every response line passes through here first, so an unsolicited
   +CMQTTRX* arriving in the middle of a command's reply is never lost. */
bool A7672S::at(const char* cmd, const char* expect, uint32_t timeoutMs, String* out) {
  /* Drain anything already waiting THROUGH the URC handler rather than
     binning it — otherwise a command that arrived microseconds before this
     call is silently lost. */
  while (M.available()) {
    String pre = readLine(30);
    if (!pre.length()) break;
    DBG("<< %s\n", pre.c_str());
    handleUrc(pre);
  }
  DBG(">> %s\n", cmd);
  M.print(cmd);
  M.print("\r\n");

  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    String line = readLine(timeoutMs - (millis() - t0));
    if (!line.length()) break;
    DBG("<< %s\n", line.c_str());

    if (line.startsWith("+CMQTTRX") || line.startsWith("+CMQTTCONNLOST") ||
        line.startsWith("+CMQTTNONET") || line.startsWith("+CMTI")) {
      handleUrc(line); continue;
    }

    if (out) { *out += line; *out += '\n'; }
    if (line.indexOf(expect) >= 0) return true;
    if (line == "ERROR" || line.startsWith("+CME ERROR") ||
        line.startsWith("+CMS ERROR")) return false;
  }
  return false;
}

/* cmd -> ">" prompt -> raw bytes -> OK.  Used for topic/payload/will/cert. */
bool A7672S::sendWithPrompt(const char* cmd, const uint8_t* data, size_t len,
                            uint32_t timeoutMs) {
  while (M.available()) M.read();
  DBG(">> %s  (+%u bytes)\n", cmd, (unsigned)len);
  M.print(cmd);
  M.print("\r\n");

  bool prompt = false;
  const uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    String line = readLine(1000);
    if (line == ">") { prompt = true; break; }
    if (line.length()) {
      DBG("<< %s\n", line.c_str());
      if (line == "ERROR" || line.startsWith("+CME ERROR")) return false;
    }
  }
  if (!prompt) return false;

  M.write(data, len);
  M.flush();

  while (millis() - t0 < timeoutMs) {
    String line = readLine(timeoutMs - (millis() - t0));
    if (!line.length()) break;
    DBG("<< %s\n", line.c_str());
    if (line.startsWith("+CMQTTRX")) { handleUrc(line); continue; }
    if (line == "OK") return true;
    if (line == "ERROR" || line.startsWith("+CME ERROR")) return false;
  }
  return false;
}

/* ================================================================= bring-up */
bool A7672S::begin() {
  M.begin(MODEM_BAUD, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);

#if PIN_MODEM_RST >= 0
  pinMode(PIN_MODEM_RST, OUTPUT);
  digitalWrite(PIN_MODEM_RST, HIGH);
#endif

  /* If it is already awake, AT answers at once and we skip the pulse —
     pulsing PWRKEY on a running A7672S powers it OFF. */
  if (at("AT", "OK", 1500)) { DBG("modem already up\n"); }
  else {
#if PIN_MODEM_PWRKEY >= 0
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    delay(100);
    digitalWrite(PIN_MODEM_PWRKEY, LOW);   // A7672S: >1 s low to power on
    delay(1200);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
#endif
    bool up = false;
    for (int i = 0; i < 30 && !up; i++) up = at("AT", "OK", 1000);  // up to ~30 s
    if (!up) { DBG("modem did not answer AT (check power, baud and TX/RX order)\n"); return false; }
  }

  at("ATE0", "OK", 2000);                  // echo off — we parse the replies
  at("AT+CMEE=2", "OK", 2000);             // verbose errors, worth the bytes
  at("AT+CMQTTCFG=\"optimeout\",0,60", "OK", 2000);   // ok if unsupported
  return true;
}

bool A7672S::netAttach() {
  String out;
  if (!at("AT+CPIN?", "READY", 10000)) { DBG("SIM not ready\n"); return false; }

  char buf[160];
  snprintf(buf, sizeof buf, "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
  at(buf, "OK", 5000);
  if (strlen(APN_USER)) {
    snprintf(buf, sizeof buf, "AT+CGAUTH=1,1,\"%s\",\"%s\"", APN_USER, APN_PASS);
    at(buf, "OK", 5000);
  }
  at("AT+CGATT=1", "OK", 20000);

  /* wait for registration: CGREG stat 1 (home) or 5 (roaming) */
  bool reg = false;
  for (int i = 0; i < 60 && !reg; i++) {
    out = "";
    at("AT+CGREG?", "OK", 2000, &out);
    reg = out.indexOf(",1") > 0 || out.indexOf(",5") > 0;
    if (!reg) delay(1000);
  }
  if (!reg) { DBG("not registered on the network\n"); return false; }

  /* Some A76XX firmwares need the data socket opened explicitly before the
     MQTT stack will run (CMQTTCONNECT err 9 = "network not opened").
     On firmwares that auto-activate this returns "already opened" — fine. */
  out = "";
  at("AT+NETOPEN", "OK", 20000, &out);
  for (int i = 0; i < 10; i++) {
    out = "";
    at("AT+NETOPEN?", "OK", 2000, &out);
    if (out.indexOf("+NETOPEN: 1") >= 0) break;
    delay(1000);
  }
  return true;
}

bool A7672S::caPresent(const char* filename) {
  String out;
  at("AT+CCERTLIST", "OK", 5000, &out);
  return out.indexOf(filename) >= 0;
}

bool A7672S::sslSetup() {
  if (!caPresent(MQTT_CA_FILE)) {
    Serial.printf("\n*** CA certificate '%s' is NOT in the modem.\n", MQTT_CA_FILE);
    Serial.println("*** Run tools/upload_ca.py once (see docs/SETUP.md step 4).");
    return false;
  }
  char buf[120];
  at("AT+CSSLCFG=\"sslversion\",0,4", "OK", 3000);        // 4 = TLS 1.2
  at("AT+CSSLCFG=\"authmode\",0,1", "OK", 3000);          // 1 = verify server
  snprintf(buf, sizeof buf, "AT+CSSLCFG=\"cacert\",0,\"%s\"", MQTT_CA_FILE);
  at(buf, "OK", 3000);
  at("AT+CSSLCFG=\"enableSNI\",0,1", "OK", 3000);         // HiveMQ Cloud needs SNI
  return true;
}

/* ===================================================================== MQTT */
bool A7672S::mqttConnect() {
  mqttOk = false;

  if (!mqttStarted) {
    String out;
    at("AT+CMQTTSTART", "+CMQTTSTART: 0", 15000, &out);
    /* err 13 = "already started" on a warm retry; treat as started */
    mqttStarted = out.indexOf("+CMQTTSTART: 0") >= 0 || out.indexOf("13") >= 0;
    if (!mqttStarted) { DBG("CMQTTSTART failed\n"); return false; }
  }

  char buf[320];
  snprintf(buf, sizeof buf, "AT+CMQTTACCQ=0,\"%s\",1", DEVICE_ID);  // 1 = SSL
  if (!at(buf, "OK", 5000)) { at("AT+CMQTTREL=0", "OK", 3000); at(buf, "OK", 5000); }
  at("AT+CMQTTSSLCFG=0,0", "OK", 3000);

  /* last will: the broker publishes "offline" (retained) if we vanish */
  const char* willTopic = TOPIC_STATUS;
  const char* willMsg   = "{\"state\":\"offline\"}";
  snprintf(buf, sizeof buf, "AT+CMQTTWILLTOPIC=0,%u", (unsigned)strlen(willTopic));
  sendWithPrompt(buf, (const uint8_t*)willTopic, strlen(willTopic));
  snprintf(buf, sizeof buf, "AT+CMQTTWILLMSG=0,%u,1", (unsigned)strlen(willMsg));
  sendWithPrompt(buf, (const uint8_t*)willMsg, strlen(willMsg));

  snprintf(buf, sizeof buf,
           "AT+CMQTTCONNECT=0,\"tcp://%s:%d\",%d,1,\"%s\",\"%s\"",
           MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE, MQTT_USER, MQTT_PASS);
  /* tcp:// is correct even for TLS — the TLS switch is CMQTTACCQ's 3rd arg */
  String out;
  if (!at(buf, "+CMQTTCONNECT: 0,0", 60000, &out)) {
    Serial.printf("MQTT connect failed: %s\n", out.c_str());
    /* 30 bad user/pass · 31 not authorized · 32 TLS handshake ·
       33 cert not set · 34 cert/CA bad (classic cause: CA uploaded with
       CRLF line endings) · 25 DNS · 9 network not opened                */
    return false;
  }

  mqttOk = true;
  subscribe(TOPIC_CMD, 1);
  subscribe(TOPIC_CFG_FENCE, 1);
  publish(TOPIC_STATUS, "{\"state\":\"online\",\"fw\":\"" FW_VERSION "\"}", 1, true);
  Serial.println("MQTT connected");
  return true;
}

void A7672S::mqttStop() {
  at("AT+CMQTTDISC=0,60", "OK", 20000);
  at("AT+CMQTTREL=0", "OK", 5000);
  mqttOk = false;
}

bool A7672S::publish(const char* topic, const char* payload,
                     uint8_t qos, bool retain) {
  if (!mqttOk) return false;
  char buf[96];

  snprintf(buf, sizeof buf, "AT+CMQTTTOPIC=0,%u", (unsigned)strlen(topic));
  if (!sendWithPrompt(buf, (const uint8_t*)topic, strlen(topic))) return false;

  const size_t len = strlen(payload);
  snprintf(buf, sizeof buf, "AT+CMQTTPAYLOAD=0,%u", (unsigned)len);
  if (!sendWithPrompt(buf, (const uint8_t*)payload, len, 15000)) return false;

  snprintf(buf, sizeof buf, "AT+CMQTTPUB=0,%u,60,%d", qos, retain ? 1 : 0);
  if (!at(buf, "+CMQTTPUB: 0,0", 70000)) { mqttOk = false; return false; }
  return true;
}

bool A7672S::subscribe(const char* topic, uint8_t qos) {
  if (!mqttOk) return false;
  char buf[96];
  snprintf(buf, sizeof buf, "AT+CMQTTSUBTOPIC=0,%u,%u", (unsigned)strlen(topic), qos);
  if (!sendWithPrompt(buf, (const uint8_t*)topic, strlen(topic))) return false;
  return at("AT+CMQTTSUB=0", "+CMQTTSUB: 0,0", 20000);
}

/* ---- inbound: +CMQTTRXSTART / RXTOPIC / RXPAYLOAD / RXEND ---- */
void A7672S::handleUrc(const String& line) {
  if (line.startsWith("+CMTI")) {              // new SMS stored on the SIM
    smsIndex = line.substring(line.lastIndexOf(',') + 1).toInt();
    Serial.printf("inbound SMS at index %d\n", smsIndex);
    return;
  }
  if (line.startsWith("+CMQTTCONNLOST") || line.startsWith("+CMQTTNONET")) {
    Serial.printf("MQTT link lost: %s\n", line.c_str());
    mqttOk = false;
    return;
  }
  if (!line.startsWith("+CMQTTRXSTART")) return;

  static char topic[160];
  static char payload[REPLAY_SLOT_BYTES];
  topic[0] = payload[0] = 0;

  const uint32_t t0 = millis();
  while (millis() - t0 < 10000) {
    String l = readLine(3000);
    if (!l.length()) break;
    DBG("<< %s\n", l.c_str());

    if (l.startsWith("+CMQTTRXTOPIC")) {
      const int n = l.substring(l.lastIndexOf(',') + 1).toInt();
      readExact(topic, min((size_t)n, sizeof(topic) - 1), 3000);
      DBG("   topic=%s\n", topic);
    } else if (l.startsWith("+CMQTTRXPAYLOAD")) {
      const int n = l.substring(l.lastIndexOf(',') + 1).toInt();
      readExact(payload, min((size_t)n, sizeof(payload) - 1), 5000);
      DBG("   payload=%s\n", payload);
    } else if (l.startsWith("+CMQTTRXEND")) {
      if (msgCb && topic[0]) msgCb(topic, payload);
      return;
    }
  }
}

void A7672S::poll() {
  while (M.available()) {
    String line = readLine(50);
    if (!line.length()) break;
    DBG("<< %s\n", line.c_str());
    handleUrc(line);
  }
}

/* ===================================================================== GNSS */
bool A7672S::gnssOn() {
  if (!at("AT+CGNSSPWR=1", "OK", 10000)) return false;
  for (int i = 0; i < 20; i++) {                 // wait for "+CGNSSPWR: READY!"
    String l = readLine(1000);
    if (l.indexOf("READY") >= 0) break;
  }
  at("AT+CGNSSMODE=15", "OK", 3000);             // GPS+GLONASS+BDS+Galileo
  at("AT+CGNSSNMEA=0", "OK", 3000);              // we poll CGNSSINFO, no NMEA spam
  return true;
}

bool A7672S::gnssRestart() {
  at("AT+CGNSSPWR=0", "OK", 5000);
  delay(1500);
  at("AT+CGNSSCOLD", "OK", 5000);                // cold start, ok if unsupported
  return gnssOn();
}

/* ddmm.mmmmmm -> decimal degrees */
static double dmToDeg(const String& v, char hemi) {
  if (v.length() < 4) return 0;
  const int dot = v.indexOf('.');
  const int degDigits = (dot >= 0 ? dot : v.length()) - 2;
  if (degDigits <= 0) return 0;
  const double deg = v.substring(0, degDigits).toDouble();
  const double min = v.substring(degDigits).toDouble();
  double d = deg + min / 60.0;
  if (hemi == 'S' || hemi == 'W') d = -d;
  return d;
}

bool A7672S::gnssRead(GnssFix& f) {
  String out;
  if (!at("AT+CGNSSINFO", "OK", 3000, &out)) return false;
  const int i = out.indexOf("+CGNSSINFO:");
  if (i < 0) return false;

  String body = out.substring(i + 11);
  body.trim();
  if (body.startsWith(",,") || body.length() < 10) { f.valid = false; return true; }

  /* +CGNSSINFO: <mode>,<GPSsv>,<GLOsv>,<BDSsv>,<GALsv>,<lat>,<N/S>,<lon>,
                 <E/W>,<date>,<utc>,<alt>,<speed_kn>,<course>,<pdop>,<hdop>,<vdop> */
  String fld[20];
  int n = 0, start = 0;
  for (int k = 0; k <= (int)body.length() && n < 20; k++) {
    if (k == (int)body.length() || body[k] == ',') { fld[n++] = body.substring(start, k); start = k + 1; }
  }
  if (n < 16 || !fld[5].length() || !fld[7].length()) { f.valid = false; return true; }

  f.lat       = dmToDeg(fld[5], fld[6].length() ? fld[6][0] : 'N');
  f.lon       = dmToDeg(fld[7], fld[8].length() ? fld[8][0] : 'E');
  f.sats      = fld[1].toInt() + fld[2].toInt() + fld[3].toInt() + fld[4].toInt();
  f.alt_m     = fld[11].toFloat();
  f.speed_kmh = fld[12].toFloat() * 1.852f;      // CGNSSINFO speed is in knots
  f.course    = fld[13].toFloat();
  f.hdop      = fld[15].length() ? fld[15].toFloat() : 99;
  snprintf(f.utc, sizeof f.utc, "%s %s", fld[9].c_str(), fld[10].c_str());
  f.valid     = (f.lat != 0 || f.lon != 0);
  return true;
}

/* ===================================================================== cell */
bool A7672S::cellRead(CellInfo& c) {
  String out;
  if (at("AT+CSQ", "OK", 2000, &out)) {
    const int i = out.indexOf("+CSQ:");
    if (i >= 0) {
      const int rssi = out.substring(i + 5, out.indexOf(',', i)).toInt();
      c.rssi_dbm = (rssi >= 0 && rssi <= 31) ? -113 + 2 * rssi : -113;
    }
  }
  out = "";
  if (at("AT+COPS?", "OK", 3000, &out)) {
    const int q1 = out.indexOf('"');
    const int q2 = out.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) {
      strncpy(c.operator_, out.substring(q1 + 1, q2).c_str(), sizeof(c.operator_) - 1);
      c.operator_[sizeof(c.operator_) - 1] = 0;
    }
  }
  out = "";
  if (at("AT+CPSI?", "OK", 3000, &out)) {
    /* +CPSI: LTE CAT-M1,Online,404-45,...,EUTRAN-BAND3,... */
    if (out.indexOf("NO SERVICE") >= 0) { c.registered = false; }
    else {
      c.registered = out.indexOf("Online") >= 0;
      const int b = out.indexOf("BAND");
      if (b >= 0) {
        int e = b;
        while (e < (int)out.length() && out[e] != ',' && out[e] != '\n') e++;
        strncpy(c.band, out.substring(b, e).c_str(), sizeof(c.band) - 1);
        c.band[sizeof(c.band) - 1] = 0;
      }
      const int cm = out.indexOf("+CPSI: ");
      if (cm >= 0) {
        int e = out.indexOf(',', cm);
        if (e > cm) {
          strncpy(c.mode, out.substring(cm + 7, e).c_str(), sizeof(c.mode) - 1);
          c.mode[sizeof(c.mode) - 1] = 0;
        }
      }
    }
  }
  return true;
}

/* ===================================================================== time
   The tracker has no RTC. Without this every packet leaves with ts=0 and
   is stamped on arrival, which is fine live but wrong for packets that
   sat in the offline buffer for an hour. */
static uint32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = y - era * 400;
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

bool A7672S::timeSync() {
  at("AT+CTZU=1", "OK", 3000);                 // auto timezone from network
  String out;
  if (!at("AT+CCLK?", "OK", 3000, &out)) return false;

  const int q = out.indexOf('"');
  if (q < 0) return false;
  /* +CCLK: "26/10/18,00:30:15+22"   — the trailing number is the zone
     offset in QUARTER hours, so +22 means +05:30. */
  String t = out.substring(q + 1, out.indexOf('"', q + 1));
  if (t.length() < 17) return false;

  const int yy = t.substring(0, 2).toInt();
  const int mo = t.substring(3, 5).toInt();
  const int dd = t.substring(6, 8).toInt();
  const int hh = t.substring(9, 11).toInt();
  const int mi = t.substring(12, 14).toInt();
  const int ss = t.substring(15, 17).toInt();
  if (yy < 24 || mo < 1 || mo > 12 || dd < 1 || dd > 31) {
    DBG("CCLK not yet valid: %s\n", t.c_str());
    return false;                              // modem hasn't been told yet
  }

  int tzq = 0;
  const int sign = t.indexOf('+') > 16 ? 1 : (t.indexOf('-') > 16 ? -1 : 0);
  if (sign) tzq = sign * t.substring(t.length() - 2).toInt();

  const long days = daysFromCivil(2000 + yy, mo, dd);
  uint32_t e = (uint32_t)(days * 86400L) + hh * 3600UL + mi * 60UL + ss;
  e -= tzq * 15 * 60;                          // local -> UTC

  epochBase   = e;
  epochMillis = millis();
  Serial.printf("time synced: %s (tz %+d min) -> epoch %lu\n",
                t.c_str(), tzq * 15, (unsigned long)e);
  return true;
}

uint32_t A7672S::epochNow() {
  if (!epochBase) return 0;
  return epochBase + (millis() - epochMillis) / 1000;
}

/* ===================================================================== temp */
int A7672S::chipTempC() {
  String out;
  if (!at("AT+CPMUTEMP", "OK", 3000, &out)) return -99;
  const int i = out.indexOf("+CPMUTEMP:");
  if (i < 0) return -99;
  return out.substring(i + 10).toInt();
}

/* ===================================================================== LBS
   Cell-tower position. Accuracy is hundreds of metres to kilometres, so
   it answers "which part of town" and never "which building". */
bool A7672S::lbsRead(double& lat, double& lon, float& accM) {
  String out;
  at("AT+CLBSCFG=0,3,\"lbs-simcom.com:3002\"", "OK", 3000);   // ok if unsupported
  if (!at("AT+CLBS=1,1", "+CLBS:", 30000, &out)) return false;

  const int i = out.indexOf("+CLBS:");
  if (i < 0) return false;
  String body = out.substring(i + 6);
  body.trim();

  String f[6];
  int n = 0, start = 0;
  for (int k = 0; k <= (int)body.length() && n < 6; k++)
    if (k == (int)body.length() || body[k] == ',') {
      f[n++] = body.substring(start, k); start = k + 1;
    }
  if (n < 4 || f[0].toInt() != 0) return false;   // field 0 = result code

  /* SIMCom returns longitude FIRST. Some firmwares swap them, so sanity
     check rather than trust the order blindly. */
  double a = f[1].toDouble(), b = f[2].toDouble();
  if (fabs(a) <= 90.0 && fabs(b) > 90.0) { lat = a; lon = b; }
  else                                   { lon = a; lat = b; }
  accM = f[3].toFloat();
  if (lat == 0 && lon == 0) return false;
  Serial.printf("LBS fix: %.5f,%.5f +/- %.0f m\n", lat, lon, accM);
  return true;
}

/* ===================================================================== SMS */
bool A7672S::smsBegin() {
  at("AT+CMGF=1", "OK", 3000);             // text mode
  at("AT+CSCS=\"GSM\"", "OK", 3000);
  at("AT+CNMI=2,1,0,0,0", "OK", 3000);     // notify with +CMTI, don't dump
  return true;
}

bool A7672S::smsSend(const char* number, const char* text) {
  if (!number || !number[0]) return false;
  char buf[64];
  snprintf(buf, sizeof buf, "AT+CMGS=\"%s\"", number);

  while (M.available()) M.read();
  DBG(">> %s\n", buf);
  M.print(buf); M.print("\r\n");

  bool prompt = false;
  const uint32_t t0 = millis();
  while (millis() - t0 < 10000) {
    String l = readLine(1000);
    if (l == ">") { prompt = true; break; }
    if (l.length() && (l == "ERROR" || l.startsWith("+CM"))) {
      DBG("<< %s\n", l.c_str());
      if (l == "ERROR") return false;
    }
  }
  if (!prompt) return false;

  M.print(text);
  M.write((uint8_t)0x1A);                           // Ctrl-Z ends the message
  M.flush();

  while (millis() - t0 < 60000) {          // the network can be slow
    String l = readLine(5000);
    if (!l.length()) break;
    DBG("<< %s\n", l.c_str());
    if (l.startsWith("+CMGS")) return true;
    if (l == "ERROR" || l.startsWith("+CMS ERROR")) return false;
  }
  return false;
}

bool A7672S::smsPending(int& index) {
  if (smsIndex < 0) return false;
  index = smsIndex;
  smsIndex = -1;
  return true;
}

bool A7672S::smsRead(int index, char* from, size_t fromLen,
                     char* body, size_t bodyLen) {
  char cmd[32];
  snprintf(cmd, sizeof cmd, "AT+CMGR=%d", index);
  String out;
  if (!at(cmd, "OK", 10000, &out)) return false;

  const int h = out.indexOf("+CMGR:");
  if (h < 0) return false;
  /* +CMGR: "REC UNREAD","+919...",,"26/10/18,00:30:15+22"
     <message body on the next line> */
  const int q1 = out.indexOf('"', out.indexOf('"', h + 6) + 1);
  const int q2 = out.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) return false;
  strncpy(from, out.substring(q1 + 1, q2).c_str(), fromLen - 1);
  from[fromLen - 1] = 0;

  const int nl = out.indexOf('\n', h);
  if (nl < 0) return false;
  int end = out.indexOf('\n', nl + 1);
  if (end < 0) end = out.length();
  String b = out.substring(nl + 1, end);
  b.trim();
  strncpy(body, b.c_str(), bodyLen - 1);
  body[bodyLen - 1] = 0;
  return true;
}

void A7672S::smsDelete(int index) {
  char cmd[32];
  snprintf(cmd, sizeof cmd, "AT+CMGD=%d", index);
  at(cmd, "OK", 5000);
}
