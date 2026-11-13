#include "cloud/cloud.h"

#include <WiFi.h>

#include "config.h"
#include "debug/debug_log.h"
#include "root_ca.h"
#include "secrets.h"

static const char AUTH_HOST[] = "identitytoolkit.googleapis.com";
static const char TOKEN_HOST[] = "securetoken.googleapis.com";
static const uint32_t DB_TIMEOUT_MS = 5000;    // a failed tick is dropped, never queued
static const uint32_t AUTH_TIMEOUT_MS = 8000;  // boot and ~hourly only: a full handshake with another host
static const uint32_t AUTH_BACKOFF_MS = 5000;  // after a failed login, no new attempt for this long

// ---------------------------------------------------------------- helpers

// The known networks, in order of preference. secrets.h either lists them (WIFI_NETWORKS) or, as it
// always did, names just one (WIFI_SSID / WIFI_PASSWORD).
struct WifiNetwork {
  const char *ssid;
  const char *password;
};
static const WifiNetwork NETWORKS[] = {
#ifdef WIFI_NETWORKS
    WIFI_NETWORKS
#else
    {WIFI_SSID, WIFI_PASSWORD}
#endif
};
static const int NETWORK_COUNT = sizeof(NETWORKS) / sizeof(NETWORKS[0]);
static_assert(NETWORK_COUNT >= 1, "secrets.h: at least one WiFi network");

// The chip reports it through an event when its own search of the SSID ends without finding it. The
// status of WiFi.status() cannot be used for that: it still holds the one of the previous attempt right
// after WiFi.begin().
static volatile bool noApFound = false;

/*******************************************************************************
 * @brief       Detecta que el chip no encontró la red WiFi que buscaba.
 *
 * @details     Callback del evento «estación desconectada»: si el motivo es
 *              NO_AP_FOUND levanta noApFound. No sirve WiFi.status() para esto:
 *              justo después de WiFi.begin() todavía conserva el del intento
 *              anterior.
 *
 * @param       Entrada: info - datos del evento (motivo de la desconexión).
 *              Salida:  noApFound - true si el SSID no se encontró.
 *
 * @peripherals Internos: bloque WiFi (evento STA desconectada).
 *              Externos: punto de acceso WiFi.
 *******************************************************************************/
static void onWifiEvent(arduino_event_id_t, arduino_event_info_t info) {
  if (info.wifi_sta_disconnected.reason == WIFI_REASON_NO_AP_FOUND) noApFound = true;
}

/*******************************************************************************
 * @brief       Intenta conectarse a una red de la lista, hasta un tiempo
 *              máximo.
 *
 * @details     Desconecta, hace WiFi.begin() con esa red y espera la conexión
 *              cada 100 ms. Con varias redes, si el chip avisa que no la
 *              encuentra (~2-3 s) la abandona enseguida en vez de esperar todo
 *              el timeout. Nunca loguea el SSID, solo su posición.
 *
 * @param       Entrada: index - posición de la red en NETWORKS.
 *              Entrada: timeoutMs - espera máxima por esta red, en ms.
 *              Salida:  retorno (bool) - true si quedó conectado.
 *              Salida:  noApFound - se borra antes de cada intento.
 *
 * @peripherals Internos: bloque WiFi (STA), Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi.
 *******************************************************************************/
static bool joinNetwork(int index, uint32_t timeoutMs) {
  static bool listening = false;
  if (!listening) {
    WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    listening = true;
  }
  WiFi.disconnect();
  noApFound = false;
  WiFi.begin(NETWORKS[index].ssid, NETWORKS[index].password);
  const uint32_t start = millis();
  bool gone = false;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    if (NETWORK_COUNT > 1 && noApFound) {
      gone = true;
      break;
    }
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    DBG("WIFI", "conectado a la red %d de %d: IP %s RSSI %d dBm (%lu ms)", index + 1, NETWORK_COUNT,
        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned long)(millis() - start));
    return true;
  }
  DBG("WIFI", "%s la red %d de %d tras %lu ms (status %d)", gone ? "no encuentra" : "sin conexion con", index + 1,
      NETWORK_COUNT, (unsigned long)(millis() - start), (int)WiFi.status());
  return false;
}

