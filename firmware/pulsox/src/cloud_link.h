// The cloud connection, run by a task of its own so that the blocking parts (WiFi, the TLS
// handshakes, one HTTPS request per second) never stall the sensor polling or the screen:
// the FIFO of the MAX30102 holds 320 ms of samples, a handshake can take seconds.
//
// The loop task only posts commands (connect, start a session, finish, abort, disconnect)
// and hands over one `live` tick per second; the cloud task does the I/O. Everything is
// asynchronous: look at cloudLinkState() / cloudLinkBusy() to see how it went. The REST
// calls and their order are PROTOCOL.md "Transporte en la nube" (src/cloud.cpp is the
// client, ported from firmware/cloud-test).
//
//   cloudLinkConnect()      WiFi + login + PUT info + DELETE live   IDLE -> CONNECTING -> READY | OFFLINE
//   cloudLinkStartSession() POST sessions                           READY -> SESSION  (OFFLINE if it fails)
//   cloudLinkPushTick()     PUT live (+ POST reading every 5 s)     only while SESSION
//   cloudLinkFinish()       PATCH sessions, PATCH info, DELETE live SESSION -> CLOSING -> READY
//   cloudLinkAbort()        DELETE sessions/<sid>, DELETE live      SESSION -> CLOSING -> READY
//   cloudLinkDisconnect()   WiFi off                                -> IDLE
//
// OFFLINE means the measurement goes on without sending (no WiFi, wrong login...).
#pragma once
#include <stdint.h>
#include "config.h"

enum class LinkState : uint8_t {
  IDLE,        // nothing running, WiFi off
  CONNECTING,  // WiFi + login + the first writes
  READY,       // connected, no session open
  OFFLINE,     // could not connect (or lost the session): nothing is sent
  SESSION,     // session open, ticks go out
  CLOSING,     // finishing or aborting the session
};

// One second of the measurement, as `live` (PROTOCOL.md). A tick that the task has not
// sent by the time the next one arrives is dropped: a late value is of no use.
struct LiveTick {
  uint32_t elapsedMs;        // since the measurement started
  bool finger;
  bool spo2Valid, bpmValid;
  float spo2, bpm;           // spo2 is only sent when valid; bpm when > 0
  float quality;             // 0..1
  uint8_t ppgCount;          // samples in ppg[]; 0 omits the field
  int16_t ppg[CLOUD_PPG_FS]; // one second of the pulse trace, systolic peak up
};

struct CloudLinkStats {
  uint32_t ticksOk, ticksFail, ticksDropped;
  uint32_t readingsOk, readingsFail;
  uint32_t putMaxMs;         // slowest PUT live of the session
};

// Creates the task. Once, from setup().
void cloudLinkBegin();

void cloudLinkConnect();
void cloudLinkStartSession();
void cloudLinkPushTick(const LiveTick &tick);
// spo2 / bpm: the result of the measurement, added to the history as its last reading when
// both are > 0.
void cloudLinkFinish(float spo2, float bpm, float quality);
void cloudLinkAbort();
void cloudLinkDisconnect();

LinkState cloudLinkState();
bool cloudLinkBusy();              // a command is queued or running
bool cloudLinkSent();              // the last session was closed in the cloud (endedAt written)
CloudLinkStats cloudLinkStats();   // of the current / last session
