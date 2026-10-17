/* =====================================================================
   config.h  —  EVERY tunable for the MultiSense Tracker lives here.
   Hardware: ESP32-S3-WROOM-1 N16R8 + SIMCom A7672S + AS7341 +
             VL53L5CX + INA226 (R010) + WS2812B + buzzer
   Edit this file only; the rest of the sketch reads from it.
   v3.0 — pin map matches Pranjay's physical wiring, Oct 2026
   ===================================================================== */
#pragma once

/* ---------------------------------------------------------------- identity */
#define DEVICE_ID        "TRK-A7672S-01"   // also the MQTT client id
#define FW_VERSION       "3.0.0"

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
#define TOPIC_STATUS     TOPIC_BASE "/status"       // retained, LWT = "offline"
#define TOPIC_EVENT      TOPIC_BASE "/event"
#define TOPIC_CMD        TOPIC_BASE "/cmd"          // subscribed
#define TOPIC_CFG_FENCE  TOPIC_BASE "/cfg/geofence" // subscribed, retained

/* =====================================================================
   PIN MAP  —  as physically wired. Verified safe on ESP32-S3:
   GPIO 26-37 are the octal flash/PSRAM (35/36/37 will break boot),
   19/20 are native USB, 0/3/45/46 are strapping pins. None used below.

   GPIO 43/44 are U0TXD/U0RXD — the ROM's default debug UART. They are
   free for the modem ONLY because Serial goes over native USB:
   set  Tools > USB CDC On Boot = Enabled.  See docs/SETUP.md §2.
   ===================================================================== */
#define PIN_I2C_SDA        8    // AS7341 + VL53L5CX + INA226
#define PIN_I2C_SCL        9
#define I2C_HZ        400000UL

#define PIN_MODEM_TX      43   // ESP32 TX  -> A7672S RX  (U0TXD)
#define PIN_MODEM_RX      44   // ESP32 RX  <- A7672S TX  (U0RXD)
#define PIN_MODEM_PWRKEY  -1   // not wired: the module self-starts on power
#define PIN_MODEM_RST     -1
#define MODEM_BAUD    115200

#define PIN_LED_DATA       1   // WS2812B OUT
#define LED_COUNT          1
#define LED_BRIGHTNESS    60   // 0-255; dimmed further in emergency mode

#define PIN_BUZZER         2   // buzzer +ve
#define BUZZER_PASSIVE     1   // 1 = passive buzzer via LEDC tone
                               // 0 = active buzzer, switched on/off only

#define PIN_TOF_INT        4   // VL53L5CX INT  (data-ready, active low)
#define PIN_TOF_RST        5   // VL53L5CX RST  (hardware recovery)
#define TOF_RST_ACTIVE_HIGH 1  // 1 = I2C_RST semantics (pulse HIGH, idle LOW).
                               // Set 0 if your breakout brings out an
                               // active-LOW enable instead, or the sensor
                               // will sit held in reset and never answer.
#define PIN_SPEC_INT       6   // AS7341 INT    (light threshold)
#define PIN_INA_ALERT     15   // INA226 ALE    (under-voltage alert)

/* GPIO 7 is deliberately UNUSED. The INA226's VBS pin is the chip's own
   bus-voltage sense input and must go to the 2S pack node — NOT to a
   GPIO. Pack voltage arrives over I2C. If a wire still runs VBS->GPIO 7,
   remove it: that pin is 3.3 V max and 8.4 V will damage it. */

/* ---------------------------------------------------------------- timing */
#define REPORT_MS_DEFAULT  30000UL   // fallback period if adaptive is off
#define REPORT_MS_MIN       5000UL
#define REPORT_MS_MAX     600000UL
#define SENSOR_MS            500UL   // local sensor refresh
#define GNSS_POLL_MS        1000UL
#define GRID_EVERY_N            1    // 1 = full 8x8 ToF grid every packet.
                                     // Raise to 10 if your SIM plan hurts.
#define MQTT_RETRY_MS_MIN   5000UL
#define MQTT_RETRY_MS_MAX 120000UL

/* ---------------- adaptive reporting (FEAT_ADAPTIVE) ----------------
   Moving fast -> frequent; parked -> rare; any event -> immediate.
   This is the single biggest saver of both data and battery.          */
#define FEAT_ADAPTIVE           1
#define REPORT_MS_MOVING   10000UL   // speed above MOVING_KMH
#define REPORT_MS_IDLE     60000UL   // has a fix, barely moving
#define REPORT_MS_PARKED  300000UL   // stationary longer than PARKED_AFTER_MS
#define MOVING_KMH            3.0f
#define PARKED_AFTER_MS   600000UL   // 10 min of stillness = parked

/* ---------------- emergency / low-battery modes --------------------- */
#define FEAT_EMERGENCY          1
#define REPORT_MS_EMERGENCY 900000UL  // 15 min, position-only packets
#define EMERGENCY_SOC_PCT        10   // enter at or below this
#define EMERGENCY_EXIT_PCT       15   // leave above this (hysteresis)
#define FEAT_LAST_GASP           1
#define LAST_GASP_SOC_PCT         3   // final position publish, then idle
#define LAST_GASP_VOLTS        6.20f  // ...or this pack voltage, whichever first

/* ---------------------------------------------------------------- battery
   2S1P LiPo, 2500 mAh. INA226 with an R010 (0.01 ohm) shunt, high-side
   on the pack lead. Bus voltage read over I2C via the module's VBS pin. */
#define BATT_CELLS            2
#define BATT_CAPACITY_MAH  2500.0f  // coulomb counting needs this to be right
#define INA226_ADDR        0x40
#define SHUNT_OHMS        0.01f     // R010. Wrong here = everything wrong.
#define SHUNT_MAX_AMPS     8.0f     // sets the current LSB (8/32768 = 244 uA)
#define BATT_IR_OHMS      0.120f    // WHOLE-pack internal resistance, for OCV
                                    // CALIBRATE: docs/SETUP.md "fuel gauge"
