#!/usr/bin/env python3
"""Dispositivo PulsOx simulado: hace por REST lo mismo que hara el ESP32.

Sirve para probar la web app en la nube sin firmware y es la especificacion
ejecutable del cliente del ESP32 (ver PROTOCOL.md, "Transporte en la nube").
Solo usa la biblioteca estandar.

Que hace, en orden (cada paso es una llamada HTTPS del ESP32 real):

  1. Login con email/contrasena       POST identitytoolkit .../accounts:signInWithPassword
     (renueva el token antes de que venza, 1 h)  POST securetoken .../v1/token
  2. Al arrancar                      PUT    devices/<id>/info      y   DELETE devices/<id>/live
  3. Boton de medicion presionado     POST   devices/<id>/sessions  -> {"name": <sid>}
  4. Cada segundo                     PUT    devices/<id>/live      (telemetria + PPG en CSV)
  5. Cada ~5 s con lectura valida     POST   devices/<id>/sessions/<sid>/readings
  6. Al terminar                      PATCH  sessions/<sid> {endedAt}, PATCH info, DELETE live
     (--crash lo omite: simula un corte de energia a mitad de la medicion)

Configuracion en tools/cloud/.env (esta en .gitignore; ver .env.example), o en
variables de entorno. Para probar sin Firebase: `node tools/cloud/mock_rtdb.js`
y `python tools/cloud/fake_device.py --mock http://localhost:9000`.

Ejemplos:
  python tools/cloud/fake_device.py --duration 90
  python tools/cloud/fake_device.py --cycles 3 --gap 10 --scenario mixed
  python tools/cloud/fake_device.py --duration 30 --crash
"""

import argparse
import http.client
import json
import math
import os
import random
import sys
import time
import urllib.parse

SERVER_TIMESTAMP = {".sv": "timestamp"}  # hora del servidor: el ESP32 no tiene RTC ni NTP
DEFAULT_AUTH_URL = "https://identitytoolkit.googleapis.com/v1"
DEFAULT_TOKEN_URL = "https://securetoken.googleapis.com/v1/token"
READING_EVERY_S = 5
TELEMETRY_EVERY_S = 1.0


def load_env(path):
    """KEY=VALUE por linea (# comenta). Las variables de entorno tienen prioridad."""
    values = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                values[key.strip()] = value.split(" #")[0].strip().strip('"').strip("'")
    values.update({k: v for k, v in os.environ.items() if k.startswith(("FIREBASE_", "PULSOX_"))})
    return values


class HttpError(Exception):
    def __init__(self, status, body):
        super().__init__(f"HTTP {status}: {body}")
        self.status = status
        self.body = body


class Rest:
    """Cliente HTTP minimo con conexion persistente (keep-alive).

    El ESP32 hace lo mismo con HTTPClient::setReuse(true): un handshake TLS por
    arranque en vez de uno por request (cada PUT de 1 Hz sin reuso costaria ~1 s)."""

    def __init__(self, timeout=15):
        self.timeout = timeout
        self._conn = None
        self._key = None

    def request(self, method, url, body=None, headers=None, raw=None):
        parts = urllib.parse.urlsplit(url)
        key = (parts.scheme, parts.netloc)
        path = parts.path + (f"?{parts.query}" if parts.query else "")
        data = raw if raw is not None else (None if body is None else json.dumps(body, separators=(",", ":")).encode())
        hdrs = {"Content-Type": "application/json"} if raw is None else {}
        hdrs.update(headers or {})
        for attempt in (0, 1):  # una reconexion si el servidor cerro la conexion ociosa
            try:
                if self._conn is None or self._key != key:
                    self.close()
                    cls = http.client.HTTPSConnection if parts.scheme == "https" else http.client.HTTPConnection
                    self._conn = cls(parts.netloc, timeout=self.timeout)
                    self._key = key
                self._conn.request(method, path, body=data, headers=hdrs)
                resp = self._conn.getresponse()
                payload = resp.read()
                break
            except (http.client.HTTPException, OSError):
                self.close()
                if attempt:
                    raise
        text = payload.decode("utf-8", "replace")
        parsed = json.loads(text) if text.strip() else None
        if resp.status >= 400:
            raise HttpError(resp.status, text.strip())
        return parsed

    def close(self):
        if self._conn is not None:
            try:
                self._conn.close()
            finally:
                self._conn = None


