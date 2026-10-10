// Runs the PPG pipeline (lib/ppg) on a capture CSV, on the PC.
//
// Same C code as the firmware, fed from a file instead of the sensor. Writes what
// tools/capture.py would record from the board in CSV mode:
//   <out>.csv          one row per sample   (header: PPG_CSV_D_HEADER)
//   <out>_results.csv  one row per second   (header: PPG_CSV_R_HEADER)
// so tools/validate_ppg.py reads the result of the host and of the board alike.
//
// The input needs columns named n, red and ir (what capture.py wrote before the
// pipeline existed, e.g. captures/pulgar_V4.csv).
//
// Build, from firmware/pulsox (any C99 compiler; `python -m ziglang cc` works too):
//   cc -O2 -std=c11 -Iinclude -Ilib/ppg tools/ppg_host.c lib/ppg/ppg_processor.c \
//      lib/ppg/sos_filter.c lib/ppg/ppg_csv.c -lm -o ppg_host
// Run:
//   ./ppg_host captures/pulgar_V4.csv /tmp/pulgar_V4_host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppg_csv.h"
#include "ppg_processor.h"

#define LINE_MAX_LEN 512

// Index of the column called `name` in a comma-separated header, or -1.
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

// The `idx`-th comma-separated field of `line` as an unsigned integer.
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

static PpgProcessor ppg;  // ~8 KB: static, as in the firmware

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "uso: %s entrada.csv salida_base\n", argv[0]);
    return 2;
  }
  FILE *in = fopen(argv[1], "r");
  if (!in) {
    perror(argv[1]);
    return 1;
  }
  char path[1024], line[LINE_MAX_LEN];
  snprintf(path, sizeof(path), "%s.csv", argv[2]);
  FILE *fd = fopen(path, "w");
  snprintf(path, sizeof(path), "%s_results.csv", argv[2]);
  FILE *fr = fopen(path, "w");
  if (!fd || !fr) {
    perror("salida");
    return 1;
  }

  if (!fgets(line, sizeof(line), in)) return 1;
  int cn = column(line, "n"), cred = column(line, "red"), cir = column(line, "ir");
  if (cn < 0 || cred < 0 || cir < 0) {
    fprintf(stderr, "%s: faltan columnas n, red, ir\n", argv[1]);
    return 1;
  }
  fprintf(fd, "%s\n", PPG_CSV_D_HEADER);
  fprintf(fr, "%s\n", PPG_CSV_R_HEADER);

  ppg_init(&ppg);
  char out[LINE_MAX_LEN];
  unsigned long n, red, ir;
  long rows = 0;
  while (fgets(line, sizeof(line), in)) {
    if (!field(line, cn, &n) || !field(line, cred, &red) || !field(line, cir, &ir)) continue;
    bool refreshed = ppg_push(&ppg, (uint32_t)red, (uint32_t)ir);
    ppg_csv_d(out, sizeof(out), (uint32_t)n, (uint32_t)red, (uint32_t)ir, ppg_debug(&ppg));
    fputs(out + 2, fd);  // the files drop the "D," / "R," prefix, as capture.py does
    if (refreshed) {
      ppg_csv_r(out, sizeof(out), (uint32_t)n, ppg_output(&ppg));
      fputs(out + 2, fr);
    }
    rows++;
  }
  fclose(in);
  fclose(fd);
  fclose(fr);
  fprintf(stderr, "%ld muestras procesadas\n", rows);
  return 0;
}