#define CURRENT_SIGN        (+1)    // flip to -1 if charge/discharge inverted
#define CHARGING_MA_MIN    30.0f    // above this (after sign fix) = charging
#define SOC_LOW_PCT           20    // red blink @ 1 s
#define SOC_CRIT_PCT          10    // red blink @ 250 ms
#define BATT_CHIRP_MS    60000UL    // low-battery chirp period; 0 = silent

/* ---------------- fuel-gauge filtering (FEAT_FUEL_FUSION) -----------
   Raw INA226 samples are noisy and a voltage-only SoC jumps around
   under load. Three stages: median-of-5 despike, then an EMA, then
   coulomb counting corrected slowly toward the OCV curve whenever the
   pack is resting. See battery.cpp for the whole story.               */
#define FEAT_FUEL_FUSION        1
#define FILT_MEDIAN_N           5    // despike window (odd, <=9)
#define FILT_EMA_ALPHA      0.20f    // 0..1, lower = smoother/slower
#define REST_CURRENT_MA     60.0f    // below this the pack counts as resting
#define REST_SETTLE_MS     20000UL   // ...for this long before OCV is trusted
#define OCV_CORRECT_RATE    0.02f    // fraction of the OCV error folded in
                                     // per correction; small = smooth
#define SOC_SAVE_EVERY_MS 300000UL   // persist coulomb state to NVS (5 min)
#define SOC_SAVE_DELTA_PCT        2  // ...or when SoC moves this much

/* ---------------------------------------------------------------- geofence
   Up to 4 zones, pushed from the dashboard as one retained JSON array
   and cached in NVS, so the buzzer still fires with the dashboard
   closed and the server down.                                         */
#define FENCE_MAX_ZONES         4
#define FENCE_HYST_M        15.0f   // metres of hysteresis, stops chatter
#define FENCE_MIN_SATS          5   // ignore fixes weaker than this
#define FENCE_MAX_HDOP       3.0f

/* ---------------- trips + odometer (FEAT_TRIPS) --------------------- */
#define FEAT_TRIPS              1
#define TRIP_START_KMH       5.0f   // sustained speed that opens a trip
#define TRIP_START_MS       15000UL // ...for this long
#define TRIP_STOP_KMH        2.0f   // below this
#define TRIP_STOP_MS       120000UL // ...for this long closes the trip

/* ---------------- accident / crash detection (FEAT_CRASH) -----------
   Pattern: travelling at speed -> a HARSH deceleration -> a standstill
   that persists. All three inside CRASH_WINDOW_MS.

   The discriminator is deceleration in g, not a km/h drop. A red-light
   stop from 40 km/h takes ~5 s = 0.22 g. Emergency braking is 0.6-0.8 g.
   A real impact is many g, but 1 Hz GNSS can only see it as "all the
   speed vanished between two samples", which still lands above 0.6 g.
   Anything looser than ~0.5 g WILL fire at every traffic light.

   Honest limit: GNSS speed at 1 Hz is a weak crash sensor. It cannot see
   a low-speed knock, a rollover, or an impact while already stopped. If
   this matters, add an MPU6050/LSM6DSO on the same I2C bus — then it
   becomes a real accelerometer trigger with this as corroboration. */
#define FEAT_CRASH              1
#define CRASH_MIN_SPEED_KMH  25.0f  // must have been going at least this fast
#define CRASH_DECEL_G         0.60f // sustained decel over one GNSS interval
#define CRASH_STILL_KMH        2.0f // then below this
#define CRASH_STILL_MS       15000UL// ...for this long, with no recovery
#define CRASH_WINDOW_MS      25000UL// decel and standstill must share this window
#define CRASH_REARM_MS      120000UL// ignore further crashes for this long
#define CRASH_CANCEL_MS      60000UL// moving again within this -> "cleared" event

/* ---------------------------------------------------------------- offline buffer */
#define REPLAY_SLOTS           60   // packets held in PSRAM while 4G is down
#define REPLAY_SLOT_BYTES    2560   // packets grew with trips+fence+diag

/* ---------------------------------------------------------------- reliability */
#define FEAT_WATCHDOG           1
#define WDT_TIMEOUT_S          90   // must exceed the slowest AT round trip
#define FEAT_TIME_SYNC          1   // AT+CCLK -> real epoch, fixes ts:0
#define TIME_RESYNC_MS   3600000UL  // re-read network time hourly
#define FEAT_MODEM_TEMP         1   // AT+CPMUTEMP as crude ambient
#define FEAT_LBS_FALLBACK       1   // cell-tower position when GNSS has no fix
#define LBS_RETRY_MS     120000UL   // don't hammer the LBS service
#define LBS_AFTER_NO_FIX_MS 180000UL// only after GNSS has failed this long

/* ---------------- SMS fallback + SMS commands (FEAT_SMS) ------------ */
#define FEAT_SMS                1
#define SMS_OWNER        "+919999999999"  // E.164. Empty string = outbound off
#define SMS_ON_DATA_DOWN_MS 900000UL      // text a position after 15 min offline
#define SMS_MIN_GAP_MS     1800000UL      // never more than one SMS / 30 min
#define SMS_ALLOW_ANY_SENDER    0         // 0 = only SMS_OWNER may command it

/* ---------------------------------------------------------------- misc */
#define FIND_DEFAULT_S         30   // find-device duration if none supplied
#define SERIAL_BAUD       115200
#define AT_DEBUG               1    // 1 = echo all modem traffic to USB serial
