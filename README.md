# SockPod

A small pod that clips to your sock and tracks how you move: steps, cadence,
jumps, sprints and how hard you're working. It sends live stats and session
summaries to the SockPod app over Bluetooth LE.

Built on an ESP32 with an MPU-6050-class motion sensor. A BMP280 pressure sensor
for elevation was fitted at first but has been removed (see [Hardware](#hardware)),
so the pod currently reports elevation as off.

## Features

| Feature | What you get | How it's measured |
|---|---|---|
| Movement tracking | Active time vs rest time, and seconds spent at light, moderate and vigorous intensity | MPU-6050. Every second the pod averages how far acceleration rises above 1 g (ENMO). Below a threshold with no foot strike counts as rest |
| Steps and cadence | Step count, and steps per minute while walking or running | MPU-6050: each foot strike is an acceleration spike. The pod sits on one foot, so it sees every other step and doubles the count |
| Jumps | Jump count, highest and average jump | MPU-6050: the foot reads about 0 g while in the air. Height comes from airtime, `h = g·t² / 8` (0.5 s in the air ≈ 31 cm). Reasonably accurate if you land the way you took off |
| Sprints | Number of sprints, total and longest sprint time | Cadence of at least 180 steps/min with vigorous intensity for at least 3 s in a row |
| Elevation change (off for now) | Metres climbed and descended | Needs a BMP280 pressure sensor, which isn't fitted at the moment, so the pod reports elevation as off. With one fitted: air pressure relative to the start of the session, ignoring changes under 1 m as noise. That suits hills, stairs and slopes but is too coarse for jump height, which is why the MPU-6050 handles jumps |
| Wireless sync | Live stats and session summaries in the SockPod app | ESP32 built-in Bluetooth LE |

The thresholds behind these (in the `TUNING` block of `SockPod.ino`) are
starting points, not measured values. See [Tuning](#tuning).

## Hardware

- ESP32 DevKit (ESP-WROOM-32). Its USB chip is a CP2102, which needs the
  Silicon Labs CP210x driver on Windows (download it from Silicon Labs; the
  laptop has a copy in `CP210x_Universal_Windows_Driver/`, which isn't in git).
- MPU-6050 (GY-521) accelerometer + gyroscope
- BMP280 pressure sensor: **not fitted at the moment.** It dropped off the I2C
  bus after the motion tests on 2026-10-19 and was removed. The firmware still
  looks for one at boot, so it can be wired back in; without it, elevation
  reports as off (`baro` 0).

The motion sensor is on the ESP32's I2C bus (a refitted BMP280 shares the same
four wires):

| Sensor pin | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

- MPU-6050 address `0x68` (AD0 unconnected).
- If a BMP280 is refitted: address `0x76` if SDO goes to GND, `0x77` if it goes
  to 3V3. The firmware tries both and prints every I2C address it finds at boot.

## Using it

- **BOOT button:** press to start a session, press again to stop it.
- **LED:** short blip every 2 s = idle, 1 Hz blink = recording.
- When a session stops, its summary is saved to flash, so the app can still
  read it after the pod has been switched off. Only the most recent session is
  kept.

Serial commands at 115200 baud:

| Key | Does |
|---|---|
| `s` | Start / stop a session |
| `l` | Print live stats as JSON once a second (on / off) |
| `p` | Serial Plotter mode: acceleration, rotation and altitude (on / off; altitude stays 0 without a BMP280) |
| `r` | Print the last finished session as JSON |

## Bluetooth LE interface (for the app)

The pod advertises as **`SockPod`** with one service:

| Characteristic | UUID | Properties | Contents |
|---|---|---|---|
| Service | `a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a00` | | |
| Live | `a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a01` | read, notify | JSON, updated once a second |
| Session | `a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a02` | read | JSON summary of the last finished session |
| Control | `a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a03` | write | `start` or `stop` (UTF-8 text) |

All values are UTF-8 JSON. Live messages can be up to about 200 bytes, so the
app should request an MTU of at least 247 before subscribing. With the default
MTU, notifications get cut off at 20 bytes. Reads aren't affected, because BLE
reads long values in pieces.

**Live** (`rec` = 1 while recording; the counts are for the current session,
or the last one when idle):

```json
{"rec":1,"t":312,"act":250,"rest":62,"zone":2,"enmo":0.31,"steps":604,"cad":166,
 "jumps":3,"jlast":27.4,"spr":1,"up":4.0,"dn":1.0,"alt":3.1,"baro":1,"last":7}
```

| Field | Meaning |
|---|---|
| `rec` | 1 = recording, 0 = idle |
| `t` | Session time, s |
| `act`, `rest` | Active and rest time, s |
| `zone` | Intensity right now: 0 rest, 1 light, 2 moderate, 3 vigorous |
| `enmo` | Intensity right now, g |
| `steps` | Steps this session |
| `cad` | Cadence right now, steps/min (0 when not walking or running) |
| `jumps`, `jlast` | Jumps this session, height of the last jump in cm |
| `spr` | Sprints this session |
| `up`, `dn` | Metres climbed and descended this session |
| `alt` | Altitude relative to the session start, m |
| `baro` | 1 if a BMP280 is fitted and working, 0 if elevation is off (as on the current pod) |
| `last` | ID of the newest session. When it changes, read the Session characteristic |

**Session**:

```json
{"id":7,"dur":1820,"act":1500,"rest":320,"light":600,"mod":700,"vig":200,"enmo":0.42,
 "steps":3120,"gait":1100,"cad_avg":170,"cad_max":192,"jumps":12,"jump_max":38.5,
 "jump_avg":24.1,"sprints":4,"sprint_s":31,"sprint_max":11,"up":12.3,"dn":11.8,"baro":1}
```

| Field | Meaning |
|---|---|
| `id` | Session number, increases across reboots. `{"id":0}` = no session yet |
| `dur` | Session length, s |
| `act`, `rest` | Active and rest time, s |
| `light`, `mod`, `vig` | Seconds at each intensity |
| `enmo` | Average intensity while active, g |
| `steps` | Total steps |
| `gait` | Seconds spent walking or running |
| `cad_avg`, `cad_max` | Average cadence while walking or running, and the highest, steps/min |
| `jumps`, `jump_max`, `jump_avg` | Jump count, highest and average height, cm |
| `sprints`, `sprint_s`, `sprint_max` | Sprint count, total sprint time and longest sprint, s |
| `up`, `dn` | Metres climbed and descended |
| `baro` | 1 if a BMP280 was working, 0 if elevation wasn't measured (as on the current pod) |

## The app

`frontend/` is a web app that connects to the pod over Web Bluetooth. It's
plain HTML, CSS and JavaScript with no build step or packages.

- **Live view:** session timer, active and rest time, steps, cadence, intensity,
  jumps, sprints and elevation, updated every second. Elevation shows Off while
  no BMP280 is fitted.
- **Start / Stop:** controls the pod's session, the same as the BOOT button.
- **History:** each finished session is fetched from the pod and saved in the
  browser (`localStorage`). Tap a session for its intensity breakdown, cadence,
  jumps, sprints and elevation. **Export** downloads them all as JSON.
- **Demo mode:** a simulated pod for trying the app, or presenting it, without hardware.

**Hosted copy:** <https://vaibhavraj-0906.github.io/sockpod/>. Open it in
**Chrome or Edge** on the laptop and click **Connect SockPod**.

**From the local files on this laptop:**

```bash
python -m http.server 8000 --directory frontend
```

Then open <http://localhost:8000> in **Chrome or Edge** and click **Connect SockPod**.

- **Browsers:** Web Bluetooth works in Chrome and Edge on Windows, macOS,
  Android and ChromeOS. It doesn't work in iPhone/iPad browsers or Firefox; the
  app says so and offers the demo.
- **Secure page needed:** the browser only allows Bluetooth on `https://` pages
  or on `localhost`. That's why the app is hosted on GitHub Pages.
- **One pod, one session:** the pod keeps only its most recent finished
  session. Connect the app before starting the next one, or the previous one is lost.
- **Phones: planned for a future update.** The hosted copy loads in Chrome on
  Android, but connecting the pod from a phone hasn't been tested yet. When it
  is: Bluetooth must be on, and Chrome needs the Nearby devices permission
  (Location on Android 11 and older), or the device chooser stays empty.

**Updating the hosted copy:** GitHub Pages serves `frontend/` from the
[sockpod repo](https://github.com/vaibhavraj-0906/sockpod). Every push to `main`
that changes `frontend/` republishes it through `.github/workflows/pages.yml`,
usually within a minute or two (progress is under the repo's **Actions** tab):

```bash
git add frontend
git commit -m "Describe the change"
git push
```

To republish without a change, run the workflow from the **Actions** tab or with
`gh workflow run pages.yml`. If the browser still shows the old version, reload the page.

## Tuning

Every threshold is in the `TUNING` block at the top of `SockPod.ino`. They were
set from two guided motion tests on 2026-10-19, which are saved in `motion_logs/`.

To re-tune, strap the pod to your foot and run the guided test (about 2 minutes,
on the spot, USB cable plugged in):

```bash
python motion_test.py COM7
```

It prompts you through standing, walking, jogging, jumping, sprinting and
seated foot-tapping. It saves the 50 Hz acceleration trace with each phase
labelled, along with the pod's own session summary.

| Setting | Value | Measured |
|---|---|---|
| `STEP_G` | 1.8 g | Foot strikes when walking on the spot peak at 1.7–3.0 g |
| `MIN_STRIDE_MS` | 450 ms | Sprinting on the spot gave 0.46–0.64 s between strikes of one foot |
| `AIR_G` / `LAND_G` | 0.6 g / 1.5 g | In the air the foot reads 0.3–0.6 g, not 0 g. Landings peak at 2.1–2.9 g |
| `AIR_GLITCH_MS` | 80 ms | The foot jiggles in the air, and the legs extend before landing |
| `MIN_AIR_MS` | 250 ms (~8 cm) | A walking stride once looked like a 241 ms jump |
| `REST` / `MODERATE` / `VIGOROUS_ENMO_G` | 0.07 / 0.15 / 0.60 g | Standing ≤ 0.06, walking 0.10–0.31, jogging 0.17–0.33, sprinting 0.78–1.43 |
| `STILL_SD_G` | 0.08 g | Standing still varies 0.056–0.087 g within a second; a desk 0.01 g |

**Results of the second test, with these settings:**
- **Rest:** 61 s rest against 63 s active. Before tuning, it counted no rest at all.
- **Strikes:** evenly spaced in walking (~0.7 s apart), jogging (~0.6 s) and
  sprinting (~0.53 s), with no sign of double counting.
- **Seated tapping:** 0 strikes.
- **Jumps:** 4 of 5 detected (9–33 cm).
- **Sprints:** 1 detected.

## Testing

Run on 2026-10-20 with the pod still on a desk, USB plugged in, and the app
loaded from the hosted copy.

**Pod over Bluetooth** (`python ble_test.py`, 19 of 19 passed). It uses the same
GATT interface as the app:

- Advertises as `SockPod` (found in 0.6 s), with the service and the Live,
  Session and Control characteristics and their documented properties.
- MTU negotiated to 247. Live notifications came every 0.96–1.03 s, each one
  complete JSON (up to 147 bytes) with every documented field.
- `start` and `stop` started and stopped recording within a second, and the
  session ID went from 7 to 8.
- The session summary had every field, its duration matched the wall clock
  (8 s against 8.2 s), and active + rest = duration.
- Lying still, the pod counted 0 steps, jumps and sprints, and all 8 s as rest.
- An unknown command (`jump`) was ignored.
- After disconnecting, the pod advertised again; it reconnected in 4.6 s with
  the summary still there. Elevation reported off (`baro` 0).

**Pod over USB serial** (`serial_tool.py`): the boot log found the motion sensor
at 0x68, reported the BMP280 missing (elevation off), loaded the saved
calibration and started advertising. Session 8 was still readable after a reboot.
`s` started and stopped session 9, `l` printed live JSON once a second, and `r`
returned session 9's summary.

**Hosted app, demo mode** (desktop width and 375 px phone width):

- Loads over https with no console errors, and Web Bluetooth is available.
- Start/Stop work; the button shows Starting…/Stopping… and is disabled meanwhile.
- During a 70 s demo session the live counts matched the demo script (96 steps,
  4 jumps and 1 sprint at 51 s), and active + rest equalled the timer.
- The finished session appeared under Sessions with a Demo badge. Its details
  show the intensity bar and all ten facts, and light + moderate + vigorous
  equals active time.
- Export produced `sockpod-sessions.json` with the saved sessions.
- Sessions survived a page reload, and the next demo session got the next ID.
- Clear: Cancel keeps the sessions, OK deletes them. Disconnect returns to the
  connect screen. No sideways scrolling at phone width.

On 2026-10-19 the app was also checked with the real pod in Chrome on Windows:
live stats, Start/Stop, and finished sessions syncing into history.

**Not tested:** connecting from an Android phone (planned for a future update),
and real running and jumping (see [Known limitations](#known-limitations)).

**Seen during testing:**
- The first serial read after the Bluetooth test returned about 2,000 repeated
  old lines (`[BLE] App disconnected` and reset banners). A reboot straight
  after was clean, so it looks like the USB serial driver replaying buffered
  output rather than the firmware, but it's worth watching for.
- Demo mode still shows elevation, while the real pod now reports it as off.

## Known limitations

- **Tested on the spot only.** Both tests were done with the USB cable plugged
  in, so real running and real jumps haven't been tested yet. Expect harder foot
  strikes when running, which may need `STEP_G` and the intensity levels adjusting.
- **Seated foot-tapping can look like steps.** It gave 15–22 false strikes in the
  first test and 0 in the second. Acceleration alone can't reliably tell the two
  apart; the gyroscope could, see the next point.
- **The motion sensor isn't a genuine MPU-6050.** It reports WHO_AM_I `0x70`
  (an MPU-6500 ID), and its gyroscope reads exactly 0 °/s. The accelerometer works,
  and the pod measures what this board reads at rest (1.14–1.16 g) and treats it
  as 1 g. A genuine MPU-6050 or MPU-6500 module would allow rotation-based features.
- **Small jumps aren't counted.** Anything under about 8 cm (250 ms in the air) is ignored.
- **No elevation at the moment.** The BMP280 was removed after it dropped off the
  I2C bus, and elevation was never tested on stairs or a hill. Whether to refit
  it or drop the feature is still open.
- **A refitted BMP280 is only looked for at boot.** If its wiring is loose at
  power-up, elevation stays off until the next reboot.
- **I2C runs at 100 kHz.** At 400 kHz the MPU-6050 stopped answering with
  longer wires to the foot.

## Project layout

```
SockPod/SockPod.ino    firmware
serial_tool.py         talk to the board over USB serial without an interactive monitor
motion_test.py         guided motion test for tuning the thresholds
ble_test.py            checks the pod's Bluetooth interface (see Testing)
jira/                  Jira import file: work items and test cases
motion_logs/           recordings from the motion tests
frontend/              the SockPod web app (index.html, app.js, ble.js, demo.js, styles.css)
.github/workflows/     publishes frontend/ to GitHub Pages
```

`SockPod/secrets.h` is left over from the fall-alert firmware and is no longer
used. It is git-ignored; delete it once you no longer need what's in it.

## Build and flash

Needs [arduino-cli](https://arduino.github.io/arduino-cli/) and the ESP32 core:

```bash
arduino-cli config set board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.0.7
```

Compile and upload (replace `COM7` with your board's port):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 SockPod
arduino-cli upload -p COM7 --fqbn esp32:esp32:esp32 SockPod
```

- If compiling reports the sketch is too big, add the larger app partition:
  `--fqbn esp32:esp32:esp32:PartitionScheme=huge_app`.
- If the upload hangs at `Connecting...`, hold the BOOT button until it starts writing.

Watch the board's output (needs `pip install pyserial`):

```bash
python serial_tool.py COM7 --reset -s 10
```

## Status

- **Firmware:** compiles cleanly on esp32:esp32 3.0.7 (89% of app flash) and
  runs on the board.
- **Tested so far:** the motion sensor is detected at 0x68. The BMP280 was first
  detected at 0x76, then dropped off the bus and was removed, so elevation is off.
  Gravity calibration works (this MPU-6050 reads 1.148 g at rest, scaled to 1.00 g).
  Sessions start and stop from serial and from Bluetooth, and survive a reboot. Over
  Bluetooth from a Windows laptop, the MTU negotiated to 247, live notifications
  arrived complete, and the session summary read back in full.
- **Motion tested on the spot:** steps, jumps, sprints and intensity were tuned
  and checked with two guided tests. See [Tuning](#tuning) and [Known limitations](#known-limitations).
- **Not tested yet:** real running and jumping without the cable. Elevation is
  off until a BMP280 is refitted.
- **App:** works with the real pod in Chrome on Windows: live stats, Start/Stop
  from the app, and finished sessions syncing into history. Hosted at
  <https://vaibhavraj-0906.github.io/sockpod/> and tested there in demo mode,
  including the phone layout. See [Testing](#testing).
- **Future update:** using the app from an Android phone.
- **Project tracking:** Jira at <https://sockpod.atlassian.net> (members only),
  project `SCRUM`, work items SCRUM-5 to SCRUM-79: 8 epics, the finished work,
  the open work (Android phone support, accuracy and clean-up), and all 36
  test cases with their results
  ([test cases](https://sockpod.atlassian.net/issues?jql=project%20%3D%20SCRUM%20AND%20labels%20%3D%20test-case)).
  `jira/sockpod-jira-import.csv` is the same set as a CSV, for re-importing into
  another Jira project.
