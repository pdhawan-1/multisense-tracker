/* =====================================================================
   tracker.h — the behaviour that makes this a tracker rather than a
   sensor logger: trips, multi-zone geofencing, accident detection and
   an adaptive report rate.

   Everything here is driven by GNSS fixes and lives in NVS, so it keeps
   working with the dashboard closed and the server asleep.
   ===================================================================== */
#pragma once
#include <stdint.h>
#include "a7672s.h"
#include "config.h"

struct Zone {
  bool   enabled = false;
  char   name[16] = "";
  double lat = 0, lon = 0;
  float  r = 0;
  bool   outside = false;
  bool   known   = false;      // first fix seen -> state established
};

struct Trip {
  bool     active   = false;
  uint32_t startMs  = 0;
  uint32_t startEpoch = 0;
  double   km       = 0;
  float    maxKmh   = 0;
  double   lat0 = 0, lon0 = 0;
};

enum TrkEventType : uint8_t {
  EV_NONE = 0, EV_FENCE_EXIT, EV_FENCE_ENTER, EV_TRIP_START, EV_TRIP_END,
  EV_ACCIDENT, EV_ACCIDENT_CLEARED
};

struct TrkEvent {
  TrkEventType type = EV_NONE;
  char   detail[160] = "";
  double lat = 0, lon = 0;
  float  speed_kmh = 0;
};

void  trkBegin();
void  trkOnFix(const GnssFix& f);        // call on every GNSS poll
bool  trkApplyFenceJson(const char* json);  // from the retained MQTT config
bool  trkTakeEvent(TrkEvent& out);       // drain one queued event

uint32_t trkReportInterval(bool emergency);
bool  trkMoving();
bool  trkParked();
double trkOdometerKm();
const Trip& trkTrip();
const Zone* trkZones();                  // FENCE_MAX_ZONES entries
uint8_t trkZonesOutside();               // count currently outside
bool  trkAccidentActive();
bool  trkSelfCheck();
