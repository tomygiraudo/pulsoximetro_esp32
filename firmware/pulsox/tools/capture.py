#!/usr/bin/env python3
"""Records the CSV stream of the PulsOx firmware (serial mode 'c') to files.

Splits the stream by line kind, using the "# X,<columns>" header lines the
firmware prints when CSV mode starts:
    D,...  one row per sample        -> <base>.csv
    R,...  one row per second        -> <base>_results.csv  (once the firmware emits them)
    E,...  events (FIFO overflow...) -> <base>_events.csv
Log lines ("[uptime][TAG] ...") and anything else are ignored. Sample gaps are
detected from the sample index `n` and reported at the end.

Close the serial monitor first: only one program can hold the port. Run it with
PlatformIO's Python, which already bundles pyserial (the `python` on the PATH
is the Microsoft Store stub):

    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools\\capture.py ^
        --port COM7 --seconds 60 --label dedo_quieto
"""

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import serial

KINDS = {"D": "", "R": "_results", "E": "_events"}  # line kind -> file name suffix
HANDSHAKE_ATTEMPTS = 5
HANDSHAKE_WAIT_S = 2.0
NO_DATA_TIMEOUT_S = 10.0


class LineReader:
    """Splits the serial byte stream into lines, keeping partial lines between reads."""

    def __init__(self, ser: serial.Serial):
        self.ser = ser
        self.buf = bytearray()

    def lines(self):
        chunk = self.ser.read(max(1, self.ser.in_waiting))  # blocks up to ser.timeout
        if chunk:
            self.buf.extend(chunk)
        while (i := self.buf.find(b"\n")) >= 0:
            line = bytes(self.buf[:i]).decode("utf-8", errors="replace").strip()
            del self.buf[: i + 1]
            yield line


class Recorder:
    """Writes each kind of line to its own CSV and tracks the health of the sample stream."""

    def __init__(self, base: Path):
        self.base = base
        self.headers: dict[str, str] = {}
        self.files: dict[str, object] = {}
        self.rows = {kind: 0 for kind in KINDS}
        self.bad_lines = 0
        # sample stream ("D") health
        self.n_idx = 0
        self.ir_idx: int | None = None
        self.prev_n: int | None = None
        self.lost = 0
        self.resets = 0
        self.ir_min: int | None = None
        self.ir_max: int | None = None

    def feed(self, line: str) -> None:
        if len(line) >= 4 and line[:2] == "# " and line[2] in KINDS and line[3] == ",":
            self.headers[line[2]] = line[4:]
        elif len(line) >= 2 and line[0] in KINDS and line[1] == ",":
            self._row(line[0], line[2:])

    def _row(self, kind: str, row: str) -> None:
        header = self.headers.get(kind)
        if header is None:
            return  # data from before the header (a stream already running): ignored
        if row.count(",") != header.count(","):
            self.bad_lines += 1  # truncated line (dropped bytes)
            return
        values: list[int] = []
        if kind == "D":
            try:
                values = [int(v) for v in row.split(",")]
            except ValueError:
                self.bad_lines += 1
                return

        if kind not in self.files:
            path = self.base.with_name(self.base.name + KINDS[kind] + ".csv")
            self.files[kind] = open(path, "w", encoding="utf-8", newline="\n")
            self.files[kind].write(header + "\n")
            if kind == "D":
                cols = header.split(",")
                self.n_idx = cols.index("n")
                self.ir_idx = cols.index("ir") if "ir" in cols else None
        self.files[kind].write(row + "\n")
        self.rows[kind] += 1
        if kind == "D":
            self._track_samples(values)

    def _track_samples(self, values: list[int]) -> None:
        n = values[self.n_idx]
        if self.prev_n is not None:
            if n <= self.prev_n:
                self.resets += 1  # the index went back: the sensor was restarted
            elif n > self.prev_n + 1:
                self.lost += n - self.prev_n - 1
        self.prev_n = n
        if self.ir_idx is not None:
            ir = values[self.ir_idx]
            self.ir_min = ir if self.ir_min is None else min(self.ir_min, ir)
            self.ir_max = ir if self.ir_max is None else max(self.ir_max, ir)

    def close(self) -> None:
        for f in self.files.values():
            f.close()


