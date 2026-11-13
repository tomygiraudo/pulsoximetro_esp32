#include "cloud/cloud_link.h"
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <math.h>
#include <stdarg.h>
#include "cloud/cloud.h"
#include "debug/debug_log.h"

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
CloudLinkStats stats = {};

char liveBuf[1024];  // static: keeps the task's stack free for the TLS handshake

/*******************************************************************************
 * @brief       Dice si un código HTTP es de éxito (2xx).
 *
 * @details     true para 200 a 299.
 *
 * @param       Entrada: code - código HTTP.
 *              Salida:  retorno (bool) - true si es 2xx.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
inline bool is2xx(int code) { return code >= 200 && code < 300; }

/*******************************************************************************
 * @brief       Publica el estado de la conexión a la nube.
 *
 * @details     Escribe el atómico `state`, que cloudLinkState() lee desde otra
 *              tarea.
 *
 * @param       Entrada: s - nuevo estado.
 *              Salida:  state - estado vigente.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
inline void setState(LinkState s) { state = (uint8_t)s; }

/*******************************************************************************
 * @brief       Agrega texto con formato a un buffer, a partir de la posición
 *              len.
 *
 * @details     Es un snprintf que escribe en buf + len y avanza len; devuelve
 *              false si no entra.
 *
 * @param       Entrada: cap - capacidad de buf.
 *              Entrada: fmt, ... - formato y argumentos.
 *              Salida:  buf - texto agregado.
 *              Salida:  len - largo usado; avanza lo escrito.
 *              Salida:  retorno (bool) - true si el texto entró.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Conecta WiFi, inicia sesión y prepara la base: escribe info y
 *              limpia `live`.
 *
 * @details     Hace wifiConnect, login, PUT info y DELETE live, que borra el
 *              `live` que deja un corte de energía a mitad de una medición.
 *              Falla si cualquiera de los pasos falla.
 *
 * @param       Salida:  retorno (bool) - true si la nube quedó lista.
 *
 * @peripherals Internos: bloque WiFi (STA y TLS), Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi, Firebase Realtime Database.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Ejecuta el comando CONNECT: conecta y fija el estado resultante.
 *
 * @details     Pasa a CONNECTING, llama a bootCloud() y deja READY u OFFLINE.
 *              Con CLOUD_FORCE_OFFLINE no conecta: espera 3 s y falla, para
 *              probar el camino sin nube.
 *
 * @param       Salida:  state - READY si conectó, OFFLINE si no.
 *
 * @peripherals Internos: bloque WiFi (STA y TLS), Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi, Firebase Realtime Database.
 *******************************************************************************/
void doConnect() {
  setState(LinkState::CONNECTING);
  uint32_t t0 = millis();
#if CLOUD_FORCE_OFFLINE
  delay(3000);  // test build: no network at all, after about as long as a failed attempt takes to give up
  bool ok = false;
#else
  bool ok = bootCloud();
#endif
  DBG("CLOU", "%s en %lu ms", ok ? "nube lista" : "SIN NUBE (se mide igual, sin enviar)",
      (unsigned long)(millis() - t0));
  setState(ok ? LinkState::READY : LinkState::OFFLINE);
}

