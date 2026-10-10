// The PulsOx application: the life cycle of the device, from the button to deep sleep.
//
//   asleep ──button──► CONNECTING   "Conectando": WiFi + login + the first writes, in the cloud task
//                          │ cloud ready (or failed: it measures anyway, without sending)
//                          ▼
//                      WAIT_FINGER  "Colocá el dedo": the sensor starts; FINGER_WAIT_S without a finger: sleep
//                          │ finger
//                          ▼
//                      MEASURING    the pulse, SpO2 and BPM on the screen, one tick a second to the cloud.
//                          │        The button is ignored. ppg_session judges it every second.
//             DONE ──────┴────── ABORTED (finger away, erratic signal, not enough valid readings...)
//               │                   │
//               ▼                   ▼
//           CLOSING             CLOSING        the session is closed in the cloud (DONE) or deleted
//               │                   │          from it (ABORTED); the sensor goes to shutdown, WiFi off
//               ▼                   ▼
//            RESULT            ERROR_SCREEN    held RESULT_SHOW_S / ERROR_SHOW_S, then deep sleep.
//                                              The button starts a new measurement from CONNECTING.
#pragma once

void appSetup();
void appLoop();