/*******************************************************************************
 * @brief       Conecta el WiFi probando las redes conocidas por orden de
 *              preferencia.
 *
 * @details     Modo estación, sin guardar las credenciales en flash y con
 *              reconexión automática. Prueba cada red de NETWORKS directamente
 *              (sin escaneo: sirve con SSID oculto) dentro de WIFI_MAX_TOTAL_MS
 *              y se queda con la primera que conecta.
 *
 * @param       Entrada: timeoutMs - espera máxima por red, en ms.
 *              Salida:  retorno (bool) - true si alguna red conectó.
 *
 * @peripherals Internos: bloque WiFi (STA), Serial/USB-CDC (log).
 *              Externos: punto de acceso WiFi.
 *******************************************************************************/
bool wifiConnect(uint32_t timeoutMs) {
  WiFi.persistent(false);  // no flash write of the credentials on every boot
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  const uint32_t start = millis();
  for (int k = 0; k < NETWORK_COUNT && millis() - start < WIFI_MAX_TOTAL_MS; k++) {
    if (joinNetwork(k, timeoutMs)) return true;
  }
  DBG("WIFI", "ninguna de las %d redes conocidas conecto (%lu ms)", NETWORK_COUNT, (unsigned long)(millis() - start));
  return false;
}

/*******************************************************************************
 * @brief       Escapa un texto para ponerlo entre comillas dentro de un JSON.
 *
 * @details     Convierte comillas, barra invertida, \n, \r y \t en su secuencia
 *              con barra, y los demás caracteres de control (menores que 0x20)
 *              en \u00XX.
 *
 * @param       Entrada: s - texto original.
 *              Salida:  retorno (String) - texto escapado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
String jsonEscape(const char* s) {
  String out;
  out.reserve(strlen(s) + 8);
  for (; *s; s++) {
    char c = *s;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c);
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out;
}

/*******************************************************************************
 * @brief       Dice si un carácter es un espacio en blanco de JSON.
 *
 * @details     Espacio, salto de línea, retorno de carro o tabulación.
 *
 * @param       Entrada: c - carácter.
 *              Salida:  retorno (bool) - true si es espacio en blanco.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static bool isJsonSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

/*******************************************************************************
 * @brief       Extrae el valor de texto de la primera clave "key": "..." de un
 *              JSON.
 *
 * @details     Busca "key" seguida de ':' y de una cadena, saltando las
 *              apariciones que no son una clave con valor de texto. Alcanza
 *              para los pocos campos que se leen (idToken, name,
 *              error.message); resuelve las secuencias con barra.
 *
 * @param       Entrada: body - JSON recibido.
 *              Entrada: key - nombre de la clave.
 *              Salida:  retorno (String) - valor, o "" si no está.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
String jsonField(const String& body, const char* key) {
  String pat = String("\"") + key + "\"";
  const int len = body.length();
  int from = 0;
  while (true) {
    int i = body.indexOf(pat, from);
    if (i < 0) return "";
    int j = i + pat.length();
    while (j < len && isJsonSpace(body[j])) j++;
    if (j >= len || body[j] != ':') { from = i + 1; continue; }
    j++;
    while (j < len && isJsonSpace(body[j])) j++;
    if (j >= len || body[j] != '"') { from = i + 1; continue; }
    j++;
    int end = j;
    bool escaped = false;
    while (end < len && body[end] != '"') {
      if (body[end] == '\\') { escaped = true; end++; }
      end++;
    }
    if (!escaped) return body.substring(j, end);
    String out;
    for (int k = j; k < end && k < len; k++) {
      char c = body[k];
      if (c == '\\' && k + 1 < len) {
        c = body[++k];
        if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
        else if (c == 'r') c = '\r';
      }
      out += c;
    }
    return out;
  }
}

/*******************************************************************************
 * @brief       Codifica un texto en porcentaje para un formulario o una URL.
 *
 * @details     Deja pasar letras, dígitos y -_.~ y convierte el resto en %XX.
 *
 * @param       Entrada: s - texto original.
 *              Salida:  retorno (String) - texto codificado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned)(uint8_t)c);
      out += b;
    }
  }
  return out;
}

/*******************************************************************************
 * @brief       Saca el motivo de un error de una respuesta de Firebase.
 *
 * @details     Firebase lo pone en `error` (RTDB: "Permission denied") o en
 *              `error.message` (Auth): prueba esas claves y, si no hay ninguna,
 *              devuelve los primeros 60 caracteres del cuerpo.
 *
 * @param       Entrada: body - cuerpo de la respuesta HTTP.
 *              Salida:  retorno (String) - texto del error.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static String errorText(const String& body) {
  String e = jsonField(body, "error");
  if (e.length() == 0) e = jsonField(body, "message");
  if (e.length() == 0) e = body.substring(0, 60);
  return e;
}

// ----------------------------------------------------------------- CloudDb

/*******************************************************************************
 * @brief       Crea el cliente de la base: guarda el host y la raíz del
 *              dispositivo.
 *
 * @details     La raíz de todas las rutas es /devices/<deviceId>/. El cliente
 *              TLS de la base queda con los certificados raíz (ROOT_CA_PEM)
 *              para validar al servidor; todavía no se conecta.
 *
 * @param       Entrada: dbHost - host de la base, sin esquema.
 *              Entrada: deviceId - identificador del dispositivo.
 *              Salida:  dbHost_, root_, dbClient_ - inicializados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
CloudDb::CloudDb(const char* dbHost, const char* deviceId) : dbHost_(dbHost) {
  root_ = String("/devices/") + deviceId + "/";
  dbClient_.setCACert(ROOT_CA_PEM);
}

/*******************************************************************************
 * @brief       Hace un pedido HTTPS corto y aparte a un servidor de
 *              autenticación.
 *
 * @details     Usa un cliente TLS propio (otro host y otra cadena de
 *              certificados que la base) con timeout AUTH_TIMEOUT_MS y hace un
 *              POST. El HTTPClient se declara después del cliente porque su
 *              destructor lo detiene.
 *
 * @param       Entrada: host, uri - servidor y ruta del pedido.
 *              Entrada: body, contentType - cuerpo del POST y su tipo.
 *              Salida:  code - código HTTP, o error de transporte (< 0).
 *              Salida:  resp - cuerpo de la respuesta (si hubo).
 *              Salida:  retorno (bool) - true si hubo respuesta HTTP.
 *
 * @peripherals Internos: bloque WiFi (cliente TLS).
 *              Externos: servidor de autenticación de Firebase.
 *******************************************************************************/