class Auth:
    """Login por REST y renovacion del idToken (vence en 1 h)."""

    def __init__(self, rest, api_key, email, password, auth_url, token_url):
        self.rest, self.api_key, self.email, self.password = rest, api_key, email, password
        self.auth_url, self.token_url = auth_url.rstrip("/"), token_url
        self.id_token = self.refresh_token = None
        self.expires_at = 0.0
        self.uid = None

    def login(self):
        url = f"{self.auth_url}/accounts:signInWithPassword?key={urllib.parse.quote(self.api_key)}"
        res = self.rest.request("POST", url, {"email": self.email, "password": self.password, "returnSecureToken": True})
        self._store(res["idToken"], res["refreshToken"], res["expiresIn"])
        self.uid = res.get("localId")

    def _refresh(self):
        form = urllib.parse.urlencode({"grant_type": "refresh_token", "refresh_token": self.refresh_token}).encode()
        url = f"{self.token_url}?key={urllib.parse.quote(self.api_key)}"
        res = self.rest.request("POST", url, raw=form, headers={"Content-Type": "application/x-www-form-urlencoded"})
        self._store(res["id_token"], res["refresh_token"], res["expires_in"])

    def _store(self, id_token, refresh_token, expires_in):
        self.id_token, self.refresh_token = id_token, refresh_token
        ttl = float(expires_in)
        self.expires_at = time.monotonic() + ttl - min(300.0, ttl / 2)  # renovar con margen

    def token(self):
        if self.id_token is None:
            self.login()
        elif time.monotonic() >= self.expires_at:
            try:
                self._refresh()
                log("token renovado")
            except (HttpError, OSError, KeyError):
                self.login()
                log("token renovado con un login nuevo")
        return self.id_token

    def invalidate(self):
        self.id_token = None


class Db:
    """Escrituras REST a /devices/<id>/... autenticadas con ?auth=<idToken>."""

    def __init__(self, rest, auth, database_url, device_id):
        self.rest, self.auth = rest, auth
        self.root = f"{database_url.rstrip('/')}/devices/{device_id}"

    def _call(self, method, path, body=None, silent=False):
        for attempt in (0, 1):
            query = {"auth": self.auth.token()}
            if silent:
                query["print"] = "silent"  # sin cuerpo de respuesta: menos datos para el ESP32
            url = f"{self.root}/{path}.json?{urllib.parse.urlencode(query)}"
            try:
                return self.rest.request(method, url, body)
            except HttpError as err:
                if err.status == 401 and attempt == 0:  # token vencido o revocado: un login nuevo y reintento
                    self.auth.invalidate()
                    continue
                raise

    def put(self, path, body):
        return self._call("PUT", path, body, silent=True)

    def patch(self, path, body):
        return self._call("PATCH", path, body, silent=True)

    def delete(self, path):
        return self._call("DELETE", path, silent=True)

    def post(self, path, body):
        return self._call("POST", path, body)  # devuelve {"name": <push key>}


# ------------------------------------------------------------------ senal


def ppg_pulse(t):
    """Un ciclo cardiaco normalizado (fase 0..1): pico sistolico + onda dicrota."""
    return math.exp(-(((t - 0.22) / 0.07) ** 2)) + 0.36 * math.exp(-(((t - 0.52) / 0.1) ** 2))


