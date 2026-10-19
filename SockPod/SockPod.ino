/*
  =====================================================================
   SockPod  —  ESP32 + MPU-6050 + BMP280
   A small pod worn on the sock that tracks how you move.
  =====================================================================

  WHAT SOCKPOD TRACKS
    Movement  : active time vs rest time, and how intense the movement is
    Steps     : step count and cadence (steps per minute) while running
    Jumps     : jump count, and jump height from time in the air
    Sprints   : bursts of fast, high-cadence running, counted
    Elevation : climb and descent from air pressure (hills, stairs, slopes)
    Sync      : live stats and session summaries over Bluetooth LE

  HOW EACH ONE IS MEASURED  (MPU-6050 at 100 Hz, BMP280 at 10 Hz)
    Gravity   : whenever the pod is still for a second, the |accel| it reads
                then is taken as 1 g, so offsets in cheap sensors cancel out.
    Intensity : ENMO = average of max(|accel| - 1 g, 0) over each second.
                Below REST_ENMO_G with no foot strike = rest, otherwise
                light / moderate / vigorous.
    Steps     : each foot strike is an acceleration spike above STEP_G.
                The pod is on one foot, so steps = 2 x strikes.
    Jumps     : the foot reads ~0 g while in the air. Airtime t runs from
                dropping below AIR_G to the landing spike above LAND_G,
                and jump height = g * t^2 / 8.
    Sprints   : cadence >= SPRINT_CADENCE and vigorous intensity for at
                least SPRINT_MIN_S seconds in a row.
    Elevation : altitude from pressure relative to the session start;
                changes under ELEV_STEP_M are ignored as noise. The BMP280's
                ~1 m resolution is fine for hills and stairs but too coarse
                for jump height, which is why the MPU-6050 handles jumps.

  WIRING  (both sensors share the I2C bus)
    MPU-6050 / GY-521 : VCC -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
                        AD0 unconnected (address 0x68)
    BMP280            : VCC -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
                        address 0x76 (SDO to GND) or 0x77 (SDO to 3V3)
    The pod runs without the BMP280; elevation is then reported as 0.

  CONTROLS
    BOOT button : press to start / stop a session
    LED (GPIO2) : short blip every 2 s = idle, 1 Hz blink = recording

  SERIAL COMMANDS  (115200 baud)
    s = start / stop a session
    l = live stats as JSON once a second (on / off)
    p = Serial Plotter mode (on / off)
    r = print the last finished session as JSON

  BLUETOOTH LE  (device name "SockPod"; UUIDs and JSON fields in README.md)
    live    : read + notify, JSON, once a second
    session : read, JSON summary of the last finished session (kept in flash)
    control : write "start" or "stop"

  The thresholds below are starting points, not measured values. Tune them
  with the 'p' and 'l' modes while walking, running and jumping.

  No extra libraries needed — only the ESP32 board package.
*/

#include <Wire.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

// ======================= TUNING ==================================
// Values marked "measured" come from the 2026-10-19 motion test (on-the-spot
// walking, jogging, jumping and sprinting; motion_logs/motion_20261019_192001.csv).
// Steps (one foot, so each strike = 2 steps)
const float    STEP_G          = 1.8;    // foot strike. Measured: on-the-spot walking peaks 1.7-3.0 g
const float    STEP_REARM_G    = 1.3;    // must drop below this before the next strike
const uint32_t MIN_STRIDE_MS   = 450;    // ignore strikes closer than this (caps cadence at 266 steps/min)
const uint32_t CADENCE_STOP_MS = 2000;   // no strike for this long = cadence 0
// Jumps (airtime)
const float    AIR_G           = 0.6;    // below this = foot in free fall. Measured: jumps dip to 0.3-0.6 g
const float    LAND_G          = 1.5;    // above this = landed. Measured: landings peak 2.1-2.9 g
const uint32_t AIR_GLITCH_MS   = 80;     // bumps allowed while in the air (the foot jiggles; legs extend)
const uint32_t MIN_AIR_MS      = 250;    // shorter = a stride, not a jump (~8 cm). Measured: walking gave a 241 ms false jump
const uint32_t MAX_AIR_MS      = 1000;   // longer = pod dropped or thrown (~1.2 m)
// Intensity (ENMO, in g, averaged over 1 s). Measured: standing <= 0.06,
// walking 0.10-0.31, jogging 0.17-0.33, sprinting 0.78-1.43
const float    REST_ENMO_G     = 0.07;
const float    MODERATE_ENMO_G = 0.15;
const float    VIGOROUS_ENMO_G = 0.60;
// Gravity calibration: a second this steady counts as still, and whatever
// |accel| reads then is taken as 1 g (cheap MPU-6050 boards read 1.1-1.2 g).
// Measured: standing still varies 0.056-0.087 g per second, a desk 0.01 g
const float    STILL_SD_G      = 0.08;
// Sprints
const uint16_t SPRINT_CADENCE  = 180;    // steps per minute
const uint32_t SPRINT_MIN_S    = 3;      // must last this long to count
const uint32_t SPRINT_GAP_S    = 1;      // slower seconds allowed inside a sprint
// Elevation
const float    ELEV_STEP_M     = 1.0;    // ignore altitude changes smaller than this
const float    BARO_ALPHA      = 0.1;    // pressure smoothing per 10 Hz sample

