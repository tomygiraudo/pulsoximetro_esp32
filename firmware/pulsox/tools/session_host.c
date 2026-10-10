// Runs the measurement judge (lib/ppg/ppg_session.c) on a capture CSV, on the PC.
//
// Same C code as the firmware: every sample goes through the pipeline (ppg_processor.c), and
// every once-per-second output into the session, which starts at the first second with the
// finger on, as the firmware does. Prints how the measurement ended. The input needs the
// columns n, red and ir (what tools/capture.py records).
//
//   session_host capture.csv [-v] [--finger-off S] [--saturate S [LEN]] [--noise S] [--cut S]
//
//   -v               one line per second: finger, SpO2, BPM, quality and what the judge made of it
//   --finger-off S   S seconds after the start, the finger is taken away (the sensor reads ambient light)
//   --saturate S L   S seconds after the start, the sensor clips (full scale) for L seconds (default 3)
//   --noise S [P]    from S seconds after the start, random noise of +-P % (default 10) of the signal
//   --cut S          the capture ends S seconds after the start (a measurement cut short)
//
// Build, from firmware/pulsox (any C99 compiler; `python -m ziglang cc` works too):
//   cc -O2 -std=c11 -Iinclude -Ilib/ppg tools/session_host.c lib/ppg/ppg_session.c \
//      lib/ppg/ppg_processor.c lib/ppg/sos_filter.c -lm -o session_host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppg_processor.h"
#include "ppg_session.h"

#define LINE_MAX_LEN 512

static int column(const char *header, const char *name) {
  char copy[LINE_MAX_LEN];
  strncpy(copy, header, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';
  int idx = 0;
  for (char *tok = strtok(copy, ",\r\n"); tok; tok = strtok(NULL, ",\r\n"), idx++) {
    if (strcmp(tok, name) == 0) return idx;
  }
  return -1;
}

static int field(const char *line, int idx, unsigned long *out) {
  const char *s = line;
  for (int i = 0; i < idx; i++) {
    s = strchr(s, ',');
    if (!s) return 0;
    s++;
  }
  char *end;
  *out = strtoul(s, &end, 10);
  return end != s;
}

static PpgProcessor ppg;
static PpgSession session;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "uso: %s captura.csv [-v] [--finger-off S] [--saturate S [LEN]] [--noise S] [--cut S]\n",
            argv[0]);
    return 2;
  }
  int verbose = 0, off_s = -1, sat_s = -1, sat_len = 3, cut_s = -1, noise_s = -1, noise_pct = 10;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-v")) verbose = 1;
    else if (!strcmp(argv[i], "--finger-off") && i + 1 < argc) off_s = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cut") && i + 1 < argc) cut_s = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--noise") && i + 1 < argc) {
      noise_s = atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') noise_pct = atoi(argv[++i]);
    }
    else if (!strcmp(argv[i], "--saturate") && i + 1 < argc) {
      sat_s = atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') sat_len = atoi(argv[++i]);
    } else {
      fprintf(stderr, "argumento desconocido: %s\n", argv[i]);
      return 2;
    }
  }

  FILE *in = fopen(argv[1], "r");
  if (!in) {
    perror(argv[1]);
    return 1;
  }
  char line[LINE_MAX_LEN];
  if (!fgets(line, sizeof(line), in)) return 1;
  int cred = column(line, "red"), cir = column(line, "ir");
  if (cred < 0 || cir < 0) {
    fprintf(stderr, "%s: faltan columnas red, ir\n", argv[1]);
    return 1;
  }

  ppg_init(&ppg);
  ppg_session_init(&session);
  int started = 0;
  long start_n = 0, sample = 0, end_n = -1;  // sample counts, 100 per second
  unsigned long red, ir;
  while (fgets(line, sizeof(line), in)) {
    if (!field(line, cred, &red) || !field(line, cir, &ir)) continue;
    if (started) {
      long since = sample - start_n;
      if (off_s >= 0 && since >= off_s * 100L) red = ir = 2000;  // finger away: ambient light only
      if (sat_s >= 0 && since >= sat_s * 100L && since < (sat_s + sat_len) * 100L) red = ir = 262143;
      if (noise_s >= 0 && since >= noise_s * 100L) {  // +-noise_pct % of each channel, uniform
        red = (unsigned long)((long)red + (rand() % 2001 - 1000) * (long)red * noise_pct / 100000);
        ir = (unsigned long)((long)ir + (rand() % 2001 - 1000) * (long)ir * noise_pct / 100000);
      }
      if (cut_s >= 0 && since >= cut_s * 100L) break;
    }
    sample++;
    if (!ppg_push(&ppg, (uint32_t)red, (uint32_t)ir)) continue;

    const PpgOutput *o = ppg_output(&ppg);
    if (!started) {
      if (!o->finger) continue;
      started = 1;
      start_n = sample;
    }
    PpgSessionState st = ppg_session_push(&session, o);
    if (verbose) {
      printf("%3u s | dedo %d | SpO2 %3.0f%s | BPM %5.1f%s | q %.2f | PI %.2f%%%s | buenos %u malos seguidos %u\n",
             session.seconds, o->finger, o->spo2, o->spo2_valid ? " " : "?", o->bpm, o->bpm_valid ? " " : "?",
             o->quality, o->perfusion_index, o->saturated ? " SAT" : "", session.valid_seconds, session.bad_run);
    }
    if (st != PPG_SESSION_RUNNING) {
      end_n = sample;
      break;
    }
  }
  fclose(in);

  const char *name = strrchr(argv[1], '\\');
  name = name ? name + 1 : (strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1]);
  if (!started) {
    printf("%-30s sin dedo en toda la captura\n", name);
  } else if (session.state == PPG_SESSION_DONE) {
    printf("%-30s TERMINADA a los %u s | validos %u | SpO2 %.0f BPM %.0f calidad %.2f\n", name, session.seconds,
           session.valid_seconds, session.spo2, session.bpm, session.quality);
  } else if (session.state == PPG_SESSION_ABORTED) {
    double since_event = -1.0;
    if (off_s >= 0) since_event = (end_n - start_n) / 100.0 - off_s;
    else if (sat_s >= 0) since_event = (end_n - start_n) / 100.0 - sat_s;
    else if (noise_s >= 0) since_event = (end_n - start_n) / 100.0 - noise_s;
    printf("%-30s DESCARTADA a los %u s: %s | validos %u", name, session.seconds, ppg_abort_text(session.reason),
           session.valid_seconds);
    if (since_event >= 0) printf(" | %.1f s despues del evento simulado", since_event);
    printf("\n");
  } else {
    printf("%-30s sin terminar tras %u s (captura cortada) | validos %u\n", name, session.seconds,
           session.valid_seconds);
  }
  return 0;
}