// ---- Session -------------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Ejecuta el comando START: abre una sesión de medición en la
 *              base.
 *
 * @details     Solo desde READY. Pone a cero stats, sent y seq, vacía la cola
 *              de ticks y hace POST sessions con startedAt del servidor. Guarda
 *              la clave en sid y pasa a SESSION (a OFFLINE si falla).
 *
 * @param       Entrada: state - debe ser READY.
 *              Salida:  sid, seq, sent, stats - sesión nueva y contadores en
 *                       cero.
 *              Salida:  state - SESSION, u OFFLINE si no se pudo abrir.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void doStart() {
  if (state != (uint8_t)LinkState::READY) {
    DBG("CLOU", "StartSession ignorado: estado %u", (unsigned)state.load());
    return;
  }
  stats = {};
  sent = false;
  seq = 0;
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

/*******************************************************************************
 * @brief       Arma el JSON del nodo `live` con los datos de un tick.
 *
 * @details     Es un reemplazo completo cada segundo: session_id, seq, ts del
 *              servidor, elapsed_ms, validez, calidad y fs, y spo2, bpm y la
 *              traza ppg solo si valen (la base no guarda nulls). Mismos campos
 *              que buildLive() de firmware/cloud-test.
 *
 * @param       Entrada: t - tick del segundo.
 *              Entrada: sid, seq - sesión y número de tick.
 *              Salida:  liveBuf - JSON armado.
 *              Salida:  retorno (bool) - false si no entra en el buffer.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Publica la lectura final de la sesión, el único dato del
 *              historial.
 *
 * @details     Es la mediana de los segundos estables: los anteriores vienen de
 *              un filtro que aún se asentaba. Hace POST sessions/<sid>/readings
 *              hasta 3 veces (500 ms entre intentos): sin ella la sesión queda
 *              vacía.
 *
 * @param       Entrada: spo2, bpm, quality - resultado de la medición.
 *              Entrada: sid - sesión abierta.
 *              Salida:  retorno (bool) - true si se publicó.
 *              Salida:  stats.readingsOk, stats.readingsFail - contadores de
 *                       lecturas.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
bool postReading(float spo2, float bpm, float quality) {
  char body[128], path[96];
  size_t n = 0;
  if (!appendf(body, sizeof(body), n, "{\"ts\":{\".sv\":\"timestamp\"},\"spo2\":%.1f,\"bpm\":%d,\"quality\":%.2f}",
               (double)spo2, (int)lroundf(bpm), (double)quality))
    return false;
  snprintf(path, sizeof(path), "sessions/%s/readings", sid.c_str());
  for (int attempt = 1; attempt <= 3; attempt++) {  // not droppable: without it the session is empty
    int code = db.post(path, body);
    if (is2xx(code)) {
      stats.readingsOk++;
      return true;
    }
    stats.readingsFail++;
    DBG("CLOU", "POST reading -> %d %s", code, db.lastError().c_str());
    delay(500);
  }
  return false;
}

/*******************************************************************************
 * @brief       Envía un tick: arma el JSON y lo escribe en `live` con PUT.
 *
 * @details     Incrementa seq, arma el JSON y hace PUT live. Un tick fallido no
 *              se reintenta ni se encola (el siguiente llega en 1 s); sin WiFi
 *              se salta sin intentar.
 *
 * @param       Entrada: t - tick del segundo.
 *              Salida:  seq - número de tick, incrementado.
 *              Salida:  stats - ticksOk, ticksFail y putMaxMs.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
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
  } else if (code == CLOUD_ERR_NOWIFI) {
    stats.ticksFail++;  // skipped without trying; the next tick comes in 1 s, nothing is queued
  } else {
    stats.ticksFail++;
    DBG("CLOU", "seq %lu: PUT live fallo (%d %s)", (unsigned long)seq, code, db.lastError().c_str());
  }
}

/*******************************************************************************
 * @brief       Ejecuta un paso del cierre o aborto de la sesión, con
 *              reintentos.
 *
 * @details     Llama a call(arg) hasta 3 veces (500 ms entre intentos) hasta
 *              obtener un 2xx y loguea cada intento. No se puede descartar.
 *
 * @param       Entrada: what - nombre del paso, para el log.
 *              Entrada: call - función que hace el pedido.
 *              Entrada: arg - argumento de call (la ruta).
 *              Salida:  retorno (bool) - true si algún intento dio 2xx.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
bool closeStep(const char *what, int (*call)(const char *arg), const char *arg) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    int code = call(arg);
    DBG("CLOU", "%s -> %d %s", what, code, is2xx(code) ? "" : db.lastError().c_str());
    if (is2xx(code)) return true;
    delay(500);
  }
  return false;
}

/*******************************************************************************
 * @brief       Escribe endedAt (hora del servidor) en la ruta indicada, con
 *              PATCH.
 *
 * @details     Paso de cierre de la sesión para closeStep().
 *
 * @param       Entrada: path - ruta de la sesión.
 *              Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int patchEndedAt(const char *path) { return db.patch(path, "{\"endedAt\":{\".sv\":\"timestamp\"}}"); }

/*******************************************************************************
 * @brief       Actualiza info.updated (hora del servidor) con PATCH.
 *
 * @details     Ignora el argumento: existe para tener la misma firma que los
 *              demás pasos de closeStep().
 *
 * @param       Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int patchInfo(const char *) { return db.patch("info", "{\"updated\":{\".sv\":\"timestamp\"}}"); }

/*******************************************************************************
 * @brief       Borra con DELETE la ruta indicada de la base.
 *
 * @details     Paso de cierre o aborto para closeStep().
 *
 * @param       Entrada: path - ruta a borrar.
 *              Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int deleteNode(const char *path) { return db.del(path); }

void deleteSession();

/*******************************************************************************
 * @brief       Loguea el resumen de la sesión: ticks, lecturas, PUT máximo y
 *              heap mínimo.
 *
 * @details     Una línea con los contadores de stats y el mínimo de heap libre.
 *
 * @param       Entrada: how - cómo terminó la sesión ("fin" o "aborto").
 *              Entrada: stats - contadores de la sesión.
 *
 * @peripherals Internos: Serial/USB-CDC (log). Externos: Ninguno.
 *******************************************************************************/
void logSessionSummary(const char *how) {
  DBG("CLOU", "%s: ticks OK %lu fallo %lu descartados %lu | lecturas OK %lu fallo %lu | PUT live max %lu ms | heap minimo %u",
      how, (unsigned long)stats.ticksOk, (unsigned long)stats.ticksFail, (unsigned long)stats.ticksDropped,
      (unsigned long)stats.readingsOk, (unsigned long)stats.readingsFail, (unsigned long)stats.putMaxMs,
      (unsigned)ESP.getMinFreeHeap());
}

/*******************************************************************************
 * @brief       Ejecuta el comando FINISH: cierra en la nube una medición buena.
 *
 * @details     Envía el último tick pendiente, publica la lectura final y
 *              cierra en el orden de PROTOCOL.md: endedAt, info y por último
 *              borrar `live` (avisa a los visores que terminó). Sin lectura
 *              válida (spo2 y bpm > 0) borra la sesión.
 *
 * @param       Entrada: c - resultado: spo2, bpm y quality.
 *              Entrada: state - debe ser SESSION.
 *              Salida:  sent - true si el cierre salió bien.
 *              Salida:  state - READY.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void doFinish(const Cmd &c) {
  if (state != (uint8_t)LinkState::SESSION) {
    DBG("CLOU", "Finish ignorado: estado %u", (unsigned)state.load());
    return;
  }
  LiveTick last;
  if (xQueueReceive(tickQueue, &last, 0) == pdTRUE) sendTick(last);  // the tick posted just before Finish
  setState(LinkState::CLOSING);
  logSessionSummary("fin");
  if (!(c.spo2 > 0.0f && c.bpm > 0.0f) || !postReading(c.spo2, c.bpm, c.quality)) {
    // A session with no reading is an empty row in the history: better none at all.
    DBG("CLOU", "sin lectura final: la sesion se borra");
    deleteSession();
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "sessions/%s", sid.c_str());
  bool ok = closeStep("PATCH sessions/<sid> endedAt", patchEndedAt, path);
  sent = ok;
  closeStep("PATCH info", patchInfo, nullptr);
  ok = closeStep("DELETE live", deleteNode, "live") && ok;
  DBG("CLOU", "medicion cerrada%s", ok ? "" : " CON ERRORES");
  setState(LinkState::READY);
}

/*******************************************************************************
 * @brief       Borra la sesión y `live` de la base: nada queda en el historial.
 *
 * @details     Borra sessions/<sid> y live con reintentos, pone sent en false y
 *              vuelve a READY.
 *
 * @param       Entrada: sid - sesión a borrar.
 *              Salida:  sent - false.
 *              Salida:  state - READY.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void deleteSession() {
  char path[64];
  snprintf(path, sizeof(path), "sessions/%s", sid.c_str());
  bool ok = closeStep("DELETE sessions/<sid>", deleteNode, path);
  ok = closeStep("DELETE live", deleteNode, "live") && ok;
  sent = false;
  DBG("CLOU", "sesion borrada%s", ok ? "" : " CON ERRORES");
  setState(LinkState::READY);
}

/*******************************************************************************
 * @brief       Ejecuta el comando ABORT: descarta la sesión de una medición
 *              inválida.
 *
 * @details     Solo desde SESSION: pasa a CLOSING, loguea el resumen y borra la
 *              sesión.
 *
 * @param       Entrada: state - debe ser SESSION.
 *              Salida:  state - CLOSING y después READY.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void doAbort() {
  if (state != (uint8_t)LinkState::SESSION) {
    DBG("CLOU", "Abort ignorado: estado %u", (unsigned)state.load());
    return;
  }
  setState(LinkState::CLOSING);
  logSessionSummary("aborto");
  deleteSession();
}

/*******************************************************************************
 * @brief       Ejecuta el comando DISCONNECT: apaga la conexión a la nube.
 *
 * @details     Cierra la conexión TLS de la base, desconecta el WiFi y apaga la
 *              radio (WIFI_OFF).
 *
 * @param       Salida:  state - IDLE.
 *
 * @peripherals Internos: bloque WiFi (apagado), Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi, Firebase Realtime Database.
 *******************************************************************************/
void doDisconnect() {
  db.disconnect();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  setState(LinkState::IDLE);
  DBG("CLOU", "WiFi apagado");
}

/*******************************************************************************
 * @brief       Despacha un comando de la cola al manejador que le corresponde.
 *
 * @details     switch sobre CmdType: CONNECT, START, FINISH, ABORT o
 *              DISCONNECT.
 *
 * @param       Entrada: c - comando.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi, Firebase Realtime Database.
 *******************************************************************************/
void handle(const Cmd &c) {
  switch (c.type) {
    case CmdType::CONNECT: doConnect(); break;
    case CmdType::START: doStart(); break;
    case CmdType::FINISH: doFinish(c); break;
    case CmdType::ABORT: doAbort(); break;
    case CmdType::DISCONNECT: doDisconnect(); break;
  }
}

/*******************************************************************************
 * @brief       Tarea FreeRTOS de la nube: ejecuta los comandos y envía los
 *              ticks.
 *
 * @details     Espera hasta 20 ms un comando en cmdQueue, lo ejecuta con
 *              handle() y baja pending. Si no hay comando y el estado es
 *              SESSION, envía el tick que haya en tickQueue. No termina nunca.
 *
 * @param       Entrada: cmdQueue, tickQueue - colas de comandos y de ticks.
 *              Salida:  pending - baja al terminar cada comando.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, bloque WiFi (TLS),
 *              Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi, Firebase Realtime Database.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Encola un comando para la tarea de la nube.
 *
 * @details     Sube pending y envía a cmdQueue sin esperar. Si la cola (4) está
 *              llena pierde el comando, baja pending y lo loguea.
 *
 * @param       Entrada: c - comando.
 *              Salida:  pending, cmdQueue - un comando más en curso, o ninguno
 *                       si la cola estaba llena.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, Serial/USB-CDC (log).
 *              Externos: Ninguno.
 *******************************************************************************/
void post(const Cmd &c) {
  pending++;
  if (xQueueSend(cmdQueue, &c, 0) != pdTRUE) {
    pending--;
    DBG("CLOU", "cola de comandos llena: comando %u perdido", (unsigned)c.type);
  }
}

}  // namespace