// ======================= PINS & TIMING ===========================
const int      SDA_PIN   = 21;
const int      SCL_PIN   = 22;
const int      LED_PIN   = 2;            // on-board LED on most ESP32 DevKits
const int      BTN_PIN   = 0;            // BOOT button, active LOW
const uint8_t  MPU_ADDR  = 0x68;
const uint32_t I2C_HZ    = 100000;       // 100 kHz copes with long wires down to the foot
const uint32_t SAMPLE_MS = 10;           // MPU-6050 at 100 Hz
const uint32_t BARO_MS   = 100;          // BMP280 at 10 Hz
const uint32_t TICK_MS   = 1000;         // stats, BLE notify, live output

// ======================= BLUETOOTH LE ============================
#define DEVICE_NAME  "SockPod"
#define SERVICE_UUID "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a00"
#define LIVE_UUID    "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a01"   // read + notify
#define SESSION_UUID "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a02"   // read
#define CONTROL_UUID "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a03"   // write

// ======================= STATE ===================================
struct Session {
  uint32_t id, durS;                     // durS = activeS + restS
  uint32_t activeS, restS, lightS, moderateS, vigorousS;
  float    enmoSum;                      // summed over active seconds
  uint32_t strikes, gaitS;               // gaitS = seconds with cadence > 0
  uint16_t maxCadence;
  uint16_t jumps;
  float    maxJumpCm, sumJumpCm;
  uint16_t sprints;
  uint32_t sprintS, longestSprintS;
  float    gainM, lossM;
};

Session  ses;                             // current session, or the last one after stop
bool     recording = false;
uint32_t sessionCounter = 0;
String   lastSessionJson = "{\"id\":0}";
Preferences prefs;

// Steps and cadence
bool     stepArmed = true;
uint32_t lastStrikeMs = 0, stepSuppressUntil = 0;
uint32_t strikeTimes[4], strikeCount = 0;     // ring of recent strikes for cadence
bool     strikeThisTick = false;
// Jumps
enum JumpState { GROUND, AIR };
JumpState jumpState = GROUND;
uint32_t takeoffMs = 0, airBumpSince = 0;
float    lastJumpCm = 0;
// Intensity
float    enmoAcc = 0;
uint16_t enmoN = 0;
// Gravity calibration
float    gBase = 1.0;                    // raw |accel| that means 1 g on this board
float    gBaseSaved = 0;                 // last value written to flash
float    stillSum = 0, stillSq = 0;
uint16_t stillN = 0;
bool     gBaseLogged = false;
// Sprints
bool     inSprint = false;
uint32_t sprintRunS = 0, sprintGapS = 0;
// Elevation
uint8_t  bmpAddr = 0;                    // 0 = no BMP280
uint16_t dig_T1, dig_P1;
int16_t  dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
float    pFilt = 0, p0 = 0, altM = 0, elevRef = 0;
// Timing, controls, output
uint32_t lastSample = 0, lastBaro = 0, lastTick = 0, plotCounter = 0;
uint16_t mpuFailures = 0;                // consecutive failed reads
uint32_t btnDownSince = 0;
bool     btnWasDown = false, plotMode = false, liveMode = false;
// BLE (callbacks run on the Bluetooth task, so they only set flags)
enum Command : uint8_t { CMD_NONE, CMD_START, CMD_STOP };
volatile uint8_t pendingCmd = CMD_NONE;
volatile bool    bleConnected = false;
bool     bleWasConnected = false;
BLECharacteristic* liveChar = nullptr;
BLECharacteristic* sessionChar = nullptr;