class Vitals:
    """Generador de SpO2/BPM/PPG parecido a web/js/simulator.js."""

    SCENARIOS = {"normal": (97.5, 74), "warning": (92.0, 110), "critical": (87.0, 135), "mixed": (97.5, 74)}

    def __init__(self, scenario, fs, finger_delay):
        self.spo2, self.bpm = self.SCENARIOS[scenario]
        self.scenario, self.fs = scenario, fs
        self.finger_ticks_left = finger_delay  # el dedo tarda en asentarse tras el boton
        self.finger = False
        self.quality = 0.0
        self.phase = 0.0
        self.beat_amp = 1.0
        self.episode = None  # (campo, objetivo, ticks) solo en "mixed"

    def tick(self):
        if self.finger_ticks_left > 0:
            self.finger_ticks_left -= 1
        else:
            self.finger = True
        if not self.finger:
            self.quality = 0.0
            return
        if self.scenario == "mixed":
            self._advance_episode()
        base_spo2, base_bpm = self.SCENARIOS[self.scenario]
        self.spo2 = min(100.0, max(82.0, self.spo2 + random.gauss(0, 0.25)))
        self.bpm = min(165.0, max(38.0, self.bpm + random.gauss(0, 1.1)))
        if self.scenario != "mixed":  # volver hacia la base de la situacion
            self.spo2 += (base_spo2 - self.spo2) * 0.1
            self.bpm += (base_bpm - self.bpm) * 0.1
        elif self.episode:
            field, target, _ = self.episode
            setattr(self, field, getattr(self, field) + (target - getattr(self, field)) * 0.18)
        self.quality = min(0.98, max(0.5, 0.72 + random.gauss(0, 0.08)))

    def _advance_episode(self):
        if self.episode:
            field, target, left = self.episode
            self.episode = (field, target, left - 1) if left > 1 else None
        elif random.random() < 0.05:
            if random.random() < 0.5:
                self.episode = ("spo2", random.randint(84, 94), random.randint(8, 15))
            else:
                self.episode = ("bpm", random.choice([random.randint(40, 58), random.randint(102, 140)]), random.randint(8, 15))

    def ppg_batch(self):
        """fs muestras (1 s) ya filtradas, enteras, con el pico sistolico hacia arriba."""
        step = self.bpm / 60.0 / self.fs
        noise = (1 - self.quality) * 0.06
        out = []
        for _ in range(self.fs):
            self.phase += step
            if self.phase >= 1:
                self.phase -= 1
                self.beat_amp = 1 + random.gauss(0, 0.04)
            v = (ppg_pulse(self.phase) - 0.28) * self.beat_amp * (0.55 + 0.45 * self.quality)
            out.append(round((v + random.gauss(0, noise)) * 1000))
        return out


# ---------------------------------------------------------------- medicion


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def build_live(sid, seq, elapsed_ms, vitals, battery):
    """El nodo `live`: lo que se escribe (PUT, reemplazo completo) cada segundo.

    La base no guarda nulls: un campo sin valor simplemente no se manda."""
    live = {
        "session_id": sid,
        "seq": seq,
        "ts": SERVER_TIMESTAMP,
        "elapsed_ms": elapsed_ms,  # cronometro del dispositivo: la web calcula el inicio sin fiarse de relojes
        "finger_detected": vitals.finger,
        "spo2_valid": False,
        "bpm_valid": False,
        "signal_quality": round(vitals.quality, 2),
        "battery_pct": battery,
        "fs": vitals.fs,
    }
    if vitals.finger:
        valid = vitals.quality > 0.45
        live.update(
            spo2=round(vitals.spo2, 1),
            spo2_valid=valid,
            bpm=round(vitals.bpm),
            bpm_valid=valid,
            ppg=",".join(str(s) for s in vitals.ppg_batch()),
        )
    return live


