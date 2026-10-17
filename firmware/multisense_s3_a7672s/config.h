/* =====================================================================
   config.h  —  EVERY tunable for the MultiSense Tracker lives here.
   Hardware: ESP32-S3-WROOM-1 N16R8 + SIMCom A7672S + AS7341 +
             VL53L5CX + INA226 (R010) + WS2812B + buzzer
   Edit this file only; the rest of the sketch reads from it.
   ===================================================================== */
#pragma once

/* ---------------------------------------------------------------- identity */
#define DEVICE_ID        "TRK-A7672S-01"   // also the MQTT client id
#define FW_VERSION       "2.0.0"

/* ---------------------------------------------------------------- cellular */
#define APN              "airtelgprs.com" // Airtel IN. Jio: "jionet"  BSNL: "bsnlnet"
#define APN_USER         ""
#define APN_PASS         ""

/* ---------------------------------------------------------------- MQTT broker
   HiveMQ Cloud Serverless (free). Host from your cluster's Overview page.
   NOTE: server_addr ALWAYS uses the tcp:// prefix, even for TLS — the
   TLS switch is the 3rd arg of AT+CMQTTACCQ, not the URL scheme.        */
#define MQTT_HOST        "xxxxxxxxxxxx.s1.eu.hivemq.cloud"
#define MQTT_PORT        8883
#define MQTT_USER        "tracker-device"
#define MQTT_PASS        "CHANGE_ME"
#define MQTT_KEEPALIVE   60              // s, 1..64800
#define MQTT_CA_FILE     "isrgrootx1.pem" // uploaded once via tools/upload_ca.py

/* Topics — keep in sync with backend/mqtt_bridge.py and frontend/mqtt-live.js */
#define TOPIC_BASE       "multisense/" DEVICE_ID
#define TOPIC_TELEMETRY  TOPIC_BASE "/telemetry"
#define TOPIC_STATUS     TOPIC_BASE "/status"      // retained, LWT = "offline"
#define TOPIC_EVENT      TOPIC_BASE "/event"
#define TOPIC_CMD        TOPIC_BASE "/cmd"         // subscribed
#define TOPIC_CFG_FENCE  TOPIC_BASE "/cfg/geofence" // subscribed, retained

/* ---------------------------------------------------------------- timing */
#define REPORT_MS_DEFAULT  30000UL   // telemetry period; set_interval can change it
#define REPORT_MS_MIN       5000UL
#define REPORT_MS_MAX     600000UL
#define SENSOR_MS            500UL   // local sensor refresh (LED/fence react fast)
#define GNSS_POLL_MS        1000UL
#define GRID_EVERY_N            1    // 1 = full 8x8 ToF grid in every packet.
                                     // Raise to 10 if your SIM data plan hurts.
#define MQTT_RETRY_MS_MIN   5000UL   // reconnect backoff floor
#define MQTT_RETRY_MS_MAX 120000UL   // ...and ceiling

/* ---------------------------------------------------------------- pins
   ESP32-S3 N16R8: GPIO 26-37 are taken by the octal flash/PSRAM and
   19/20 by native USB. Everything below avoids them.                    */
#define PIN_I2C_SDA        8
#define PIN_I2C_SCL        9
#define I2C_HZ        400000UL

#define PIN_MODEM_TX      17   // ESP32 TX -> A7672S RX
#define PIN_MODEM_RX      18   // ESP32 RX <- A7672S TX
#define PIN_MODEM_PWRKEY  16   // low pulse to power on; -1 if always-on module
#define PIN_MODEM_RST     -1   // optional
#define MODEM_BAUD    115200

#define PIN_LED_DATA      21   // WS2812B DIN (onboard RGB on a DevKitC is 48)
#define LED_COUNT          1
#define LED_BRIGHTNESS    60   // 0-255; keep low, it's a battery device

#define PIN_BUZZER         4
#define BUZZER_PASSIVE     1   // 1 = passive buzzer driven by LEDC tone
                               // 0 = active buzzer, just switched on/off
#define PIN_TOF_LPN        5   // VL53L5CX LPn (comms enable); -1 if tied high

/* ---------------------------------------------------------------- battery
   2S Li-ion: 8.4 V full -> 6.0 V empty. INA226 with an R010 (0.01 ohm) shunt,
   high-side on the pack lead.                                           */
#define BATT_CELLS            2
#define INA226_ADDR        0x40
#define SHUNT_OHMS        0.01f    // R010. Wrong value here = wrong everything.
#define SHUNT_MAX_AMPS     8.0f    // sets the current LSB (8/32768 = 244 uA)
#define BATT_IR_OHMS      0.120f   // pack internal resistance, for OCV comp.
                                   // CALIBRATE: see docs/SETUP.md "fuel gauge".
#define CURRENT_SIGN        (+1)   // flip to -1 if charge/discharge read inverted
#define CHARGING_MA_MIN    30.0f   // above this (after sign fix) = charging
#define SOC_LOW_PCT           20   // red blink @ 1 s
#define SOC_CRIT_PCT          10   // red blink @ 250 ms
#define BATT_CHIRP_MS    60000UL   // low-battery chirp period; 0 = silent

/* ---------------------------------------------------------------- geofence
   Pushed from the dashboard as a retained JSON message and cached in NVS,
   so the buzzer still fires with the dashboard closed and the server down. */
#define FENCE_HYST_M        15.0f  // metres of hysteresis, stops edge chatter
#define FENCE_MIN_SATS          5  // ignore fixes weaker than this for fencing
#define FENCE_MAX_HDOP       3.0f

/* ---------------------------------------------------------------- offline buffer */
#define REPLAY_SLOTS           60  // packets held in PSRAM while 4G is down
#define REPLAY_SLOT_BYTES    2048

/* ---------------------------------------------------------------- misc */
#define FIND_DEFAULT_S         30  // find-device duration if none supplied
#define SERIAL_BAUD       115200
#define AT_DEBUG               1   // 1 = echo all modem traffic to USB serial
