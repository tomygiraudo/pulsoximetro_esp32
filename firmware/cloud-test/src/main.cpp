// Cloud-client test: a simulated PulsOx measurement (no sensor, no TFT) written to
// Firebase Realtime Database over HTTPS, exactly what tools/cloud/fake_device.py does
// from a PC. Purpose: validate the whole cloud contract (PROTOCOL.md, "Transporte en la
// nube") on the real ESP32-C3 before wiring it into firmware/pulsox.
//
// The ESP32 is only a client of the database: it writes telemetry and reads nothing.
// Serial commands (115200, see printHelp) drive manual tests: cut, token refresh,
// an invalid write, stats.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <stdarg.h>

#include "cloud.h"
#include "debug_log.h"
#include "fake_vitals.h"

// ---- Configuration (override any of these with -D in build_flags) -------------
#ifndef DB_HOST
#define DB_HOST "pulsoximetro-esp-default-rtdb.firebaseio.com"  // databaseURL without scheme
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "pulsox-4ba0e4ea46d7"
#endif
#ifndef FW_VERSION
#define FW_VERSION "cloud-test-0.1.0"
#endif
#ifndef MEASUREMENT_S
#define MEASUREMENT_S 60  // seconds per measurement
#endif
#ifndef CYCLES
#define CYCLES 1  // measurements started by themselves at boot (0 = wait for the `m` command)
#endif
#ifndef GAP_S
#define GAP_S 10  // rest between cycles (the real device would deep-sleep)
#endif
#ifndef FS_HZ
#define FS_HZ 50  // PPG sampling rate reported and simulated
#endif
#ifndef READING_EVERY_S
#define READING_EVERY_S 5  // history reading while the value is valid
#endif
#ifndef FAKE_BATTERY_PCT
#define FAKE_BATTERY_PCT 78  // -1 omits battery_pct (what the real firmware does without an ADC)
#endif
#ifndef SCENARIO_MIXED
#define SCENARIO_MIXED 0  // 1: random low-SpO2 / abnormal-BPM episodes
#endif
#ifndef CRASH_TEST
#define CRASH_TEST 0  // 1: do not close the measurement (simulates a power cut)
#endif
#ifndef FORCE_REFRESH_AFTER_S
#define FORCE_REFRESH_AFTER_S 0  // >0: renew the token this many seconds after each login (test)
#endif
#ifndef NTP_SYNC
#define NTP_SYNC 0  // 1: set the clock before TLS (plan B if the handshake fails on certificate dates)
#endif
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif
#ifndef SERIAL_WAIT_MS
#define SERIAL_WAIT_MS 15000  // max wait for the monitor at boot (on battery there's no host: times out)
#endif
#ifndef SERIAL_GRACE_MS
#define SERIAL_GRACE_MS 1500  // extra time once it's attached: after a reset USB re-enumerates
#endif

static const uint32_t TICK_MS = 1000;
static const size_t LIVE_BUF_SIZE = 1024;  // `live` is ~0.6 KB; the rules allow up to 2000 chars of ppg
static_assert(FS_HZ >= 1 && FS_HZ <= 100, "FS_HZ: the live JSON must fit in LIVE_BUF_SIZE");

// ---- State ----------------------------------------------------------------------
enum class State { Idle, Gap, Measuring };

static CloudDb db(DB_HOST, DEVICE_ID);
static FakeVitals vitals(SCENARIO_MIXED, FS_HZ, 2);
static State state = State::Idle;
static bool cloudReady = false;
static int cyclesLeft = CYCLES;
static uint32_t gapEndMs = 0;

// Current measurement
static String sid;
static uint32_t startedMs = 0, nextTickMs = 0, nextReadingMs = 0;
static uint32_t slot = 1;  // nominal second being run, 1..MEASUREMENT_S
static uint32_t seq = 0;   // ticks actually built (what the web sees as a new tick)
static uint32_t tickOk = 0, tickFail = 0, tickNoWifi = 0, tickSkipped = 0;
static uint32_t readingsOk = 0, readingsFail = 0;
static uint32_t putLatSum = 0, putLatMax = 0;

