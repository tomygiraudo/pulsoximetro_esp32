#!/usr/bin/env python3
"""Verifica las reglas de la Realtime Database contra el servicio real.

Usa solo la biblioteca estandar y las mismas credenciales que fake_device.py
(tools/cloud/.env). Escribe unicamente bajo un deviceId de prueba
(rules-check-<hex>) y lo borra al final: no toca los datos del dispositivo real.

Comprueba (lo que el plan exige de las reglas):
  - lectura anonima de /devices/<id>                     -> 200
  - lectura anonima de / y de /devices (listar)          -> denegada
  - escritura anonima                                    -> denegada
  - escritura con el token de OTRO usuario               -> denegada
        (crea un usuario descartable con accounts:signUp y lo borra; --no-signup lo omite)
  - escritura del usuario del dispositivo (live/info/sessions) -> 200
  - valores fuera de rango / campos desconocidos         -> denegados (.validate)
  - consulta orderBy="startedAt"&limitToLast=N           -> 200 (necesita .indexOn)

Una escritura denegada por las reglas responde 401 "Permission denied" en la
API REST de la Realtime Database; el script acepta 401 y 403 e imprime el codigo
real. Sale con codigo 1 si algo no se cumple.

    python tools/cloud/check_rules.py
    python tools/cloud/check_rules.py --mock http://localhost:9000   # contra el mock local
"""

import argparse
import json
import os
import secrets
import sys
import urllib.parse

from fake_device import SERVER_TIMESTAMP, Auth, HttpError, Rest, load_env, DEFAULT_AUTH_URL, DEFAULT_TOKEN_URL

