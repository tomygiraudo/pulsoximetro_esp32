// Firebase Realtime Database client over REST (HTTPS), ESP32 side of
// PROTOCOL.md "Transporte en la nube". Port of Rest / Auth / Db from
// tools/cloud/fake_device.py. Reusable as is by the real firmware.
//
//  * DB connection: one persistent TLS connection (HTTPClient::setReuse(true)),
//    reopened by itself if the server closes it. CloudStats::tlsConnects counts
//    the handshakes, so a measurement that reuses the connection reads 1.
//  * Auth connection: separate and short-lived (other host, other CA chain),
//    used at boot and again ~5 min before the idToken expires.
//  * 401 -> new login and ONE retry (a failed .validate rule also answers 401,
//    so it must not loop).
//  * Never logs the token, the password or the API key.
#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Return codes of the CloudDb calls: > 0 is the HTTP status; < 0 is a transport
// error (HTTPClient's HTTPC_ERROR_*) or one of these.
#define CLOUD_ERR_NOWIFI (-101)  // WiFi is down: the request was not even tried
#define CLOUD_ERR_AUTH (-102)    // no valid idToken (login / refresh failed)

struct CloudStats {
  uint32_t ok = 0;           // calls that ended in 2xx
  uint32_t fail = 0;         // calls that ended in anything else (incl. no WiFi)
  uint32_t latSumMs = 0;     // latency of the last request of each call that reached the network
  uint32_t latMaxMs = 0;
  uint32_t latCount = 0;
  uint32_t tlsConnects = 0;  // new handshakes on the DB connection (1 = it was reused)
  uint32_t logins = 0;       // signInWithPassword calls that succeeded
  uint32_t refreshes = 0;    // token refreshes that succeeded
};

// WiFi STA with auto-reconnect. Logs IP and RSSI. false if it times out.
bool wifiConnect(uint32_t timeoutMs);

// Minimal helpers (the JSON is built with snprintf, no ArduinoJson).
String jsonEscape(const char* s);
String jsonField(const String& body, const char* key);  // string value of "key", "" if absent

class CloudDb {
 public:
  // dbHost: e.g. "xxx-default-rtdb.firebaseio.com" (no scheme). Paths are relative
  // to devices/<deviceId>/ and without ".json" (e.g. "live", "sessions/<sid>").
  CloudDb(const char* dbHost, const char* deviceId);

  bool login();                          // signInWithPassword; false and logs the reason on failure

  /*******************************************************************************
   * @brief       Devuelve el uid del usuario con el que se inició sesión.
   *
   * @details     Devuelve uid_ (lo llena login()); vacío si todavía no hubo
   *              login.
   *
   * @param       Entrada: uid_ - uid obtenido en el login.
   *              Salida:  retorno (const String&) - ese uid.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  const String& uid() const { return uid_; }

  /*******************************************************************************
   * @brief       Cierra la conexión TLS persistente con la base.
   *
   * @details     Llama a dbClient_.stop(); se usa antes de apagar el WiFi. El
   *              próximo pedido vuelve a conectar.
   *
   * @param       Salida:  dbClient_ - conexión cerrada.
   *
   * @peripherals Internos: bloque WiFi (conexión TLS).
   *              Externos: Firebase Realtime Database.
   *******************************************************************************/
  void disconnect() { dbClient_.stop(); }  // drops the persistent DB connection (before WiFi goes off)

  void expireToken();                    // the next call renews the token (command `r`)
  void setRefreshAfterS(uint32_t s);     // test: renew s seconds after each login/refresh instead of ~55 min

  int put(const char* path, const char* json);
  int patch(const char* path, const char* json);
  int del(const char* path);
  int post(const char* path, const char* json, String* pushKey = nullptr);

  /*******************************************************************************
   * @brief       Devuelve la latencia del último pedido que llegó a la red.
   *
   * @details     Devuelve lastLatencyMs_, en milisegundos.
   *
   * @param       Entrada: lastLatencyMs_ - latencia del último pedido.
   *              Salida:  retorno (uint32_t) - esa latencia, en ms.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  uint32_t lastLatencyMs() const { return lastLatencyMs_; }

  /*******************************************************************************
   * @brief       Devuelve el texto de error de Firebase de la última llamada
   *              fallida.
   *
   * @details     Devuelve lastError_; vacío si la última llamada salió bien.
   *
   * @param       Entrada: lastError_ - texto del último error.
   *              Salida:  retorno (const String&) - ese texto.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  const String& lastError() const { return lastError_; }  // Firebase "error" text of the last failed call

  /*******************************************************************************
   * @brief       Devuelve los contadores de uso de la nube.
   *
   * @details     Devuelve stats_: llamadas ok y fallidas, latencias, handshakes
   *              TLS, logins y renovaciones.
   *
   * @param       Entrada: stats_ - contadores acumulados.
   *              Salida:  retorno (const CloudStats&) - esos contadores.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  const CloudStats& stats() const { return stats_; }

 private:
  int request(const char* method, const char* path, const char* json, bool silent, String* respBody);
  bool ensureToken();
  bool refresh();
  bool authCall(const char* host, const String& uri, const String& body, const char* contentType,
                int* code, String* resp);
  void storeToken(const String& idToken, const String& refreshToken, const String& expiresIn);

  const char* dbHost_;
  String root_;  // "/devices/<deviceId>/"
  WiFiClientSecure dbClient_;  // persistent
  HTTPClient dbHttp_;          // must outlive the requests: its destructor would stop the client

  String idToken_, refreshToken_, uid_, lastError_;
  uint32_t refreshAtMs_ = 0;
  uint32_t authBackoffUntilMs_ = 0;  // millis() deadline after a failed login; 0 = none
  uint32_t forcedRefreshS_ = 0;
  uint32_t lastLatencyMs_ = 0;
  CloudStats stats_;
};