bool CloudDb::authCall(const char* host, const String& uri, const String& body, const char* contentType,
                       int* code, String* resp) {
  WiFiClientSecure client;
  client.setCACert(ROOT_CA_PEM);
  HTTPClient http;  // declared after `client`: its destructor stops the client, which must still exist
  http.setConnectTimeout(AUTH_TIMEOUT_MS);
  http.setTimeout(AUTH_TIMEOUT_MS);
  http.begin(client, host, 443, uri, true);
  http.addHeader("Content-Type", contentType);
  *code = http.POST(body);
  if (*code > 0) *resp = http.getString();
  http.end();
  return *code > 0;
}

/*******************************************************************************
 * @brief       Guarda el token de acceso y programa cuándo renovarlo.
 *
 * @details     Guarda idToken y refreshToken. Vigencia = expiresIn (3600 s si
 *              no viene); se renueva con margen de la mitad o 300 s, lo que sea
 *              menor. forcedRefreshS_ (para pruebas) reemplaza ese plazo.
 *
 * @param       Entrada: idToken, refreshToken, expiresIn - datos de la
 *                       respuesta de login o refresh.
 *              Salida:  idToken_, refreshToken_ - tokens guardados.
 *              Salida:  refreshAtMs_ - instante (millis) en que toca renovar.
 *
 * @peripherals Internos: Serial/USB-CDC (log). Externos: Ninguno.
 *******************************************************************************/
void CloudDb::storeToken(const String& idToken, const String& refreshToken, const String& expiresIn) {
  idToken_ = idToken;
  refreshToken_ = refreshToken;
  long ttl = expiresIn.toInt();
  if (ttl <= 0) ttl = 3600;
  long margin = ttl / 2 < 300 ? ttl / 2 : 300;  // renew with margin, like fake_device.py
  uint32_t waitS = forcedRefreshS_ ? forcedRefreshS_ : (uint32_t)(ttl - margin);
  refreshAtMs_ = millis() + waitS * 1000UL;
  DBG("AUTH", "token ok (%u chars), vence en %ld s, se renueva en %lu s", (unsigned)idToken_.length(), ttl,
      (unsigned long)waitS);
}

