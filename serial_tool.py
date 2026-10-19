"""Talk to the SockPod ESP32 over USB serial without an interactive monitor.

  python serial_tool.py COM7                 # print output for 20 s
  python serial_tool.py COM7 --reset         # reboot the board first, to see boot logs
  python serial_tool.py COM7 -c s            # start (or stop) a session
  python serial_tool.py COM7 -c l -s 60      # live stats as JSON for 60 s
  python serial_tool.py COM7 -c p -s 30 -o jumps.csv   # log plot mode to a file
  python serial_tool.py COM7 -c r -s 2       # print the last session summary
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("-c", "--cmd", help="one command character to send (s, l, p, r)")
    ap.add_argument("-s", "--seconds", type=float, default=20)
    ap.add_argument("-o", "--out", help="also write every line to this file")
    ap.add_argument("--reset", action="store_true", help="pulse EN to reboot the board")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, 115200, 0.2
    ser.dtr = ser.rts = False          # don't hold the ESP32 in reset/boot mode
    ser.open()
    if args.reset:
        ser.rts = True; time.sleep(0.1); ser.rts = False
        time.sleep(0.5)
    else:
        time.sleep(0.3)
    ser.reset_input_buffer() if not args.reset else None

    if args.cmd:
        ser.write(args.cmd[0].encode())
        print(f">>> sent '{args.cmd[0]}'", flush=True)

    out = open(args.out, "w", encoding="utf-8") if args.out else None
    end = time.time() + args.seconds
    try:
        while time.time() < end:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip()
            print(text, flush=True)
            if out:
                out.write(text + "\n")
    except KeyboardInterrupt:
        pass
    finally:
        if args.cmd in ("p", "l"):
            ser.write(args.cmd.encode())   # switch plot / live mode back off
        ser.close()
        if out:
            out.close()


if __name__ == "__main__":
    sys.exit(main())
