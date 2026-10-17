/* =====================================================================
   a7672s.h — SIMCom A7672S: power-up, LTE attach, TLS, MQTT, GNSS.

   Why a hand-rolled AT driver and not TinyGSM: TinyGSM has no support
   for the A76XX native MQTT stack (AT+CMQTT*), and running PubSubClient
   over a TinyGSM TCP socket means doing TLS in the ESP32 instead of the
   modem — which is slower, uses more RAM, and loses the modem's
   persistent session. The AT stack is the right tool here.

   Everything is non-blocking EXCEPT begin()/netAttach()/mqttConnect(),
   which are one-shot bring-up calls. poll() must be called every loop().
   ===================================================================== */
#pragma once
#include <Arduino.h>

struct GnssFix {
  bool   valid = false;
  double lat = 0, lon = 0;
  float  alt_m = 0, speed_kmh = 0, course = 0;
  int    sats = 0;
  float  hdop = 99;
  char   utc[24] = "";
};

struct CellInfo {
  char  mode[12]     = "unknown";
  char  operator_[24] = "";
  char  band[12]     = "";
  int   rssi_dbm     = -113;
  bool  registered   = false;
};

typedef void (*MqttMsgCb)(const char* topic, const char* payload);

class A7672S {
public:
  bool begin();                  // UART, PWRKEY pulse, AT sync, echo off
  bool netAttach();              // SIM -> APN -> registration -> PDP active
  bool sslSetup();               // TLS1.2 + CA verify; false if CA missing
  bool mqttConnect();            // acquire client, LWT, connect, subscribe
  void mqttStop();
  bool connected() const { return mqttOk; }

  bool publish(const char* topic, const char* payload,
               uint8_t qos = 1, bool retain = false);
  bool subscribe(const char* topic, uint8_t qos = 1);
  void onMessage(MqttMsgCb cb) { msgCb = cb; }
  void poll();                   // drain URCs, dispatch inbound messages

  bool gnssOn();
  bool gnssRestart();
  bool gnssRead(GnssFix& f);
  bool cellRead(CellInfo& c);

  /* exposed for the bring-up sketch / serial AT passthrough */
  bool at(const char* cmd, const char* expect = "OK",
          uint32_t timeoutMs = 3000, String* out = nullptr);
  bool caPresent(const char* filename);

private:
  bool   mqttOk     = false;
  bool   mqttStarted = false;
  MqttMsgCb msgCb   = nullptr;

  bool   sendWithPrompt(const char* cmd, const uint8_t* data, size_t len,
                        uint32_t timeoutMs = 10000);
  String readLine(uint32_t timeoutMs);
  void   handleUrc(const String& line);
  size_t readExact(char* dst, size_t n, uint32_t timeoutMs);
  void   drainTo(uint32_t ms);
};

extern A7672S modem;
