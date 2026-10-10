#include "cloud_link.h"
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <math.h>
#include <stdarg.h>
#include "cloud.h"
#include "debug_log.h"

namespace {

enum class CmdType : uint8_t { CONNECT, START, FINISH, ABORT, DISCONNECT };
struct Cmd {
  CmdType type;
  float spo2, bpm, quality;  // FINISH only
};

CloudDb db(DB_HOST, DEVICE_ID);

QueueHandle_t cmdQueue = nullptr;   // commands, in order
QueueHandle_t tickQueue = nullptr;  // depth 1, overwritten: only the latest tick matters

std::atomic<uint8_t> state{(uint8_t)LinkState::IDLE};
std::atomic<uint8_t> pending{0};    // commands posted and not finished yet
std::atomic<bool> sent{false};

// Task-side state of the session.
String sid;
uint32_t seq = 0;
uint32_t nextReadingMs = 0;
CloudLinkStats stats = {};

char liveBuf[1024];  // static: keeps the task's stack free for the TLS handshake

inline bool is2xx(int code) { return code >= 200 && code < 300; }
inline void setState(LinkState s) { state = (uint8_t)s; }

// snprintf that appends at buf[len]; false if it would not fit.
bool appendf(char *buf, size_t cap, size_t &len, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - len) return false;
  len += (size_t)n;
  return true;
}

// ---- Connect: WiFi, login, info, clear a stale `live` ----------------------------------
bool bootCloud() {
  if (!wifiConnect(WIFI_CONNECT_TIMEOUT_MS)) return false;
  if (!db.login()) {
    DBG("CLOU", "login fallo: %s", db.lastError().c_str());
    return false;
  }
  // The same uid has to be the one allowed to write in tools/cloud/database.rules.json.
  DBG("CLOU", "uid %s", db.uid().c_str());

  int code = db.put("info", "{\"device_id\":\"" DEVICE_ID "\",\"fw_version\":\"" FW_VERSION
                            "\",\"sensor\":\"MAX30102\",\"updated\":{\".sv\":\"timestamp\"}}");
  DBG("CLOU", "PUT info -> %d %s", code, is2xx(code) ? "" : db.lastError().c_str());
  if (!is2xx(code)) return false;
  code = db.del("live");  // clears the `live` a power cut in the middle of a measurement leaves behind
  DBG("CLOU", "DELETE live -> %d %s", code, is2xx(code) ? "" : db.lastError().c_str());
  return is2xx(code);
}

void doConnect() {
  setState(LinkState::CONNECTING);
  uint32_t t0 = millis();
  bool ok = bootCloud();
  DBG("CLOU", "%s en %lu ms", ok ? "nube lista" : "SIN NUBE (se mide igual, sin enviar)",
      (unsigned long)(millis() - t0));
  setState(ok ? LinkState::READY : LinkState::OFFLINE);
}

// ---- Session -------------------------------------------------------------------------------
void doStart() {
  if (state != (uint8_t)LinkState::READY) {
    DBG("CLOU", "StartSession ignorado: estado %u", (unsigned)state.load());
    return;
  }
  stats = {};
  sent = false;
  seq = 0;
  nextReadingMs = READING_EVERY_S * 1000UL;
  xQueueReset(tickQueue);  // nothing from before this session
  String newSid;
  int code = db.post("sessions", "{\"startedAt\":{\".sv\":\"timestamp\"}}", &newSid);
  if (!is2xx(code) || newSid.length() == 0) {
    DBG("CLOU", "no se pudo abrir la sesion: %d %s", code, db.lastError().c_str());
    setState(LinkState::OFFLINE);
    return;
  }
  sid = newSid;
  DBG("CLOU", "sesion abierta: %s", sid.c_str());
  setState(LinkState::SESSION);
}