/*******************************************************************************
 * @brief       Crea las colas y la tarea de la nube.
 *
 * @details     Una sola vez (si cmdQueue ya existe no hace nada): cola de
 *              comandos (4), cola de ticks (1) y la tarea "cloud" con
 *              CLOUD_TASK_STACK bytes de pila y prioridad 1.
 *
 * @param       Salida:  cmdQueue, tickQueue - colas creadas.
 *
 * @peripherals Internos: tarea y colas FreeRTOS. Externos: Ninguno.
 *******************************************************************************/
void cloudLinkBegin() {
  if (cmdQueue) return;
  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  tickQueue = xQueueCreate(1, sizeof(LiveTick));
  xTaskCreate(cloudTask, "cloud", CLOUD_TASK_STACK, nullptr, 1, nullptr);
}

/*******************************************************************************
 * @brief       Pide conectar a la nube (WiFi + login), en segundo plano.
 *
 * @details     Encola CONNECT; lo ejecuta la tarea de la nube.
 *
 * @param       Salida:  pending - un comando más en curso.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, bloque WiFi (vía la tarea).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void cloudLinkConnect() { post({CmdType::CONNECT, 0, 0, 0}); }

/*******************************************************************************
 * @brief       Pide abrir una sesión de medición en la nube.
 *
 * @details     Encola START; lo ejecuta la tarea de la nube.
 *
 * @param       Salida:  pending - un comando más en curso.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, bloque WiFi (vía la tarea).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void cloudLinkStartSession() { post({CmdType::START, 0, 0, 0}); }

/*******************************************************************************
 * @brief       Pide cerrar la sesión con el resultado de la medición.
 *
 * @details     Encola FINISH con spo2, bpm y quality; la tarea de la nube
 *              publica la lectura final y cierra.
 *
 * @param       Entrada: spo2, bpm, quality - resultado de la medición.
 *              Salida:  pending - un comando más en curso.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, bloque WiFi (vía la tarea).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void cloudLinkFinish(float spo2, float bpm, float quality) { post({CmdType::FINISH, spo2, bpm, quality}); }

/*******************************************************************************
 * @brief       Pide descartar la sesión de la nube.
 *
 * @details     Encola ABORT; la tarea de la nube borra la sesión y `live`.
 *
 * @param       Salida:  pending - un comando más en curso.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, bloque WiFi (vía la tarea).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void cloudLinkAbort() { post({CmdType::ABORT, 0, 0, 0}); }

/*******************************************************************************
 * @brief       Pide apagar la conexión a la nube.
 *
 * @details     Encola DISCONNECT; la tarea de la nube apaga el WiFi.
 *
 * @param       Salida:  pending - un comando más en curso.
 *
 * @peripherals Internos: tarea y colas FreeRTOS, bloque WiFi (vía la tarea).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
void cloudLinkDisconnect() { post({CmdType::DISCONNECT, 0, 0, 0}); }

/*******************************************************************************
 * @brief       Entrega el tick de este segundo a la tarea de la nube.
 *
 * @details     La cola de ticks es de 1: un tick que no llegó a enviarse se
 *              pisa y se cuenta en ticksDropped, porque un valor tarde no
 *              sirve.
 *
 * @param       Entrada: tick - datos del segundo (`live`).
 *              Salida:  tickQueue - queda con este tick.
 *              Salida:  stats.ticksDropped - suma uno si pisó un tick sin
 *                       enviar.
 *
 * @peripherals Internos: cola FreeRTOS. Externos: Ninguno.
 *******************************************************************************/