/*******************************************************************************
 * @brief       Inicia sesión en Firebase con email y contraseña y obtiene el
 *              token.
 *
 * @details     No lo intenta durante AUTH_BACKOFF_MS tras un login fallido.
 *              Hace signInWithPassword por HTTPS, borra de memoria el cuerpo
 *              con la contraseña y guarda uid y tokens; cada fallo anota
 *              lastError_ y arma la espera.
 *
 * @param       Salida:  retorno (bool) - true si el login salió bien.
 *              Salida:  uid_, idToken_, refreshToken_, refreshAtMs_ - sesión
 *                       obtenida.
 *              Salida:  lastError_, authBackoffUntilMs_ - motivo del fallo y
 *                       fin de la espera.
 *              Salida:  stats_.logins - suma un login exitoso.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: servidor de autenticación de Firebase.
 *******************************************************************************/
bool CloudDb::login() {
  if (authBackoffUntilMs_ != 0 && (int32_t)(millis() - authBackoffUntilMs_) < 0) {
    lastError_ = "login en espera (backoff)";
    return false;
  }
  String body = String("{\"email\":\"") + jsonEscape(FIREBASE_EMAIL) + "\",\"password\":\"" +
                jsonEscape(FIREBASE_PASSWORD) + "\",\"returnSecureToken\":true}";
  String uri = String("/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;
  int code = 0;
  String resp;
  uint32_t t0 = millis();
  bool sent = authCall(AUTH_HOST, uri, body, "application/json", &code, &resp);
  body = "";  // do not keep the password around
  if (!sent) {
    lastError_ = HTTPClient::errorToString(code);
    DBG("AUTH", "login: error de transporte %d (%s)", code, lastError_.c_str());
    authBackoffUntilMs_ = millis() + AUTH_BACKOFF_MS;
    return false;
  }
  if (code != 200) {
    lastError_ = errorText(resp);
    DBG("AUTH", "login rechazado: HTTP %d %s", code, lastError_.c_str());
    authBackoffUntilMs_ = millis() + AUTH_BACKOFF_MS;
    return false;
  }
  String id = jsonField(resp, "idToken");
  String rt = jsonField(resp, "refreshToken");
  if (id.length() == 0 || rt.length() == 0) {
    lastError_ = "respuesta de login sin idToken/refreshToken";
    DBG("AUTH", "%s", lastError_.c_str());
    authBackoffUntilMs_ = millis() + AUTH_BACKOFF_MS;
    return false;
  }
  authBackoffUntilMs_ = 0;
  uid_ = jsonField(resp, "localId");
  storeToken(id, rt, jsonField(resp, "expiresIn"));
  stats_.logins++;
  DBG("AUTH", "login ok (uid %s) en %lu ms", uid_.c_str(), (unsigned long)(millis() - t0));
  return true;
}

/*******************************************************************************
 * @brief       Renueva el token de acceso con el refresh token.
 *
 * @details     Pide /v1/token (grant_type=refresh_token) al servidor de tokens;
 *              si la respuesta trae id_token y refresh_token los guarda con
 *              storeToken().
 *
 * @param       Entrada: refreshToken_ - token de renovación vigente.
 *              Salida:  retorno (bool) - true si se renovó.
 *              Salida:  idToken_, refreshToken_, refreshAtMs_ - tokens nuevos y
 *                       próxima renovación.
 *              Salida:  stats_.refreshes - suma una renovación exitosa.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: servidor de tokens de Firebase.
 *******************************************************************************/
bool CloudDb::refresh() {
  String form = String("grant_type=refresh_token&refresh_token=") + urlEncode(refreshToken_);
  String uri = String("/v1/token?key=") + FIREBASE_API_KEY;
  int code = 0;
  String resp;
  uint32_t t0 = millis();
  if (!authCall(TOKEN_HOST, uri, form, "application/x-www-form-urlencoded", &code, &resp)) {
    DBG("AUTH", "refresh: error de transporte %d (%s)", code, HTTPClient::errorToString(code).c_str());
    return false;
  }
  if (code != 200) {
    DBG("AUTH", "refresh rechazado: HTTP %d %s", code, errorText(resp).c_str());
    return false;
  }
  String id = jsonField(resp, "id_token");
  String rt = jsonField(resp, "refresh_token");
  if (id.length() == 0 || rt.length() == 0) {
    DBG("AUTH", "refresh: respuesta sin id_token/refresh_token");
    return false;
  }
  storeToken(id, rt, jsonField(resp, "expires_in"));
  stats_.refreshes++;
  DBG("AUTH", "refresh en %lu ms", (unsigned long)(millis() - t0));
  return true;
}

/*******************************************************************************
 * @brief       Asegura que haya un token vigente antes de cada pedido a la
 *              base.
 *
 * @details     Sin token hace login(). Con el token vencido (refreshAtMs_)
 *              prueba refresh() y, si falla, un login() nuevo. Si ninguno anda
 *              sigue con el token actual y mira de nuevo en 10 s: una caída de
 *              Firebase cuesta un intento cada 10 s, no uno por tick.
 *
 * @param       Entrada: idToken_, refreshAtMs_ - token vigente y cuándo vence.
 *              Salida:  retorno (bool) - false solo si no hay token y el login
 *                       falló.
 *              Salida:  refreshAtMs_ - se posterga 10 s si falló la renovación.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: servidor de autenticación de Firebase.
 *******************************************************************************/
bool CloudDb::ensureToken() {
  if (idToken_.length() == 0) return login();
  if ((int32_t)(millis() - refreshAtMs_) < 0) return true;
  if (refresh()) {
    DBG("AUTH", "token renovado");
    return true;
  }
  if (login()) {
    DBG("AUTH", "token renovado con un login nuevo");
    return true;
  }
  // Neither worked: keep using the current token (it may still be valid) and look again in 10 s,
  // so a Firebase outage costs one failed auth attempt per 10 s and not one per tick.
  refreshAtMs_ = millis() + 10000;
  return true;
}

/*******************************************************************************
 * @brief       Fuerza la renovación del token en el próximo pedido.
 *
 * @details     Pone refreshAtMs_ en el instante actual (comando de prueba `r`).
 *
 * @param       Salida:  refreshAtMs_ - ahora.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void CloudDb::expireToken() { refreshAtMs_ = millis(); }

/*******************************************************************************
 * @brief       Fija cada cuántos segundos renovar el token (para pruebas).
 *
 * @details     Guarda forcedRefreshS_ (0 = usar el vencimiento real, ~55 min)
 *              y, si ya hay token y s no es 0, reprograma la renovación a s
 *              segundos desde ahora.
 *
 * @param       Entrada: s - segundos entre renovaciones; 0 = vencimiento real.
 *              Salida:  forcedRefreshS_, refreshAtMs_ - plazo forzado y próxima
 *                       renovación.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void CloudDb::setRefreshAfterS(uint32_t s) {
  forcedRefreshS_ = s;
  if (idToken_.length() != 0 && s != 0) refreshAtMs_ = millis() + s * 1000UL;
}

/*******************************************************************************
 * @brief       Hace un pedido REST a la base, con token, reintentos y
 *              estadísticas.
 *
 * @details     Asegura el token, arma la URL (ruta + .json + auth) y envía por
 *              la conexión TLS persistente. Reintenta una vez tras un 401
 *              (login nuevo) y una vez si el servidor cerró la conexión
 *              inactiva; un timeout de lectura no (duplicaría un POST).
 *
 * @param       Entrada: method, path, json - verbo HTTP, ruta bajo
 *                       devices/<id>/ y cuerpo (NULL = sin cuerpo).
 *              Entrada: silent - true agrega print=silent: la base no devuelve
 *                       cuerpo.
 *              Salida:  respBody - cuerpo de la respuesta, si se pidió.
 *              Salida:  retorno (int) - código HTTP (> 0) o error (< 0,
 *                       CLOUD_ERR_*).
 *              Salida:  lastError_, lastLatencyMs_, stats_ - motivo del fallo,
 *                       latencia y contadores.
 *
 * @peripherals Internos: bloque WiFi (TLS), Serial/USB-CDC (log).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int CloudDb::request(const char* method, const char* path, const char* json, bool silent, String* respBody) {
  lastError_ = "";
  if (WiFi.status() != WL_CONNECTED) {
    stats_.fail++;
    return CLOUD_ERR_NOWIFI;
  }
  bool authRetried = false;
  bool transportRetried = false;
  bool reachedNetwork = false;
  int code = CLOUD_ERR_AUTH;
  uint32_t t0 = 0;

  for (int attempt = 0; attempt < 3; attempt++) {
    if (!ensureToken()) {
      code = CLOUD_ERR_AUTH;
      break;
    }
    String uri = root_ + path + ".json?auth=" + idToken_ + (silent ? "&print=silent" : "");
    const bool wasConnected = dbClient_.connected();
    dbHttp_.begin(dbClient_, dbHost_, 443, uri, true);
    dbHttp_.setReuse(true);
    dbHttp_.setConnectTimeout(DB_TIMEOUT_MS);
    dbHttp_.setTimeout(DB_TIMEOUT_MS);
    if (json) dbHttp_.addHeader("Content-Type", "application/json");

    reachedNetwork = true;
    t0 = millis();
    code = json ? dbHttp_.sendRequest(method, String(json)) : dbHttp_.sendRequest(method);
    if (code > 0) {
      if (!wasConnected) stats_.tlsConnects++;
      // Read the body only when it is needed. A 204 has none, and getString() on a response
      // without Content-Length would wait for the server to close the connection.
      if (code != 204 && (respBody || code >= 400) && dbHttp_.getSize() != 0) {
        String b = dbHttp_.getString();
        if (code >= 400) lastError_ = errorText(b);
        if (respBody) *respBody = b;
      }
    }
    dbHttp_.end();  // keeps the connection open for the next request (setReuse)
    lastLatencyMs_ = millis() - t0;

    if (code == 401 && !authRetried) {
      // Expired or revoked token... or a failed .validate rule, which also answers 401: ONE retry only.
      DBG("DB", "401 en %s %s (%s): login nuevo y un reintento", method, path, lastError_.c_str());
      authRetried = true;
      idToken_ = "";
      continue;
    }
    if (code < 0) {
      dbClient_.stop();  // unknown state after a transport error: next request starts clean
      const bool stale = code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
                         code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED ||
                         code == HTTPC_ERROR_CONNECTION_LOST;
      // The server closed an idle keep-alive connection: reconnect once. A read timeout is not
      // retried (a POST could end up duplicated).
      if (wasConnected && stale && !transportRetried) {
        DBG("DB", "conexion perdida (%d) en %s %s: reconecto", code, method, path);
        transportRetried = true;
        continue;
      }
    }
    break;
  }

  if (reachedNetwork) {
    stats_.latSumMs += lastLatencyMs_;
    stats_.latCount++;
    if (lastLatencyMs_ > stats_.latMaxMs) stats_.latMaxMs = lastLatencyMs_;
  }
  if (code >= 200 && code < 300) stats_.ok++;
  else stats_.fail++;
  return code;
}

/*******************************************************************************
 * @brief       Escribe (PUT) un JSON en una ruta de la base, reemplazando lo
 *              que hubiera.
 *
 * @details     Llama a request() en modo silencioso: la base no devuelve
 *              cuerpo.
 *
 * @param       Entrada: path, json - ruta bajo devices/<id>/ y JSON a escribir.
 *              Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int CloudDb::put(const char* path, const char* json) { return request("PUT", path, json, true, nullptr); }

/*******************************************************************************
 * @brief       Actualiza (PATCH) los campos de un JSON en una ruta, sin borrar
 *              el resto.
 *
 * @details     Llama a request() en modo silencioso: la base no devuelve
 *              cuerpo.
 *
 * @param       Entrada: path, json - ruta bajo devices/<id>/ y campos a
 *                       actualizar.
 *              Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int CloudDb::patch(const char* path, const char* json) { return request("PATCH", path, json, true, nullptr); }

/*******************************************************************************
 * @brief       Borra (DELETE) una ruta de la base.
 *
 * @details     Llama a request() en modo silencioso y sin cuerpo.
 *
 * @param       Entrada: path - ruta bajo devices/<id>/.
 *              Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int CloudDb::del(const char* path) { return request("DELETE", path, nullptr, true, nullptr); }

/*******************************************************************************
 * @brief       Agrega (POST) un nodo hijo con clave automática en una ruta de
 *              la base.
 *
 * @details     No es silencioso: necesita la respuesta {"name": <clave>}. Si
 *              salió bien y pushKey no es nulo, extrae la clave con
 *              jsonField().
 *
 * @param       Entrada: path, json - ruta bajo devices/<id>/ y JSON a agregar.
 *              Salida:  pushKey - clave generada por la base (opcional).
 *              Salida:  retorno (int) - código HTTP o error (< 0).
 *
 * @peripherals Internos: bloque WiFi (TLS).
 *              Externos: Firebase Realtime Database.
 *******************************************************************************/
int CloudDb::post(const char* path, const char* json, String* pushKey) {
  String resp;
  int code = request("POST", path, json, false, &resp);  // not silent: we need {"name": <push key>}
  if (code >= 200 && code < 300 && pushKey) *pushKey = jsonField(resp, "name");
  return code;
}
