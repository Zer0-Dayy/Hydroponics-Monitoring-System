"""Log the ESP serial port to a file without the monitor's DTR/RTS reset toggling.

usage: python tools/serial_log.py PORT OUTFILE SECONDS [--reset]

--reset   one controlled EN pulse (RTS) after opening, so exactly one boot is
          expected; any extra boot in the log is not caused by the monitor.
Run with PlatformIO's Python (it has pyserial):
  %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/serial_log.py COM4 boot.log 60 --reset
"""
import sys
import time

import serial


def main() -> None:
    port, out, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
    ser.dtr = False  # IO0 high: normal boot, never download mode
    ser.rts = False  # EN released
    ser.open()
    start = time.monotonic()
    with open(out, "w", encoding="utf-8", errors="replace") as log:
        if "--reset" in sys.argv:
            log.write("[logger] EN reset pulse\n")
            ser.rts = True
            time.sleep(0.15)
            ser.rts = False
        buffer = b""
        while time.monotonic() - start < seconds:
            buffer += ser.read(4096)
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                log.write(f"{time.monotonic() - start:8.2f} {line.decode('utf-8', 'replace').rstrip()}\n")
                log.flush()
    ser.close()


if __name__ == "__main__":
    main()