def run_measurement(db, device_id, args, battery):
    """Boton presionado -> medicion -> fin. Devuelve True si cerro de forma limpia."""
    vitals = Vitals(args.scenario, args.fs, finger_delay=2)
    sid = db.post("sessions", {"startedAt": SERVER_TIMESTAMP})["name"]
    log(f"medicion iniciada: sesion {sid}")
    started = time.monotonic()
    seq = 0
    next_reading = started + READING_EVERY_S
    try:
        while time.monotonic() - started < args.duration:
            seq += 1
            target = started + seq * TELEMETRY_EVERY_S
            time.sleep(max(0.0, target - time.monotonic()))
            vitals.tick()
            elapsed_ms = int((time.monotonic() - started) * 1000)
            try:
                db.put("live", build_live(sid, seq, elapsed_ms, vitals, battery))
                if vitals.finger and vitals.quality > 0.45 and time.monotonic() >= next_reading:
                    next_reading += READING_EVERY_S
                    db.post(
                        f"sessions/{sid}/readings",
                        {"ts": SERVER_TIMESTAMP, "spo2": round(vitals.spo2, 1), "bpm": round(vitals.bpm), "quality": round(vitals.quality, 2)},
                    )
            except (HttpError, OSError, http.client.HTTPException) as err:
                log(f"  aviso: no se pudo escribir ({err}); sigo")
            if seq % 10 == 0:
                state = f"SpO2 {vitals.spo2:.1f} BPM {vitals.bpm:.0f}" if vitals.finger else "sin dedo"
                log(f"  seq {seq}: {state}")
    except KeyboardInterrupt:
        log("interrumpido: cierro la medicion")

    if args.crash:
        log("--crash: corte de energia simulado, la medicion queda abierta")
        return False
    # Orden de cierre: primero lo que la web lee al refrescar el historial, `live` al final
    # (borrar `live` es la senal de "medicion terminada" para los visores).
    db.patch(f"sessions/{sid}", {"endedAt": SERVER_TIMESTAMP})
    db.patch("info", {"battery_pct": battery, "updated": SERVER_TIMESTAMP})
    db.delete("live")
    log(f"medicion finalizada ({seq} lecturas de 1 s)")
    return True


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", default=os.path.join(here, ".env"), help="archivo .env (por defecto tools/cloud/.env)")
    p.add_argument("--mock", metavar="URL", help="usar el mock local (node tools/cloud/mock_rtdb.js) en vez de Firebase")
    p.add_argument("--duration", type=float, default=60, help="segundos por medicion (60)")
    p.add_argument("--cycles", type=int, default=1, help="cantidad de mediciones (1)")
    p.add_argument("--gap", type=float, default=5, help="segundos de reposo entre mediciones (5)")
    p.add_argument("--scenario", choices=sorted(Vitals.SCENARIOS), default="normal", help="valores a simular (normal)")
    p.add_argument("--fs", type=int, default=50, help="frecuencia de muestreo del PPG en Hz (50)")
    p.add_argument("--battery", type=int, default=78, help="porcentaje de bateria a informar (78)")
    p.add_argument("--crash", action="store_true", help="no cerrar la medicion (simula un corte de energia)")
    args = p.parse_args()

    cfg = load_env(args.env)
    if args.mock:
        mock = args.mock.rstrip("/")
        cfg["FIREBASE_DATABASE_URL"] = mock
        cfg["FIREBASE_AUTH_URL"] = f"{mock}/identitytoolkit/v1"
        cfg["FIREBASE_TOKEN_URL"] = f"{mock}/securetoken/v1/token"
        cfg.setdefault("FIREBASE_API_KEY", "mock-api-key")
        cfg.setdefault("PULSOX_DEVICE_ID", "pulsox-mock-device")
        cfg["FIREBASE_EMAIL"] = os.environ.get("MOCK_EMAIL", "device@pulsox.test")
        cfg["FIREBASE_PASSWORD"] = os.environ.get("MOCK_PASSWORD", "mock-password")
    missing = [k for k in ("FIREBASE_DATABASE_URL", "FIREBASE_API_KEY", "FIREBASE_EMAIL", "FIREBASE_PASSWORD", "PULSOX_DEVICE_ID") if not cfg.get(k)]
    if missing:
        sys.exit(f"Faltan en {args.env} (o en el entorno): {', '.join(missing)}. Mira tools/cloud/.env.example")

    rest = Rest()
    auth = Auth(
        rest,
        cfg["FIREBASE_API_KEY"],
        cfg["FIREBASE_EMAIL"],
        cfg["FIREBASE_PASSWORD"],
        cfg.get("FIREBASE_AUTH_URL", DEFAULT_AUTH_URL),
        cfg.get("FIREBASE_TOKEN_URL", DEFAULT_TOKEN_URL),
    )
    device_id = cfg["PULSOX_DEVICE_ID"]
    db = Db(rest, auth, cfg["FIREBASE_DATABASE_URL"], device_id)

    try:
        auth.login()
        log(f"login ok (uid {auth.uid}); dispositivo {device_id}")
        # arranque del firmware: anunciarse y limpiar un `live` que haya quedado de un corte
        db.put(
            "info",
            {
                "device_id": device_id,
                "fw_version": "fake-0.1.0",
                "sensor": "MAX30102 (simulado)",
                "battery_pct": args.battery,
                "updated": SERVER_TIMESTAMP,
            },
        )
        db.delete("live")
        for cycle in range(args.cycles):
            if cycle:
                log(f"reposo {args.gap:.0f} s (el ESP32 dormiria en deep sleep)")
                time.sleep(args.gap)
            if not run_measurement(db, device_id, args, args.battery):
                break
    except HttpError as err:
        sys.exit(f"Error de la API: {err}")
    except KeyboardInterrupt:
        log("interrumpido")
    finally:
        rest.close()


if __name__ == "__main__":
    main()