static char liveBuf[LIVE_BUF_SIZE];  // static: keeps the loop task's stack free for the TLS handshake

static inline bool is2xx(int code) { return code >= 200 && code < 300; }

// ---- Helpers --------------------------------------------------------------------
static void waitForMonitor() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < SERIAL_WAIT_MS) delay(10);  // USB-CDC: wait for the monitor
  if (Serial) delay(SERIAL_GRACE_MS);
}

// snprintf that appends at buf[len]; false (and the buffer is not to be used) if it would overflow.
static bool appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - len) return false;
  len += (size_t)n;
  return true;
}

#if NTP_SYNC
static void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 10000) delay(200);
  DBG("NTP", "%s", time(nullptr) >= 1700000000 ? "hora sincronizada" : "sin hora, sigo igual");
}
#endif

static void printHelp() {
  DBG("CMD", "comandos: m=nueva medicion  e=terminar  c=corte (sin cerrar)  r=renovar token  "
             "v=escritura invalida  s=estadisticas  ?=ayuda");
}

static void printStats(const char* tag) {
  const CloudStats& st = db.stats();
  uint32_t avg = st.latCount ? st.latSumMs / st.latCount : 0;
  DBG(tag, "DB: ok %lu fallo %lu | latencia media %lu max %lu ms | tlsConnects %lu | logins %lu refresh %lu",
      (unsigned long)st.ok, (unsigned long)st.fail, (unsigned long)avg, (unsigned long)st.latMaxMs,
      (unsigned long)st.tlsConnects, (unsigned long)st.logins, (unsigned long)st.refreshes);
  DBG(tag, "heap libre %u minimo %u | WiFi %s RSSI %d dBm | uptime %lu s", (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getMinFreeHeap(), WiFi.status() == WL_CONNECTED ? "ok" : "CAIDO",
      WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0, (unsigned long)(millis() / 1000));
}

// ---- Boot: WiFi, login, info, clear a stale `live` -----------------------------
static bool bootCloud() {
  if (WiFi.status() != WL_CONNECTED && !wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) return false;
#if NTP_SYNC
  syncTime();
#endif
  if (!db.login()) {
    DBG("BOOT", "login fallo: %s", db.lastError().c_str());
    return false;
  }
  // Compare it with the uid in tools/cloud/database.rules.json (the only one allowed to write).
  DBG("BOOT", "uid %s", db.uid().c_str());

  char info[256];
  size_t n = 0;
  bool built = appendf(info, sizeof(info), n,
                       "{\"device_id\":\"%s\",\"fw_version\":\"%s\",\"sensor\":\"MAX30102 (simulado)\"", DEVICE_ID,
                       FW_VERSION);
  if (built && FAKE_BATTERY_PCT >= 0) built = appendf(info, sizeof(info), n, ",\"battery_pct\":%d", FAKE_BATTERY_PCT);
  if (built) built = appendf(info, sizeof(info), n, ",\"updated\":{\".sv\":\"timestamp\"}}");
  if (!built) {
    DBG("BOOT", "info no entra en el buffer");
    return false;
  }
  int code = db.put("info", info);
  DBG("BOOT", "PUT info -> %d %s", code, is2xx(code) ? "" : db.lastError().c_str());
  if (!is2xx(code)) return false;
  code = db.del("live");  // clears the `live` a power cut mid-measurement leaves behind
  DBG("BOOT", "DELETE live -> %d %s", code, is2xx(code) ? "" : db.lastError().c_str());
  if (!is2xx(code)) return false;
  cloudReady = true;
  return true;
}

static bool ensureCloud() { return cloudReady || bootCloud(); }

// ---- Measurement ----------------------------------------------------------------
static bool startMeasurement() {
  if (!ensureCloud()) {
    DBG("MEAS", "sin nube (WiFi/login): no se puede iniciar");
    return false;
  }
  vitals = FakeVitals(SCENARIO_MIXED, FS_HZ, 2);
  String newSid;
  int code = db.post("sessions", "{\"startedAt\":{\".sv\":\"timestamp\"}}", &newSid);
  if (!is2xx(code) || newSid.length() == 0) {
    DBG("MEAS", "no se pudo abrir la sesion: %d %s", code, db.lastError().c_str());
    return false;
  }
  sid = newSid;
  seq = 0;
  slot = 1;
  tickOk = tickFail = tickNoWifi = tickSkipped = 0;
  readingsOk = readingsFail = 0;
  putLatSum = putLatMax = 0;
  startedMs = millis();
  nextTickMs = startedMs + TICK_MS;
  nextReadingMs = startedMs + READING_EVERY_S * 1000UL;
  state = State::Measuring;
  DBG("MEAS", "medicion iniciada: sesion %s (%d s, %d Hz)", sid.c_str(), MEASUREMENT_S, FS_HZ);
  return true;
}

// The `live` node (PUT, whole replacement, every second). The database stores no nulls:
// a field without a value is simply not sent.
static bool buildLive(uint32_t elapsedMs) {
  size_t n = 0;
  const bool finger = vitals.finger();
  const bool valid = vitals.valid();
  bool ok = appendf(liveBuf, sizeof(liveBuf), n,
                    "{\"session_id\":\"%s\",\"seq\":%lu,\"ts\":{\".sv\":\"timestamp\"},\"elapsed_ms\":%lu,"
                    "\"finger_detected\":%s,\"spo2_valid\":%s,\"bpm_valid\":%s,\"signal_quality\":%.2f,\"fs\":%d",
                    sid.c_str(), (unsigned long)seq, (unsigned long)elapsedMs, finger ? "true" : "false",
                    valid ? "true" : "false", valid ? "true" : "false", vitals.quality(), FS_HZ);
  if (ok && FAKE_BATTERY_PCT >= 0) ok = appendf(liveBuf, sizeof(liveBuf), n, ",\"battery_pct\":%d", FAKE_BATTERY_PCT);
  if (ok && finger) {
    ok = appendf(liveBuf, sizeof(liveBuf), n, ",\"spo2\":%.1f,\"bpm\":%d,\"ppg\":\"", vitals.spo2(),
                 (int)lroundf(vitals.bpm()));
    int16_t samples[FS_HZ];
    vitals.ppgBatch(samples);
    for (int i = 0; ok && i < FS_HZ; i++) ok = appendf(liveBuf, sizeof(liveBuf), n, i ? ",%d" : "%d", (int)samples[i]);
    if (ok) ok = appendf(liveBuf, sizeof(liveBuf), n, "\"");
  }
  if (ok) ok = appendf(liveBuf, sizeof(liveBuf), n, "}");
  return ok;
}

static void postReading(uint32_t now) {
  if (!vitals.valid() || (int32_t)(now - nextReadingMs) < 0) return;
  nextReadingMs += READING_EVERY_S * 1000UL;
  if ((int32_t)(now - nextReadingMs) >= 0) nextReadingMs = now + READING_EVERY_S * 1000UL;  // far behind: resync
  char body[128], path[96];
  size_t n = 0;
  if (!appendf(body, sizeof(body), n, "{\"ts\":{\".sv\":\"timestamp\"},\"spo2\":%.1f,\"bpm\":%d,\"quality\":%.2f}",
               vitals.spo2(), (int)lroundf(vitals.bpm()), vitals.quality()))
    return;
  snprintf(path, sizeof(path), "sessions/%s/readings", sid.c_str());
  int code = db.post(path, body);
  if (is2xx(code)) {
    readingsOk++;
  } else {
    readingsFail++;
    DBG("LIVE", "POST reading -> %d %s", code, db.lastError().c_str());
  }
}

static void doTick() {
  seq++;
  vitals.tick();
  const uint32_t now = millis();
  if (!buildLive(now - startedMs)) {
    tickFail++;
    DBG("LIVE", "seq %lu: el JSON no entra en el buffer", (unsigned long)seq);
    return;
  }
  int code = db.put("live", liveBuf);
  if (is2xx(code)) {
    tickOk++;
    uint32_t lat = db.lastLatencyMs();
    putLatSum += lat;
    if (lat > putLatMax) putLatMax = lat;
    postReading(now);
  } else if (code == CLOUD_ERR_NOWIFI) {
    tickNoWifi++;  // skipped without trying; the next tick comes in 1 s, nothing is queued
  } else {
    tickFail++;
    DBG("LIVE", "seq %lu: PUT live fallo (%d %s)", (unsigned long)seq, code, db.lastError().c_str());
  }
}

static void printMeasurementSummary(const char* prefix) {
  uint32_t done = tickOk + tickFail + tickNoWifi + tickSkipped;
  uint32_t avg = tickOk ? putLatSum / tickOk : 0;
  const CloudStats& st = db.stats();
  char vit[40];
  if (vitals.finger()) snprintf(vit, sizeof(vit), "SpO2 %.1f BPM %.0f", vitals.spo2(), vitals.bpm());
  else snprintf(vit, sizeof(vit), "sin dedo");
  DBG("LIVE", "%sseq %lu | %s | ticks OK %lu/%lu (fallo %lu, sin WiFi %lu, salteados %lu) | PUT live media %lu max %lu ms",
      prefix, (unsigned long)seq, vit, (unsigned long)tickOk, (unsigned long)done, (unsigned long)tickFail,
      (unsigned long)tickNoWifi, (unsigned long)tickSkipped, (unsigned long)avg, (unsigned long)putLatMax);
  DBG("LIVE", "%slecturas OK %lu fallo %lu | tlsConnects %lu | heap libre %u minimo %u", prefix,
      (unsigned long)readingsOk, (unsigned long)readingsFail, (unsigned long)st.tlsConnects,
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

// Closing order from PROTOCOL.md (8, 9, 10): endedAt first, `live` last (deleting `live` is
// what tells the viewers the measurement is over). Each step is retried: it is not droppable.
static bool closeStep(int which) {
  char path[64], body[96];
  for (int attempt = 1; attempt <= 3; attempt++) {
    int code;
    if (which == 0) {
      snprintf(path, sizeof(path), "sessions/%s", sid.c_str());
      code = db.patch(path, "{\"endedAt\":{\".sv\":\"timestamp\"}}");
    } else if (which == 1) {
      if (FAKE_BATTERY_PCT >= 0)
        snprintf(body, sizeof(body), "{\"battery_pct\":%d,\"updated\":{\".sv\":\"timestamp\"}}", FAKE_BATTERY_PCT);
      else
        snprintf(body, sizeof(body), "{\"updated\":{\".sv\":\"timestamp\"}}");
      code = db.patch("info", body);
    } else {
      code = db.del("live");
    }
    static const char* const names[] = {"PATCH sessions/<sid> endedAt", "PATCH info", "DELETE live"};
    DBG("MEAS", "cierre: %s -> %d %s", names[which], code, is2xx(code) ? "" : db.lastError().c_str());
    if (is2xx(code)) return true;
    delay(500);
  }
  return false;
}

static void afterMeasurement() {
  if (cyclesLeft > 0) cyclesLeft--;
  if (cyclesLeft > 0) {
    state = State::Gap;
    gapEndMs = millis() + GAP_S * 1000UL;
    DBG("MEAS", "reposo %d s (el ESP32 real dormiria en deep sleep); faltan %d ciclo(s)", GAP_S, cyclesLeft);
  } else {
    state = State::Idle;
    DBG("MEAS", "ciclos completos: despierto con USB, esperando comandos");
  }
}

static void finishMeasurement(const char* why) {
  printMeasurementSummary("fin: ");
  if (CRASH_TEST) {
    DBG("MEAS", "CRASH_TEST (%s): corte simulado, la medicion queda abierta", why);
    state = State::Idle;
    cyclesLeft = 0;
    return;
  }
  bool ok = closeStep(0);
  ok = closeStep(1) && ok;
  ok = closeStep(2) && ok;
  DBG("MEAS", "medicion finalizada (%s)%s", why, ok ? "" : " CON ERRORES: revisa el cierre");
  afterMeasurement();
}

static void runMeasurement(uint32_t now) {
  if ((int32_t)(now - nextTickMs) < 0) return;
  // A request that took longer than a tick: skip the seconds that were missed instead of
  // firing a burst of catch-up writes.
  const uint32_t late = now - nextTickMs;
  const uint32_t behind = late / TICK_MS;
  if (behind > 0) {
    tickSkipped += behind;
    slot += behind;
    nextTickMs += behind * TICK_MS;
    DBG("LIVE", "atrasado %lu ms: salteo %lu tick(s)", (unsigned long)late, (unsigned long)behind);
  }
  if (slot <= MEASUREMENT_S) {
    doTick();
    if (slot % 10 == 0) printMeasurementSummary("");
  }
  slot++;
  nextTickMs += TICK_MS;
  if (slot > MEASUREMENT_S) finishMeasurement("tiempo cumplido");
}

// ---- Serial commands (manual tests) ---------------------------------------------
static void invalidWrite() {
  if (!ensureCloud()) {
    DBG("CMD", "sin nube");
    return;
  }
  // spo2 = 500 violates the rule (0..100): the REST API answers 401 like for an expired token.
  const char* body = "{\"session_id\":\"invalid-test\",\"seq\":0,\"ts\":{\".sv\":\"timestamp\"},\"spo2\":500}";
  int code = db.put("live", body);
  DBG("CMD", "escritura invalida (spo2=500) -> %d %s", code, db.lastError().c_str());
  DBG("CMD", "esperado: 401, con un login nuevo y UN reintento (sin bucle); el proximo tick debe dar OK");
}

static void handleCommand(char c) {
  switch (c) {
    case 'm':
      if (state == State::Measuring) DBG("CMD", "ya hay una medicion en curso (e=terminar)");
      else startMeasurement();
      break;
    case 'e':
      if (state == State::Measuring) finishMeasurement("comando e");
      else DBG("CMD", "no hay medicion en curso");
      break;
    case 'c':
      if (state == State::Measuring) {
        DBG("CMD", "corte: dejo de escribir SIN cerrar (la sesion y `live` quedan abiertos)");
        printMeasurementSummary("corte: ");
        state = State::Idle;
        cyclesLeft = 0;
      } else {
        DBG("CMD", "no hay medicion en curso");
      }
      break;
    case 'r':
      db.expireToken();
      DBG("CMD", "token marcado como vencido: se renueva en la proxima escritura");
      break;
    case 'v': invalidWrite(); break;
    case 's': printStats("STAT"); break;
    case '?':
    case 'h': printHelp(); break;
    default: DBG("CMD", "comando desconocido '%c'", c); printHelp();
  }
}

static void pollSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') continue;
    handleCommand((char)tolower(c));
  }
}

// ---- Arduino entry points ---------------------------------------------------------
void setup() {
  waitForMonitor();
  DBG("BOOT", "cloud-test %s | dispositivo %s | base %s", FW_VERSION, DEVICE_ID, DB_HOST);
  DBG("BOOT", "medicion %d s x %d ciclo(s), reposo %d s, %d Hz, bateria %d%%, escenario %s%s", MEASUREMENT_S, CYCLES,
      GAP_S, FS_HZ, FAKE_BATTERY_PCT, SCENARIO_MIXED ? "mixed" : "normal", CRASH_TEST ? ", CRASH_TEST" : "");
  DBG("BOOT", "reset reason %d | heap libre %u", (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());

  db.setRefreshAfterS(FORCE_REFRESH_AFTER_S);
  for (int attempt = 1; attempt <= 3 && !cloudReady; attempt++) {
    if (attempt > 1) {
      DBG("BOOT", "reintento %d/3 en 6 s", attempt);
      delay(6000);  // longer than the 5 s login backoff
    }
    bootCloud();
  }
  printHelp();
  if (!cloudReady) {
    DBG("BOOT", "sin nube: revisa include/secrets.h y la red; el comando `m` reintenta");
    return;
  }
  if (CYCLES > 0) startMeasurement();
}

void loop() {
  pollSerial();
  const uint32_t now = millis();
  if (state == State::Measuring) {
    runMeasurement(now);
  } else if (state == State::Gap && (int32_t)(now - gapEndMs) >= 0) {
    if (!startMeasurement()) {
      state = State::Idle;
      cyclesLeft = 0;
    }
  }
  delay(2);
}
