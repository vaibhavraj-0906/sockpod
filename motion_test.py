"""Guided motion test for SockPod.

Prompts you through standing, walking, jogging, jumping, sprinting and fidgeting
in place while it records the pod's 50 Hz plot output, labelled by phase. A
session runs alongside, so the pod's own counts are saved too.

  python motion_test.py COM7

Writes motion_logs/motion_<time>.csv (time, phase, accel_g, rotation_dps, alt_m)
and motion_logs/motion_<time>.json (the pod's session summary).
"""
import argparse
import csv
import json
import os
import threading
import time
from datetime import datetime

import serial

# (label, seconds, what to do). Counts are what the analysis checks against.
PHASES = [
    ("still",   6, "Stand still"),
    ("walk",   20, "Walk in place: exactly 20 steps (count both feet), then stand still"),
    ("still",   5, "Stand still"),
    ("jog",    15, "Jog in place: exactly 20 steps (count both feet), then stand still"),
    ("still",   5, "Stand still"),
    ("jump",   20, "Jump 5 times on both feet, as high as is comfortable, about 2 s apart"),
    ("still",   5, "Stand still"),
    ("sprint", 10, "Sprint in place as fast as you can"),
    ("still",   6, "Stand still, then sit down"),
    ("fidget", 10, "Seated: swing and tap your foot (should NOT count as steps)"),
    ("still",   5, "Keep your foot still"),
]


def reader(ser, state, rows, other):
    while not state["stop"]:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", errors="replace").strip()
        if text.startswith("accel_g:"):
            try:
                v = dict(kv.split(":") for kv in text.split(","))
                rows.append((round(time.time() - state["t0"], 3), state["phase"],
                             float(v["accel_g"]), float(v["rotation_x100dps"]) * 100,
                             float(v["alt_m"])))
            except (ValueError, KeyError):
                pass
        elif text:
            other.append(text)


def wait_for(pred, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, 115200, 0.2
    ser.dtr = ser.rts = False          # don't reset the board
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("\nSockPod motion test (about 2 minutes). Keep the USB cable plugged in;")
    print("do everything on the spot, and keep the cable clear of your feet.")
    print("Don't press the BOOT button while strapping the pod on: it starts/stops sessions.")
    input("Put the pod on your sock, then press Enter to start... ")

    state = {"stop": False, "phase": "setup", "t0": time.time()}
    rows, other = [], []
    threading.Thread(target=reader, args=(ser, state, rows, other), daemon=True).start()

    # Start a session (send 's' again if one was already running and we just stopped it)
    ser.write(b"s")
    wait_for(lambda: any("[SESSION]" in o for o in other), 2)
    if any("stopped" in o for o in other):
        ser.write(b"s")
        wait_for(lambda: any("started" in o for o in other), 2)
    # Plot mode on (toggle again if it was already on)
    ser.write(b"p")
    if not wait_for(lambda: len(rows) > 5, 1.5):
        ser.write(b"p")
        if not wait_for(lambda: len(rows) > 5, 1.5):
            ser.write(b"s")            # stop the session we started
            time.sleep(0.5)
            state["stop"] = True
            ser.close()
            print("\nNo sensor data from the pod, so the test was stopped.")
            for o in [o for o in other if "MPU" in o][-3:]:
                print("  pod said:", o)
            print("Check the MPU-6050 wires (VCC, GND, SDA->21, SCL->22), then run this again.")
            return

    for label, secs, text in PHASES:
        if label != "still":
            state["phase"] = "ready"
            for n in (3, 2, 1):
                print(f"\r  Next: {text} ... {n}   ", end="", flush=True)
                time.sleep(1)
            print()
        state["phase"] = label
        print(f">>> {label.upper()}: {text}")
        for left in range(secs, 0, -1):
            print(f"\r    {left:2d} s left ", end="", flush=True)
            time.sleep(1)
        print()

    state["phase"] = "end"
    ser.write(b"p")                    # plot off so the summary prints
    time.sleep(0.5)
    before = sum(o.startswith('{"id"') for o in other)
    ser.write(b"s")                    # stop the session
    wait_for(lambda: sum(o.startswith('{"id"') for o in other) > before, 3)
    state["stop"] = True
    time.sleep(0.3)
    ser.close()

    os.makedirs("motion_logs", exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = os.path.join("motion_logs", f"motion_{stamp}")
    with open(path + ".csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["t", "phase", "accel_g", "rotation_dps", "alt_m"])
        w.writerows(rows)
    # The last summary is this test's session; an earlier one can come from a
    # session that was already running (e.g. BOOT pressed while strapping the pod on)
    summary = next((o for o in reversed(other) if o.startswith('{"id"')), None)
    with open(path + ".json", "w", encoding="utf-8") as f:
        f.write(summary or "{}")

    print(f"\nDone. {len(rows)} samples saved to {path}.csv")
    if summary:
        s = json.loads(summary)
        print(f"Pod counted: {s['steps']} steps, {s['jumps']} jumps "
              f"(max {s['jump_max']} cm), {s['sprints']} sprints, "
              f"{s['act']} s active / {s['rest']} s rest")
    else:
        print("No session summary received from the pod.")
    failures = sum("Read failed" in o for o in other)
    if failures:
        print(f"Warning: {failures} sensor read errors during the test; check the MPU-6050 wires.")


if __name__ == "__main__":
    main()