def handshake(ser: serial.Serial, reader: LineReader, rec: Recorder) -> bool:
    """Asks for CSV mode until the firmware answers with its header lines."""
    for _ in range(HANDSHAKE_ATTEMPTS):
        ser.write(b"c")
        ser.flush()
        deadline = time.monotonic() + HANDSHAKE_WAIT_S
        while time.monotonic() < deadline:
            for line in reader.lines():
                rec.feed(line)
            if "D" in rec.headers:
                return True
    return False


def record(reader: LineReader, rec: Recorder, seconds: float) -> None:
    """Reads until `seconds` have passed since the first sample arrived."""
    began = time.monotonic()
    first_sample_at: float | None = None
    last_progress = 0
    while True:
        for line in reader.lines():
            rec.feed(line)
        now = time.monotonic()
        if first_sample_at is None:
            if rec.rows["D"]:
                first_sample_at = now
            elif now - began > NO_DATA_TIMEOUT_S:
                raise RuntimeError(
                    "El firmware respondio pero no llegan muestras: revisar el sensor "
                    "(mirar el log del monitor serie)."
                )
            continue
        elapsed = now - first_sample_at
        if int(elapsed) > last_progress:
            last_progress = int(elapsed)
            print(f"\r  {last_progress:3d}/{seconds:.0f} s | {rec.rows['D']} muestras | "
                  f"huecos {rec.lost}", end="", flush=True)
        if elapsed >= seconds:
            return


def report(rec: Recorder, base: Path) -> None:
    print()
    print(f"Muestras: {rec.rows['D']}  |  perdidas (huecos en n): {rec.lost}  |  "
          f"reinicios del sensor: {rec.resets}  |  lineas descartadas: {rec.bad_lines}")
    if rec.ir_min is not None:
        print(f"IR: min {rec.ir_min}  max {rec.ir_max}  (con el dedo puesto se espera > 50000)")
    if rec.rows["E"]:
        print(f"Eventos: {rec.rows['E']} (ver {base.name}_events.csv)")
    if rec.rows["D"]:
        print(f"Guardado en {base}.csv" + (" (+ _results, _events)" if len(rec.files) > 1 else ""))
    else:
        print("No se guardo nada: no llegaron muestras.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default="COM7", help="serial port (default COM7, see `pio device list`)")
    ap.add_argument("--seconds", type=float, default=60, help="capture length (default 60)")
    ap.add_argument("--label", default="captura", help="name tag for the files")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by USB-CDC, kept for UART adapters")
    ap.add_argument("--dtr", action="store_true",
                    help="assert DTR on open (try it if no data arrives; may reset the board)")
    ap.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent.parent / "captures")
    args = ap.parse_args()

    label = re.sub(r"[^A-Za-z0-9_-]+", "_", args.label)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    base = args.out_dir / f"{datetime.now():%Y%m%d_%H%M%S}_{label}"

    # DTR/RTS stay low on open: transitions on those lines can reset an ESP32-C3.
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, args.baud, 0.2
    ser.dtr, ser.rts = args.dtr, False
    try:
        ser.open()
    except serial.SerialException as err:
        print(f"No se pudo abrir {args.port}: {err}\n"
              "Cerra el monitor serie (pio device monitor / VSCode) si lo tenes abierto.")
        return 1

    reader, rec = LineReader(ser), Recorder(base)
    status = 0
    try:
        if not handshake(ser, reader, rec):
            print("El firmware no contesto al modo CSV ('c'). Revisar el puerto, o probar --dtr.")
            status = 1
        else:
            print(f"Capturando {args.seconds:.0f} s en {args.port}...")
            record(reader, rec, args.seconds)
    except KeyboardInterrupt:
        print("\nInterrumpido: se guarda lo capturado hasta ahora.")
    except RuntimeError as err:
        print(err)
        status = 1
    finally:
        try:
            ser.write(b"s")  # back to the summary mode, so the CSV doesn't keep flooding
            ser.flush()
        except serial.SerialException:
            pass
        ser.close()
        rec.close()
    report(rec, base)
    return status


if __name__ == "__main__":
    sys.exit(main())