// Status messages are hidden in plot mode so the graph stays clean.
void logf(const char* fmt, ...) {
  if (plotMode) return;
  char buf[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
}

// ======================= I2C =====================================
bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void scanI2C() {
  String found;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found += String(" 0x") + String(a, HEX);
  }
  Serial.print("[I2C] Devices found:");
  Serial.println(found.length() ? found : String(" none"));
}

// ======================= MPU-6050 ================================
bool mpuInit() {
  if (!i2cWrite(MPU_ADDR, 0x6B, 0x01)) return false;  // wake up, gyro clock
  delay(50);
  i2cWrite(MPU_ADDR, 0x1A, 0x03);   // digital low-pass filter ~44 Hz
  i2cWrite(MPU_ADDR, 0x19, 9);      // sample rate 1 kHz / (1+9) = 100 Hz
  i2cWrite(MPU_ADDR, 0x1B, 0x18);   // gyro range  +-2000 deg/s (16.4 LSB per deg/s)
  i2cWrite(MPU_ADDR, 0x1C, 0x18);   // accel range +-16 g      (2048 LSB per g); foot strikes are hard
  return true;
}

// Returns total acceleration (g) and total rotation rate (deg/s).
bool readMPU(float& accG, float& gyroDps) {
  uint8_t d[14];
  if (!i2cRead(MPU_ADDR, 0x3B, d, 14)) return false;
  int16_t v[7];
  for (int i = 0; i < 7; i++) v[i] = (int16_t)((d[2 * i] << 8) | d[2 * i + 1]);
  float ax = v[0] / 2048.0, ay = v[1] / 2048.0, az = v[2] / 2048.0;
  float gx = v[4] / 16.4,   gy = v[5] / 16.4,   gz = v[6] / 16.4;
  accG    = sqrtf(ax * ax + ay * ay + az * az);
  gyroDps = sqrtf(gx * gx + gy * gy + gz * gz);
  return true;
}

// ======================= BMP280 ==================================
bool bmpInit() {
  const uint8_t addrs[] = { 0x76, 0x77 };
  for (uint8_t a : addrs) {
    uint8_t id;
    if (!i2cRead(a, 0xD0, &id, 1)) continue;
    // 0x56-0x58 = BMP280; 0x60 = BME280, whose pressure maths is identical
    if (id != 0x56 && id != 0x57 && id != 0x58 && id != 0x60) {
      Serial.printf("[BMP] Device at 0x%02X has chip ID 0x%02X, not a BMP280\n", a, id);
      continue;
    }
    uint8_t c[24];
    if (!i2cRead(a, 0x88, c, 24)) continue;
    auto u16 = [&](int i) { return (uint16_t)(c[i] | (c[i + 1] << 8)); };
    dig_T1 = u16(0);           dig_T2 = (int16_t)u16(2);  dig_T3 = (int16_t)u16(4);
    dig_P1 = u16(6);           dig_P2 = (int16_t)u16(8);  dig_P3 = (int16_t)u16(10);
    dig_P4 = (int16_t)u16(12); dig_P5 = (int16_t)u16(14); dig_P6 = (int16_t)u16(16);
    dig_P7 = (int16_t)u16(18); dig_P8 = (int16_t)u16(20); dig_P9 = (int16_t)u16(22);
    i2cWrite(a, 0xF5, 0x30);   // standby 62.5 ms, IIR filter x16 (write while still asleep)
    i2cWrite(a, 0xF4, 0x57);   // temperature x2, pressure x16, normal mode
    bmpAddr = a;
    Serial.printf("[BMP] BMP280 ready at 0x%02X\n", a);
    return true;
  }
  return false;
}