// `live`, a whole replacement every second. The database stores no nulls: a field without a
// value is not sent. Same fields as firmware/cloud-test buildLive(); no battery_pct (no ADC).
bool buildLive(const LiveTick &t) {
  size_t n = 0;
  bool ok = appendf(liveBuf, sizeof(liveBuf), n,
                    "{\"session_id\":\"%s\",\"seq\":%lu,\"ts\":{\".sv\":\"timestamp\"},\"elapsed_ms\":%lu,"
                    "\"finger_detected\":%s,\"spo2_valid\":%s,\"bpm_valid\":%s,\"signal_quality\":%.2f,\"fs\":%d",
                    sid.c_str(), (unsigned long)seq, (unsigned long)t.elapsedMs, t.finger ? "true" : "false",
                    t.spo2Valid ? "true" : "false", t.bpmValid ? "true" : "false", (double)t.quality, CLOUD_PPG_FS);
  if (ok && t.finger && t.spo2Valid) ok = appendf(liveBuf, sizeof(liveBuf), n, ",\"spo2\":%.1f", (double)t.spo2);
  if (ok && t.finger && t.bpm > 0.0f) ok = appendf(liveBuf, sizeof(liveBuf), n, ",\"bpm\":%d", (int)lroundf(t.bpm));
  if (ok && t.finger && t.ppgCount > 0) {
    ok = appendf(liveBuf, sizeof(liveBuf), n, ",\"ppg\":\"");
    for (int i = 0; ok && i < t.ppgCount; i++) ok = appendf(liveBuf, sizeof(liveBuf), n, i ? ",%d" : "%d", (int)t.ppg[i]);
    if (ok) ok = appendf(liveBuf, sizeof(liveBuf), n, "\"");
  }
  if (ok) ok = appendf(liveBuf, sizeof(liveBuf), n, "}");
  return ok;
}

bool postReading(float spo2, float bpm, float quality) {
  char body[128], path[96];
  size_t n = 0;
  if (!appendf(body, sizeof(body), n, "{\"ts\":{\".sv\":\"timestamp\"},\"spo2\":%.1f,\"bpm\":%d,\"quality\":%.2f}",
               (double)spo2, (int)lroundf(bpm), (double)quality))
    return false;
  snprintf(path, sizeof(path), "sessions/%s/readings", sid.c_str());
  int code = db.post(path, body);
  if (is2xx(code)) {
    stats.readingsOk++;
    return true;
  }
  stats.readingsFail++;
  DBG("CLOU", "POST reading -> %d %s", code, db.lastError().c_str());
  return false;
}

void sendTick(const LiveTick &t) {
  seq++;
  if (!buildLive(t)) {
    stats.ticksFail++;
    DBG("CLOU", "seq %lu: el JSON no entra en el buffer", (unsigned long)seq);
    return;
  }
  int code = db.put("live", liveBuf);
  if (is2xx(code)) {
    stats.ticksOk++;
    if (db.lastLatencyMs() > stats.putMaxMs) stats.putMaxMs = db.lastLatencyMs();
    if (t.spo2Valid && t.bpmValid && (int32_t)(t.elapsedMs - nextReadingMs) >= 0) {
      nextReadingMs += READING_EVERY_S * 1000UL;
      if ((int32_t)(t.elapsedMs - nextReadingMs) >= 0) nextReadingMs = t.elapsedMs + READING_EVERY_S * 1000UL;  // far behind
      postReading(t.spo2, t.bpm, t.quality);
    }
  } else if (code == CLOUD_ERR_NOWIFI) {
    stats.ticksFail++;  // skipped without trying; the next tick comes in 1 s, nothing is queued
  } else {
    stats.ticksFail++;
    DBG("CLOU", "seq %lu: PUT live fallo (%d %s)", (unsigned long)seq, code, db.lastError().c_str());
  }
}

// One step of the closing / aborting, retried: it is not droppable.
bool closeStep(const char *what, int (*call)(const char *arg), const char *arg) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    int code = call(arg);
    DBG("CLOU", "%s -> %d %s", what, code, is2xx(code) ? "" : db.lastError().c_str());
    if (is2xx(code)) return true;
    delay(500);
  }
  return false;
}

int patchEndedAt(const char *path) { return db.patch(path, "{\"endedAt\":{\".sv\":\"timestamp\"}}"); }
int patchInfo(const char *) { return db.patch("info", "{\"updated\":{\".sv\":\"timestamp\"}}"); }
int deleteNode(const char *path) { return db.del(path); }

void logSessionSummary(const char *how) {
  DBG("CLOU", "%s: ticks OK %lu fallo %lu descartados %lu | lecturas OK %lu fallo %lu | PUT live max %lu ms | heap minimo %u",
      how, (unsigned long)stats.ticksOk, (unsigned long)stats.ticksFail, (unsigned long)stats.ticksDropped,
      (unsigned long)stats.readingsOk, (unsigned long)stats.readingsFail, (unsigned long)stats.putMaxMs,
      (unsigned)ESP.getMinFreeHeap());
}