void cloudLinkPushTick(const LiveTick &tick) {
  if (!tickQueue) return;
  if (uxQueueMessagesWaiting(tickQueue) > 0) stats.ticksDropped++;  // the previous one was never sent
  xQueueOverwrite(tickQueue, &tick);
}

/*******************************************************************************
 * @brief       Devuelve el estado de la conexión a la nube.
 *
 * @details     Lee el atómico `state`.
 *
 * @param       Entrada: state - estado vigente.
 *              Salida:  retorno (LinkState) - IDLE, CONNECTING, READY, OFFLINE,
 *                       SESSION o CLOSING.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
LinkState cloudLinkState() { return (LinkState)state.load(); }

/*******************************************************************************
 * @brief       Dice si hay algún comando en cola o ejecutándose.
 *
 * @details     true mientras pending sea mayor que 0.
 *
 * @param       Entrada: pending - comandos en curso.
 *              Salida:  retorno (bool) - true si la tarea de la nube está
 *                       ocupada.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
bool cloudLinkBusy() { return pending.load() > 0; }

/*******************************************************************************
 * @brief       Dice si la última sesión se cerró bien en la nube (endedAt
 *              escrito).
 *
 * @details     Devuelve el atómico `sent`.
 *
 * @param       Entrada: sent - resultado del último cierre.
 *              Salida:  retorno (bool) - true si se cerró en la nube.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
bool cloudLinkSent() { return sent.load(); }

/*******************************************************************************
 * @brief       Devuelve una copia de los contadores de la sesión actual o la
 *              última.
 *
 * @details     Copia stats: ticks enviados, fallidos y descartados, lecturas y
 *              PUT más lento.
 *
 * @param       Entrada: stats - contadores de la sesión.
 *              Salida:  retorno (CloudLinkStats) - copia de esos contadores.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
CloudLinkStats cloudLinkStats() { return stats; }