DENIED = (401, 403)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", default=os.path.join(here, ".env"))
    p.add_argument("--mock", metavar="URL", help="probar contra el mock local en vez de Firebase")
    p.add_argument("--no-signup", action="store_true", help="no crear el usuario descartable (omite la prueba de 'otro usuario')")
    args = p.parse_args()

    cfg = load_env(args.env)
    if args.mock:
        mock = args.mock.rstrip("/")
        cfg.update(
            FIREBASE_DATABASE_URL=mock,
            FIREBASE_AUTH_URL=f"{mock}/identitytoolkit/v1",
            FIREBASE_TOKEN_URL=f"{mock}/securetoken/v1/token",
            FIREBASE_EMAIL=os.environ.get("MOCK_EMAIL", "device@pulsox.test"),
            FIREBASE_PASSWORD=os.environ.get("MOCK_PASSWORD", "mock-password"),
        )
        cfg.setdefault("FIREBASE_API_KEY", "mock-api-key")
        cfg.setdefault("PULSOX_DEVICE_ID", "pulsox-mock-device")
    missing = [k for k in ("FIREBASE_DATABASE_URL", "FIREBASE_API_KEY", "FIREBASE_EMAIL", "FIREBASE_PASSWORD") if not cfg.get(k)]
    if missing:
        sys.exit(f"Faltan en {args.env} (o en el entorno): {', '.join(missing)}")

    db = cfg["FIREBASE_DATABASE_URL"].rstrip("/")
    auth_url = cfg.get("FIREBASE_AUTH_URL", DEFAULT_AUTH_URL).rstrip("/")
    api_key = urllib.parse.quote(cfg["FIREBASE_API_KEY"])
    rest = Rest()
    device = Auth(rest, cfg["FIREBASE_API_KEY"], cfg["FIREBASE_EMAIL"], cfg["FIREBASE_PASSWORD"], auth_url, cfg.get("FIREBASE_TOKEN_URL", DEFAULT_TOKEN_URL))
    device.login()
    test_id = f"rules-check-{secrets.token_hex(4)}"
    base = f"{db}/devices/{test_id}"
    print(f"Base de datos: {db}\nUsuario del dispositivo: uid {device.uid}\nDeviceId de prueba: {test_id}\n")

    results = []

    def call(method, url, body=None):
        try:
            rest.request(method, url, body)
            return 200
        except HttpError as err:
            return err.status

    def check(name, expect, actual):
        ok = actual in expect
        want = "/".join(str(e) for e in expect)
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name:<62} esperado {want:<8} obtenido {actual}")

    def url(path, token=None, **query):
        if token:
            query["auth"] = token
        qs = f"?{urllib.parse.urlencode(query)}" if query else ""
        return f"{base}/{path}.json{qs}" if path else f"{base}.json{qs}"

    now_body = lambda **kw: {"session_id": "-Ntest", "seq": 1, "ts": SERVER_TIMESTAMP, **kw}
    tok = device.token()
    other_tok = None

    try:
        # --- lecturas
        check("lectura anonima de devices/<id>", (200,), call("GET", url("info")))
        check("lectura anonima de la raiz (/)", DENIED, call("GET", f"{db}/.json"))
        check("lectura anonima de /devices (no se puede listar)", DENIED, call("GET", f"{db}/devices.json"))

        # --- escrituras no autorizadas
        check("escritura anonima de live", DENIED, call("PUT", url("live"), now_body()))
        if not args.no_signup:
            try:
                res = rest.request(
                    "POST",
                    f"{auth_url}/accounts:signUp?key={api_key}",
                    {"email": f"rules-check-{secrets.token_hex(6)}@example.invalid", "password": secrets.token_urlsafe(18), "returnSecureToken": True},
                )
                other_tok = res["idToken"]
            except HttpError as err:
                print(f"SKIP  escritura con el token de otro usuario (no se pudo crear el usuario descartable: {err.body[:80]})")
        if other_tok:
            check("escritura con el token de OTRO usuario", DENIED, call("PUT", url("live", other_tok), now_body()))

        # --- escrituras del dispositivo
        check("dispositivo: PUT info", (200,), call("PUT", url("info", tok), {"device_id": test_id, "fw_version": "check", "sensor": "x", "battery_pct": 50, "updated": SERVER_TIMESTAMP}))
        check("dispositivo: PUT live valido", (200,), call("PUT", url("live", tok), now_body(elapsed_ms=1000, finger_detected=True, spo2=97.5, spo2_valid=True, bpm=72, bpm_valid=True, signal_quality=0.8, fs=50, ppg="1,2,3")))
        started = rest.request("POST", url("sessions", tok), {"startedAt": SERVER_TIMESTAMP})
        check("dispositivo: POST sessions", (200,), 200 if started and "name" in started else 0)
        sid = started["name"] if started else "x"
        check("dispositivo: POST sessions/<sid>/readings", (200,), call("POST", url(f"sessions/{sid}/readings", tok), {"ts": SERVER_TIMESTAMP, "spo2": 96, "bpm": 70, "quality": 0.7}))
        check("dispositivo: PATCH sessions/<sid> endedAt", (200,), call("PATCH", url(f"sessions/{sid}", tok), {"endedAt": SERVER_TIMESTAMP}))

        # --- .validate
        check("validate: spo2 = 500 en live", DENIED, call("PUT", url("live", tok), now_body(spo2=500)))
        check("validate: signal_quality = 7 en live", DENIED, call("PUT", url("live", tok), now_body(signal_quality=7)))
        check("validate: campo desconocido en live", DENIED, call("PUT", url("live", tok), now_body(hack="x")))
        check("validate: live sin session_id", DENIED, call("PUT", url("live", tok), {"seq": 1, "ts": SERVER_TIMESTAMP}))
        check("validate: ppg demasiado largo", DENIED, call("PUT", url("live", tok), now_body(ppg="1," * 1500)))
        check("validate: hora del cliente en el futuro", DENIED, call("PUT", url("info", tok), {"device_id": test_id, "updated": 9999999999999}))
        check("validate: nodo desconocido bajo el dispositivo", DENIED, call("PUT", url("otro", tok), {"a": 1}))

        # --- consulta del historial (necesita .indexOn)
        check("consulta sessions orderBy=startedAt&limitToLast", (200,), call("GET", url("sessions", orderBy='"startedAt"', limitToLast=30)))
    finally:
        # limpieza: el nodo de prueba y el usuario descartable
        call("DELETE", url("", tok))
        if other_tok:
            call("POST", f"{auth_url}/accounts:delete?key={api_key}", {"idToken": other_tok})
        rest.close()

    failed = results.count(False)
    print(f"\n{len(results) - failed}/{len(results)} comprobaciones correctas")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