// Closing order from PROTOCOL.md (8, 9, 10): endedAt first, `live` last (deleting `live` is
// what tells the viewers the measurement is over).
void doFinish(const Cmd &c) {
  if (state != (uint8_t)LinkState::SESSION) {
    DBG("CLOU", "Finish ignorado: estado %u", (unsigned)state.load());
    return;
  }
  LiveTick last;
  if (xQueueReceive(tickQueue, &last, 0) == pdTRUE) sendTick(last);  // the tick posted just before Finish
  setState(LinkState::CLOSING);
  logSessionSummary("fin");
  if (c.spo2 > 0.0f && c.bpm > 0.0f) postReading(c.spo2, c.bpm, c.quality);

  char path[64];
  snprintf(path, sizeof(path), "sessions/%s", sid.c_str());
  bool ok = closeStep("PATCH sessions/<sid> endedAt", patchEndedAt, path);
  sent = ok;
  closeStep("PATCH info", patchInfo, nullptr);
  ok = closeStep("DELETE live", deleteNode, "live") && ok;
  DBG("CLOU", "medicion cerrada%s", ok ? "" : " CON ERRORES");
  setState(LinkState::READY);
}

// The measurement was invalid: nothing of it stays in the history.
void doAbort() {
  if (state != (uint8_t)LinkState::SESSION) {
    DBG("CLOU", "Abort ignorado: estado %u", (unsigned)state.load());
    return;
  }
  setState(LinkState::CLOSING);
  logSessionSummary("aborto");
  char path[64];
  snprintf(path, sizeof(path), "sessions/%s", sid.c_str());
  bool ok = closeStep("DELETE sessions/<sid>", deleteNode, path);
  ok = closeStep("DELETE live", deleteNode, "live") && ok;
  sent = false;
  DBG("CLOU", "sesion borrada%s", ok ? "" : " CON ERRORES");
  setState(LinkState::READY);
}

void doDisconnect() {
  db.disconnect();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  setState(LinkState::IDLE);
  DBG("CLOU", "WiFi apagado");
}

void handle(const Cmd &c) {
  switch (c.type) {
    case CmdType::CONNECT: doConnect(); break;
    case CmdType::START: doStart(); break;
    case CmdType::FINISH: doFinish(c); break;
    case CmdType::ABORT: doAbort(); break;
    case CmdType::DISCONNECT: doDisconnect(); break;
  }
}

void cloudTask(void *) {
  for (;;) {
    Cmd cmd;
    if (xQueueReceive(cmdQueue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
      handle(cmd);
      pending--;
      continue;
    }
    LiveTick tick;
    if (state == (uint8_t)LinkState::SESSION && xQueueReceive(tickQueue, &tick, 0) == pdTRUE) sendTick(tick);
  }
}

void post(const Cmd &c) {
  pending++;
  if (xQueueSend(cmdQueue, &c, 0) != pdTRUE) {
    pending--;
    DBG("CLOU", "cola de comandos llena: comando %u perdido", (unsigned)c.type);
  }
}

}  // namespace

void cloudLinkBegin() {
  if (cmdQueue) return;
  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  tickQueue = xQueueCreate(1, sizeof(LiveTick));
  xTaskCreate(cloudTask, "cloud", CLOUD_TASK_STACK, nullptr, 1, nullptr);
}

void cloudLinkConnect() { post({CmdType::CONNECT, 0, 0, 0}); }
void cloudLinkStartSession() { post({CmdType::START, 0, 0, 0}); }
void cloudLinkFinish(float spo2, float bpm, float quality) { post({CmdType::FINISH, spo2, bpm, quality}); }
void cloudLinkAbort() { post({CmdType::ABORT, 0, 0, 0}); }
void cloudLinkDisconnect() { post({CmdType::DISCONNECT, 0, 0, 0}); }

void cloudLinkPushTick(const LiveTick &tick) {
  if (!tickQueue) return;
  if (uxQueueMessagesWaiting(tickQueue) > 0) stats.ticksDropped++;  // the previous one was never sent
  xQueueOverwrite(tickQueue, &tick);
}

LinkState cloudLinkState() { return (LinkState)state.load(); }
bool cloudLinkBusy() { return pending.load() > 0; }
bool cloudLinkSent() { return sent.load(); }
CloudLinkStats cloudLinkStats() { return stats; }
