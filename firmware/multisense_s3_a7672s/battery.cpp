#include "battery.h"
#include "config.h"
#include <Arduino.h>
#include <INA226.h>
#include <Preferences.h>

static INA226      ina(INA226_ADDR);
static Preferences bnvs;
static BattState   S;

/* ---------------------------------------------------------------- curve
   Per-cell Li-ion/LiPo open-circuit curve. Pack volts are divided by
   BATT_CELLS and IR-compensated before this is consulted.             */
int socFromCell(float vcell) {
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
    if (vcell >= V[i]) {
      const float f = (vcell - V[i]) / (V[i - 1] - V[i]);
      return (int)(P[i] + f * (P[i - 1] - P[i]) + 0.5f);
    }
  return 0;
}

/* ---------------------------------------------------------------- filters */
float medianOf(const float* buf, int n) {
  float t[9];
  if (n > 9) n = 9;
  for (int i = 0; i < n; i++) t[i] = buf[i];
  for (int i = 1; i < n; i++) {                 // insertion sort, n<=9
    const float k = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return t[n / 2];
}

/* mah += ma * hours. ma is +ve while charging, so the sign works out. */
float coulombStep(float mah, float ma, uint32_t dtMs, float capMah) {
  mah += ma * ((float)dtMs / 3600000.0f);
  if (mah < 0) mah = 0;
  if (mah > capMah) mah = capMah;
  return mah;
}

/* ---------------------------------------------------------------- state */
static float vBuf[FILT_MEDIAN_N], iBuf[FILT_MEDIAN_N];
static int   bufN = 0, bufI = 0;
static float vEma = 0, iEma = 0;
static bool  emaPrimed = false;
static uint32_t tLast = 0, tRestSince = 0, tSave = 0;
static int   socAtSave = -1;

static int ocvSoc(float vPack, float maNow) {
  /* IR-compensate to open circuit: ma is -ve on discharge, so
     v - I*R adds the sag back on. BATT_IR_OHMS is the whole pack. */
  const float vOc = vPack - (maNow / 1000.0f) * BATT_IR_OHMS;
  return socFromCell(vOc / BATT_CELLS);
}

bool battBegin() {
  S.present = ina.begin();
  if (!S.present) { Serial.println("INA226 not found"); return false; }
  ina.setMaxCurrentShunt(SHUNT_MAX_AMPS, SHUNT_OHMS);
  ina.setAverage(4);                            // 128 samples internally

  /* prime the filters from one real reading so the first packet isn't 0 */
  const float v0 = ina.getBusVoltage();
  const float i0 = ina.getCurrent_mA() * CURRENT_SIGN;
  vEma = S.v = S.vRaw = v0;
  iEma = S.ma = S.maRaw = i0;
  emaPrimed = true;
  for (int k = 0; k < FILT_MEDIAN_N; k++) { vBuf[k] = v0; iBuf[k] = i0; }
  bufN = FILT_MEDIAN_N;

  S.socOcv = ocvSoc(v0, i0);

  /* restore the coulomb counter, but sanity-check it against the curve:
     the pack may have been charged or swapped while we were powered off,
     in which case the stored value is a lie. */
  bnvs.begin("fuel", true);
  const float stored = bnvs.getFloat("mah", -1);
  bnvs.end();

  const float ocvMah = (S.socOcv / 100.0f) * BATT_CAPACITY_MAH;
  if (stored < 0) {
    S.mah = ocvMah;
    S.src = "ocv";
    Serial.printf("fuel: no stored state, starting from OCV %d%%\n", S.socOcv);
  } else if (fabsf(stored - ocvMah) > 0.20f * BATT_CAPACITY_MAH) {
    S.mah = ocvMah;
    S.src = "ocv";
    Serial.printf("fuel: stored %.0f mAh disagrees with OCV %.0f mAh by >20%%"
                  " — pack charged or swapped while off, trusting OCV\n",
                  stored, ocvMah);
  } else {
    S.mah = stored;
    S.src = "fused";
    Serial.printf("fuel: restored %.0f mAh (OCV says %.0f)\n", stored, ocvMah);
  }
  S.soc = (int)(100.0f * S.mah / BATT_CAPACITY_MAH + 0.5f);
  tLast = tSave = millis();
  socAtSave = S.soc;
  return true;
}

void battSave() {
  bnvs.begin("fuel", false);
  bnvs.putFloat("mah", S.mah);
  bnvs.end();
  socAtSave = S.soc;
  tSave = millis();
}

void battTick() {
  if (!S.present) return;
  const uint32_t now = millis();
  if (now == tLast) return;

  /* ---- 1. sample ---- */
  S.vRaw  = ina.getBusVoltage();
  S.maRaw = ina.getCurrent_mA() * CURRENT_SIGN;

  /* ---- 2. median despike ---- */
  vBuf[bufI] = S.vRaw;
  iBuf[bufI] = S.maRaw;
  bufI = (bufI + 1) % FILT_MEDIAN_N;
  if (bufN < FILT_MEDIAN_N) bufN++;
  const float vMed = medianOf(vBuf, bufN);
  const float iMed = medianOf(iBuf, bufN);

  /* ---- 3. EMA ---- */
  if (!emaPrimed) { vEma = vMed; iEma = iMed; emaPrimed = true; }
  vEma += FILT_EMA_ALPHA * (vMed - vEma);
  iEma += FILT_EMA_ALPHA * (iMed - iEma);
  S.v  = vEma;
  S.ma = iEma;
  S.charging = S.ma > CHARGING_MA_MIN;

  /* ---- 4. coulomb count ---- */
  const uint32_t dt = now - tLast;
  tLast = now;
#if FEAT_FUEL_FUSION
  S.mah = coulombStep(S.mah, S.ma, dt, BATT_CAPACITY_MAH);

  /* ---- 5. rest detection + OCV correction ----
     Only a resting pack's terminal voltage is near its OCV. Under the
     modem's 2 A bursts it is not, which is exactly when a naive
     voltage gauge reports a flat battery. */
  if (fabsf(S.ma) < REST_CURRENT_MA) {
    if (!tRestSince) tRestSince = now;
    S.resting = (now - tRestSince) >= REST_SETTLE_MS;
  } else {
    tRestSince = 0;
    S.resting  = false;
  }

  S.socOcv = ocvSoc(S.v, S.ma);
  if (S.resting) {
    const float target = (S.socOcv / 100.0f) * BATT_CAPACITY_MAH;
    S.mah += (target - S.mah) * OCV_CORRECT_RATE;   // slow, never a jump
    S.src  = "fused";
  }
#else
  S.socOcv = ocvSoc(S.v, S.ma);
  S.mah    = (S.socOcv / 100.0f) * BATT_CAPACITY_MAH;
  S.src    = "ocv";
#endif

  S.soc = (int)(100.0f * S.mah / BATT_CAPACITY_MAH + 0.5f);
  if (S.soc < 0) S.soc = 0;
  if (S.soc > 100) S.soc = 100;

  /* ---- 6. persist, so a reboot doesn't lose the integral ---- */
  if (now - tSave >= SOC_SAVE_EVERY_MS || abs(S.soc - socAtSave) >= SOC_SAVE_DELTA_PCT)
    battSave();
}

const BattState& battGet() { return S; }

/* ---------------------------------------------------------------- self-check
   Runs at boot. These are the four bits of maths that fail SILENTLY —
   a wrong answer still looks like a plausible battery percentage.    */
bool battSelfCheck() {
  bool ok = true;

  /* curve: rails and monotonic */
  if (socFromCell(4.30f) != 100 || socFromCell(2.50f) != 0) ok = false;
  for (float v = 3.0f; v < 4.2f; v += 0.02f)
    if (socFromCell(v) > socFromCell(v + 0.02f)) ok = false;
  if (socFromCell(3.85f) < 45 || socFromCell(3.85f) > 55) ok = false;

  /* median must reject a single wild spike, which is the whole point */
  const float spike[5] = {7.40f, 7.41f, 99.0f, 7.39f, 7.40f};
  const float m = medianOf(spike, 5);
  if (m < 7.35f || m > 7.45f) ok = false;

  /* coulomb: 1 A out of a full pack for 30 min removes 500 mAh = 20 % */
  float mah = BATT_CAPACITY_MAH;
  mah = coulombStep(mah, -1000.0f, 1800000UL, BATT_CAPACITY_MAH);
  if (fabsf(mah - (BATT_CAPACITY_MAH - 500.0f)) > 1.0f) ok = false;

  /* and it must clamp at both rails rather than go negative or overfill */
  if (coulombStep(10.0f, -5000.0f, 3600000UL, BATT_CAPACITY_MAH) != 0.0f) ok = false;
  if (coulombStep(BATT_CAPACITY_MAH - 5, 5000.0f, 3600000UL, BATT_CAPACITY_MAH)
      != BATT_CAPACITY_MAH) ok = false;

  Serial.printf("battery self-check: %s  (soc@3.85V/cell=%d%%, "
                "median rejects spike=%.2f, 1A/30min=%.0f mAh left)\n",
                ok ? "PASS" : "FAIL", socFromCell(3.85f), m, mah);
  return ok;
}