// Pressure in Pa, using the integer compensation from the Bosch datasheet.
bool readBMP(float& pa) {
  uint8_t d[6];
  if (!i2cRead(bmpAddr, 0xF7, d, 6)) return false;
  int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
  if (adcP == 0x80000 || adcT == 0x80000) return false;   // no measurement yet

  int32_t v1 = ((((adcT >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
  int32_t v2 = (((((adcT >> 4) - ((int32_t)dig_T1)) * ((adcT >> 4) - ((int32_t)dig_T1))) >> 12) *
                ((int32_t)dig_T3)) >> 14;
  int32_t tFine = v1 + v2;

  int64_t a = (int64_t)tFine - 128000;
  int64_t b = a * a * (int64_t)dig_P6;
  b = b + ((a * (int64_t)dig_P5) << 17);
  b = b + (((int64_t)dig_P4) << 35);
  a = ((a * a * (int64_t)dig_P3) >> 8) + ((a * (int64_t)dig_P2) << 12);
  a = (((((int64_t)1) << 47) + a)) * ((int64_t)dig_P1) >> 33;
  if (a == 0) return false;
  int64_t p = 1048576 - adcP;
  p = (((p << 31) - b) * 3125) / a;
  a = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
  b = (((int64_t)dig_P8) * p) >> 19;
  p = ((p + a + b) >> 8) + (((int64_t)dig_P7) << 4);
  pa = p / 256.0f;
  return true;
}

// Altitude relative to p0, with small changes ignored as noise.
void baroUpdate() {
  float pa;
  if (!bmpAddr || !readBMP(pa)) return;
  if (pFilt == 0) pFilt = pa;
  pFilt += BARO_ALPHA * (pa - pFilt);
  if (p0 == 0) p0 = pFilt;
  altM = 44330.0f * (1.0f - powf(pFilt / p0, 0.190295f));
  if (altM > elevRef + ELEV_STEP_M) {
    if (recording) ses.gainM += altM - elevRef;
    elevRef = altM;
  } else if (altM < elevRef - ELEV_STEP_M) {
    if (recording) ses.lossM += elevRef - altM;
    elevRef = altM;
  }
}

void baroZero() {
  if (pFilt > 0) p0 = pFilt;
  altM = 0;
  elevRef = 0;
}

// ======================= STEPS & CADENCE =========================
void onStrike(uint32_t now) {
  if (now - lastStrikeMs > CADENCE_STOP_MS) strikeCount = 0;   // fresh start after a pause
  strikeTimes[strikeCount % 4] = now;
  strikeCount++;
  lastStrikeMs = now;
  strikeThisTick = true;
  if (recording) ses.strikes++;
}

void processStep(float a, uint32_t now) {
  if (a < STEP_REARM_G) { stepArmed = true; return; }
  if (!stepArmed || jumpState == AIR || now < stepSuppressUntil) return;
  if (a > STEP_G && now - lastStrikeMs >= MIN_STRIDE_MS) {
    stepArmed = false;
    onStrike(now);
  }
}

// Steps per minute from the last few strikes of this foot.
uint16_t cadence(uint32_t now) {
  if (strikeCount < 3 || now - lastStrikeMs > CADENCE_STOP_MS) return 0;
  uint32_t n = strikeCount < 4 ? strikeCount : 4;
  uint32_t newest = strikeTimes[(strikeCount - 1) % 4];
  uint32_t oldest = strikeTimes[(strikeCount - n) % 4];
  if (newest == oldest) return 0;
  return (uint16_t)(2UL * 60000UL * (n - 1) / (newest - oldest));   // 2 steps per strike
}

// ======================= JUMPS ===================================
void processJump(float a, uint32_t now) {
  if (jumpState == GROUND) {
    if (a < AIR_G) { jumpState = AIR; takeoffMs = now; airBumpSince = 0; }
    return;
  }
  if (a > LAND_G) {
    jumpState = GROUND;
    uint32_t air = now - takeoffMs;
    if (air >= MIN_AIR_MS && air <= MAX_AIR_MS) {
      float t = air / 1000.0f;
      lastJumpCm = 9.81f * t * t / 8.0f * 100.0f;
      stepSuppressUntil = now + MIN_STRIDE_MS;   // the landing is not a step
      if (recording) {
        ses.jumps++;
        ses.sumJumpCm += lastJumpCm;
        if (lastJumpCm > ses.maxJumpCm) ses.maxJumpCm = lastJumpCm;
      }
      logf("[JUMP] %lu ms in the air -> %.1f cm", (unsigned long)air, lastJumpCm);
    } else if (air > MAX_AIR_MS) {
      logf("[JUMP] Ignored %lu ms in the air (dropped or thrown?)", (unsigned long)air);
    }
  } else if (a >= AIR_G) {
    if (airBumpSince == 0) airBumpSince = now;
    if (now - airBumpSince > AIR_GLITCH_MS) jumpState = GROUND;   // a leg swing, not free fall
  } else {
    airBumpSince = 0;
  }
}

// ======================= SPRINTS =================================
void endSprint() {
  if (inSprint) {
    ses.sprintS += sprintRunS;
    if (sprintRunS > ses.longestSprintS) ses.longestSprintS = sprintRunS;
    logf("[SPRINT] Ended after %lu s", (unsigned long)sprintRunS);
  }
  inSprint = false;
  sprintRunS = 0;
  sprintGapS = 0;
}

void updateSprint(uint16_t cad, float enmo) {
  if (cad >= SPRINT_CADENCE && enmo >= VIGOROUS_ENMO_G) {
    sprintRunS++;
    sprintGapS = 0;
    if (!inSprint && sprintRunS >= SPRINT_MIN_S) {
      inSprint = true;
      ses.sprints++;
      logf("[SPRINT] #%u started", ses.sprints);
    }
  } else if (sprintRunS > 0 && ++sprintGapS > SPRINT_GAP_S) {
    endSprint();
  }
}

// ======================= OUTPUT ==================================
void setChar(BLECharacteristic* c, const char* s, size_t n) {
  if (c) c->setValue((uint8_t*)s, n);
}

String sessionJson() {
  char buf[420];
  float    avgEnmo = ses.activeS ? ses.enmoSum / ses.activeS : 0;
  uint32_t avgCad  = ses.gaitS ? ses.strikes * 2UL * 60UL / ses.gaitS : 0;
  float    avgJump = ses.jumps ? ses.sumJumpCm / ses.jumps : 0;
  snprintf(buf, sizeof(buf),
    "{\"id\":%lu,\"dur\":%lu,\"act\":%lu,\"rest\":%lu,\"light\":%lu,\"mod\":%lu,\"vig\":%lu,"
    "\"enmo\":%.2f,\"steps\":%lu,\"gait\":%lu,\"cad_avg\":%lu,\"cad_max\":%u,"
    "\"jumps\":%u,\"jump_max\":%.1f,\"jump_avg\":%.1f,"
    "\"sprints\":%u,\"sprint_s\":%lu,\"sprint_max\":%lu,"
    "\"up\":%.1f,\"dn\":%.1f,\"baro\":%d}",
    (unsigned long)ses.id, (unsigned long)ses.durS, (unsigned long)ses.activeS, (unsigned long)ses.restS,
    (unsigned long)ses.lightS, (unsigned long)ses.moderateS, (unsigned long)ses.vigorousS,
    avgEnmo, (unsigned long)(ses.strikes * 2), (unsigned long)ses.gaitS, (unsigned long)avgCad,
    ses.maxCadence, ses.jumps, ses.maxJumpCm, avgJump,
    ses.sprints, (unsigned long)ses.sprintS, (unsigned long)ses.longestSprintS,
    ses.gainM, ses.lossM, bmpAddr ? 1 : 0);
  return String(buf);
}

void publishLive(uint8_t zone, float enmo, uint16_t cad) {
  char buf[220];
  int n = snprintf(buf, sizeof(buf),
    "{\"rec\":%d,\"t\":%lu,\"act\":%lu,\"rest\":%lu,\"zone\":%u,\"enmo\":%.2f,"
    "\"steps\":%lu,\"cad\":%u,\"jumps\":%u,\"jlast\":%.1f,\"spr\":%u,"
    "\"up\":%.1f,\"dn\":%.1f,\"alt\":%.1f,\"baro\":%d,\"last\":%lu}",
    recording ? 1 : 0, (unsigned long)ses.durS, (unsigned long)ses.activeS, (unsigned long)ses.restS,
    zone, enmo, (unsigned long)(ses.strikes * 2), cad, ses.jumps, lastJumpCm, ses.sprints,
    ses.gainM, ses.lossM, altM, bmpAddr ? 1 : 0, (unsigned long)sessionCounter);
  if (n < 0) return;
  if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
  setChar(liveChar, buf, n);
  if (bleConnected && liveChar) liveChar->notify();
  if (liveMode && !plotMode) Serial.println(buf);
}

// ======================= SESSIONS ================================
void startSession() {
  if (recording) return;
  memset(&ses, 0, sizeof(ses));
  ses.id = ++sessionCounter;
  inSprint = false; sprintRunS = 0; sprintGapS = 0;
  lastJumpCm = 0;
  baroZero();
  recording = true;
  logf("[SESSION] #%lu started", (unsigned long)ses.id);
}

void stopSession() {
  if (!recording) return;
  endSprint();
  recording = false;
  lastSessionJson = sessionJson();
  prefs.putUInt("id", ses.id);
  prefs.putString("last", lastSessionJson);
  setChar(sessionChar, lastSessionJson.c_str(), lastSessionJson.length());
  logf("[SESSION] #%lu stopped after %lu s", (unsigned long)ses.id, (unsigned long)ses.durS);
  if (!plotMode) Serial.println(lastSessionJson);   // longer than logf's buffer
}

void toggleSession() {
  if (recording) stopSession(); else startSession();
}

// Once a second: classify intensity, update cadence and sprints, publish.
void tick(uint32_t now) {
  float enmo = enmoN ? enmoAcc / enmoN : 0;
  enmoAcc = 0;
  enmoN = 0;
  uint16_t cad = cadence(now);
  bool active = enmo >= REST_ENMO_G || strikeThisTick;
  strikeThisTick = false;
  uint8_t zone = !active ? 0 : enmo >= VIGOROUS_ENMO_G ? 3 : enmo >= MODERATE_ENMO_G ? 2 : 1;

  // A steady second: re-measure what 1 g reads (the pod may have changed orientation)
  if (stillN > 50) {
    float mean = stillSum / stillN;
    float sd = sqrtf(fmaxf(stillSq / stillN - mean * mean, 0.0f));
    if (sd < STILL_SD_G && mean > 0.8f && mean < 1.3f) {
      gBase = mean;
      if (!gBaseLogged) {
        gBaseLogged = true;
        logf("[MPU] At rest it reads %.3f g; scaling that to 1 g", gBase);
      }
      if (fabsf(gBase - gBaseSaved) > 0.01f) {   // keep it across reboots; rare flash writes
        gBaseSaved = gBase;
        prefs.putFloat("gbase", gBase);
      }
    }
  }
  stillSum = 0;
  stillSq = 0;
  stillN = 0;

  if (recording) {
    if (active) { ses.activeS++; ses.enmoSum += enmo; } else ses.restS++;
    ses.durS = ses.activeS + ses.restS;
    if (zone == 1) ses.lightS++;
    else if (zone == 2) ses.moderateS++;
    else if (zone == 3) ses.vigorousS++;
    if (cad > 0) ses.gaitS++;
    if (cad > ses.maxCadence) ses.maxCadence = cad;
    updateSprint(cad, enmo);
  }

  if (bleConnected != bleWasConnected) {
    bleWasConnected = bleConnected;
    logf("%s", bleConnected ? "[BLE] App connected" : "[BLE] App disconnected");
  }
  publishLive(zone, enmo, cad);
}

// ======================= BLUETOOTH LE ============================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) { bleConnected = true; }
  void onDisconnect(BLEServer* server) {
    bleConnected = false;
    server->startAdvertising();   // let the app reconnect
  }
};

class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String cmd = String(c->getValue().c_str());
    cmd.trim();
    cmd.toLowerCase();
    if (cmd == "start") pendingCmd = CMD_START;
    else if (cmd == "stop") pendingCmd = CMD_STOP;
  }
};

void bleInit() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);   // the app should request a large MTU so JSON fits in one notification
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService* svc = server->createService(SERVICE_UUID);

  liveChar = svc->createCharacteristic(LIVE_UUID,
               BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  liveChar->addDescriptor(new BLE2902());
  sessionChar = svc->createCharacteristic(SESSION_UUID, BLECharacteristic::PROPERTY_READ);
  BLECharacteristic* control = svc->createCharacteristic(CONTROL_UUID,
                                 BLECharacteristic::PROPERTY_WRITE);
  control->setCallbacks(new ControlCallbacks());
  setChar(sessionChar, lastSessionJson.c_str(), lastSessionJson.length());
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising as \"" DEVICE_NAME "\"");
}

