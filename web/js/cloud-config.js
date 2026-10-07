// Configuración de la fuente "Nube" (ver README.md, "Nube (Firebase)").
//
//   databaseURL  URL de la Realtime Database de Firebase, sin barra final:
//                "https://<proyecto>-default-rtdb.firebaseio.com" (o, según la región,
//                "https://<proyecto>-default-rtdb.<region>.firebasedatabase.app").
//                Vacío = la fuente "Nube" no se ofrece y la app funciona como antes
//                (Red local / Demo).
//   deviceId     Identificador del dispositivo dentro de la base (/devices/<deviceId>).
//                Es la "llave" de la URL de lectura pública: dejá uno no obvio y sin
//                datos personales. Debe coincidir con PULSOX_DEVICE_ID del ESP32 /
//                tools/cloud/.env.
//
// No es un secreto: la lectura es pública y la escritura la restringen las reglas
// (tools/cloud/database.rules.json). Nunca pongas acá una API key de servicio ni
// contraseñas.
window.PULSOX_CLOUD = {
  databaseURL: "",
  deviceId: "pulsox-4ba0e4ea46d7",
};
