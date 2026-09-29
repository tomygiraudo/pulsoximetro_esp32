#!/usr/bin/env python3
"""UDP listener for the ESP32-C3 battery-autonomy test.

Run this on a machine on the same WiFi network as the ESP32 (the IP you put
in secrets.h as UDP_TARGET_IP). It logs every packet with a receive
timestamp to a CSV, so the battery life is just "last row time minus first
row time" once the packets stop arriving.

Usage:
    python3 listener.py [port]   # default port 5005
"""

import csv
import datetime
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5005
OUT_CSV = "battery_test_log.csv"


def main() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    print(f"Listening for UDP packets on 0.0.0.0:{PORT} -> logging to {OUT_CSV}")
    print("Ctrl+C to stop.")

    with open(OUT_CSV, "a", newline="") as f:
        writer = csv.writer(f)
        if f.tell() == 0:
            writer.writerow(["received_at", "from_ip", "raw_payload"])
            f.flush()

        while True:
            data, addr = sock.recvfrom(1024)
            now = datetime.datetime.now().isoformat(timespec="seconds")
            payload = data.decode(errors="replace")
            print(f"[{now}] {addr[0]}: {payload}")
            writer.writerow([now, addr[0], payload])
            f.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
