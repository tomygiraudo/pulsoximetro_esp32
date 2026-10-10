#include "cloud.h"

#include <WiFi.h>

#include "debug_log.h"
#include "root_ca.h"
#include "secrets.h"

static const char AUTH_HOST[] = "identitytoolkit.googleapis.com";
static const char TOKEN_HOST[] = "securetoken.googleapis.com";
static const uint32_t DB_TIMEOUT_MS = 5000;    // a failed tick is dropped, never queued
static const uint32_t AUTH_TIMEOUT_MS = 8000;  // boot and ~hourly only: a full handshake with another host
static const uint32_t AUTH_BACKOFF_MS = 5000;  // after a failed login, no new attempt for this long

// ---------------------------------------------------------------- helpers

bool wifiConnect(uint32_t timeoutMs) {
  WiFi.persistent(false);  // no flash write of the credentials on every boot
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    DBG("WIFI", "sin conexion tras %lu ms (status %d)", (unsigned long)(millis() - start), (int)WiFi.status());
    return false;
  }
  DBG("WIFI", "conectado: IP %s RSSI %d dBm (%lu ms)", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
      (unsigned long)(millis() - start));
  return true;
}

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

static bool isJsonSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// Value of the first `"key": "<string>"` in body. Enough for the few fields we read
// (idToken, name, error.message); skips occurrences that are not a key with a string value.
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

// Firebase puts the reason in `error` (RTDB: "Permission denied") or `error.message` (Auth).
static String errorText(const String& body) {
  String e = jsonField(body, "error");
  if (e.length() == 0) e = jsonField(body, "message");
  if (e.length() == 0) e = body.substring(0, 60);
  return e;
}

// ----------------------------------------------------------------- CloudDb

CloudDb::CloudDb(const char* dbHost, const char* deviceId) : dbHost_(dbHost) {
  root_ = String("/devices/") + deviceId + "/";
  dbClient_.setCACert(ROOT_CA_PEM);
}

// One short-lived HTTPS request to an auth host (not the DB connection).
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

void CloudDb::expireToken() { refreshAtMs_ = millis(); }

void CloudDb::setRefreshAfterS(uint32_t s) {
  forcedRefreshS_ = s;
  if (idToken_.length() != 0 && s != 0) refreshAtMs_ = millis() + s * 1000UL;
}

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

int CloudDb::put(const char* path, const char* json) { return request("PUT", path, json, true, nullptr); }
int CloudDb::patch(const char* path, const char* json) { return request("PATCH", path, json, true, nullptr); }
int CloudDb::del(const char* path) { return request("DELETE", path, nullptr, true, nullptr); }

int CloudDb::post(const char* path, const char* json, String* pushKey) {
  String resp;
  int code = request("POST", path, json, false, &resp);  // not silent: we need {"name": <push key>}
  if (code >= 200 && code < 300 && pushKey) *pushKey = jsonField(resp, "name");
  return code;
}