// ======================= CONTROLS ================================
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's') {
      toggleSession();
    } else if (c == 'l') {
      liveMode = !liveMode;
      Serial.printf("[LIVE] %s\n", liveMode ? "on" : "off");
    } else if (c == 'p') {
      plotMode = !plotMode;
      if (!plotMode) logf("[PLOT] off");
    } else if (c == 'r') {
      Serial.println(lastSessionJson);
    }
  }
}

void handleButton(uint32_t now) {
  bool down = digitalRead(BTN_PIN) == LOW;
  if (down && !btnWasDown) btnDownSince = now;
  if (!down && btnWasDown && now - btnDownSince >= 30) toggleSession();   // 30 ms debounce
  btnWasDown = down;
}

void updateLed(uint32_t now) {
  bool on = recording ? (now % 1000) < 500    // 1 Hz blink while recording
                      : (now % 2000) < 60;    // heartbeat blip when idle
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// ======================= SETUP ===================================
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  Serial.println("\n=== SockPod ===");
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);
  scanI2C();

  while (!mpuInit()) {
    Serial.println("[MPU] MPU-6050 not found. Check VCC->3V3, GND, SDA->21, SCL->22.");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(1000);
  }
  uint8_t whoAmI = 0, pwr2 = 0;
  i2cRead(MPU_ADDR, 0x75, &whoAmI, 1);   // 0x68 on a genuine MPU-6050
  i2cRead(MPU_ADDR, 0x6C, &pwr2, 1);     // non-zero = some axes in standby
  Serial.printf("[MPU] MPU-6050 ready (WHO_AM_I 0x%02X, PWR_MGMT_2 0x%02X)\n", whoAmI, pwr2);

  if (!bmpInit()) {
    Serial.println("[BMP] BMP280 not found at 0x76/0x77 - elevation off. "
                   "Check VCC->3V3, GND, SDA->21, SCL->22.");
  }

  prefs.begin("sockpod", false);
  sessionCounter  = prefs.getUInt("id", 0);
  lastSessionJson = prefs.getString("last", "{\"id\":0}");
  gBaseSaved = prefs.getFloat("gbase", 0);
  if (gBaseSaved > 0.8f && gBaseSaved < 1.3f) {
    gBase = gBaseSaved;
    Serial.printf("[MPU] Using saved calibration: %.3f g at rest\n", gBase);
  }

  bleInit();
  Serial.println("Commands: s = start/stop session, l = live stats, p = plotter, r = last session");
  Serial.println("BOOT button: start/stop session");
  Serial.println("[STATE] IDLE");
  lastTick = millis();
}

// ======================= MAIN LOOP ===============================
void loop() {
  handleSerial();
  if (pendingCmd != CMD_NONE) {
    uint8_t cmd = pendingCmd;
    pendingCmd = CMD_NONE;
    if (cmd == CMD_START) startSession(); else stopSession();
  }

  uint32_t now = millis();
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;

  float accG, gyroDps;
  if (!readMPU(accG, gyroDps)) {
    // Printed even in plot mode: a silent sensor would otherwise look like an empty graph
    Serial.println("[MPU] Read failed - check the MPU-6050 wiring");
    if (++mpuFailures % 5 == 0) {        // restart the I2C bus and the sensor
      Wire.end();
      Wire.begin(SDA_PIN, SCL_PIN);
      Wire.setClock(I2C_HZ);
      if (mpuInit()) Serial.println("[MPU] Bus restarted");
    }
    delay(100);
    return;
  }
  if (mpuFailures) {
    Serial.printf("[MPU] Reading again after %u failed reads\n", mpuFailures);
    mpuFailures = 0;
  }

  stillSum += accG;
  stillSq += accG * accG;
  stillN++;
  float a = accG / gBase;   // calibrated, so the pod at rest reads 1 g

  processJump(a, now);   // before steps, so a jump landing isn't counted as a step
  processStep(a, now);
  enmoAcc += a > 1.0f ? a - 1.0f : 0.0f;
  enmoN++;

  // Serial Plotter output at 50 Hz, with the thresholds drawn as lines
  if (plotMode && (plotCounter++ % 2 == 0)) {
    Serial.printf("accel_g:%.3f,rotation_x100dps:%.3f,alt_m:%.2f,step_limit:%.2f,air_limit:%.2f\n",
                  a, gyroDps / 100.0, altM, STEP_G, AIR_G);
  }

  if (now - lastBaro >= BARO_MS) { lastBaro = now; baroUpdate(); }

  if (now - lastTick >= TICK_MS) {
    lastTick = (now - lastTick > 3 * TICK_MS) ? now : lastTick + TICK_MS;
    tick(now);
  }

  handleButton(now);
  updateLed(now);
}
