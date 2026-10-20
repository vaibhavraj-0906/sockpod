"""Black-box test of the SockPod BLE GATT interface (the one the web app uses).

  python ble_test.py              # run the checks (needs pip install bleak)
  python ble_test.py test_logs    # also save the results and the pod's previous session there

Runs a short test session, which replaces the last session stored on the pod, so
pass a folder to keep a copy of it. Prints one PASS/FAIL line per check.
Keep the pod still on a desk, switched on, and not connected to the app.
"""
import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a00"
LIVE = "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a01"
SESSION = "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a02"
CONTROL = "a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a03"
LIVE_FIELDS = {"rec", "t", "act", "rest", "zone", "enmo", "steps", "cad", "jumps",
               "jlast", "spr", "up", "dn", "alt", "baro", "last"}
SESSION_FIELDS = {"id", "dur", "act", "rest", "light", "mod", "vig", "enmo", "steps", "gait",
                  "cad_avg", "cad_max", "jumps", "jump_max", "jump_avg", "sprints",
                  "sprint_s", "sprint_max", "up", "dn", "baro"}

out_dir = sys.argv[1] if len(sys.argv) > 1 else None
results = []


def check(name, ok, detail=""):
    results.append({"test": name, "pass": bool(ok), "detail": str(detail)})
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)


async def read_json(client, uuid):
    return json.loads((await client.read_gatt_char(uuid)).decode())


async def main():
    t0 = time.time()
    dev = await BleakScanner.find_device_by_name("SockPod", timeout=15)
    check("T01 pod advertises as 'SockPod'", dev is not None,
          f"{dev.address} found in {time.time() - t0:.1f} s" if dev else "not found in 15 s")
    if not dev:
        return

    async with BleakClient(dev, timeout=20) as client:
        check("T02 connect over BLE", client.is_connected)
        svc = client.services.get_service(SERVICE)
        chars = {c.uuid: c.properties for c in svc.characteristics} if svc else {}
        check("T03 SockPod service and 3 characteristics present",
              svc and {LIVE, SESSION, CONTROL} <= set(chars), chars)
        check("T04 negotiated MTU >= 200 (live JSON fits one notification)",
              client.mtu_size >= 200, f"MTU {client.mtu_size}")

        before = await read_json(client, SESSION)
        if out_dir:
            with open(f"{out_dir}/pod_session_before_test.json", "w") as f:
                json.dump(before, f)
        check("T05 Session read returns JSON", "id" in before, f"existing session id {before.get('id')}")

        live = await read_json(client, LIVE)
        check("T06 Live read has every documented field", LIVE_FIELDS <= set(live),
              f"missing {sorted(LIVE_FIELDS - set(live))}" if not LIVE_FIELDS <= set(live) else live)
        check("T07 elevation reported off (BMP280 removed)", live.get("baro") == 0, f"baro={live.get('baro')}")
        if live.get("rec") == 1:   # leave the pod idle before testing start/stop
            await client.write_gatt_char(CONTROL, b"stop", response=True)
            await asyncio.sleep(1.5)

        notes = []
        def on_live(_, data):
            notes.append((time.time(), bytes(data)))
        await client.start_notify(LIVE, on_live)
        await asyncio.sleep(6)
        parsed = []
        for _, raw in notes:
            try:
                parsed.append(json.loads(raw.decode()))
            except ValueError:
                pass
        gaps = [b[0] - a[0] for a, b in zip(notes, notes[1:])]
        check("T08 live notifications about once a second", len(notes) >= 5 and all(0.7 < g < 1.4 for g in gaps),
              f"{len(notes)} in 6 s, gaps {[round(g, 2) for g in gaps]}")
        check("T09 every notification is complete JSON", notes and len(parsed) == len(notes),
              f"{len(parsed)}/{len(notes)} parsed, max {max((len(r) for _, r in notes), default=0)} bytes")

        last_before = parsed[-1]["last"] if parsed else live["last"]
        notes.clear()
        t_start = time.time()
        await client.write_gatt_char(CONTROL, b"start", response=True)
        await asyncio.sleep(2)
        rec_on = [json.loads(r.decode()) for _, r in notes]
        check("T10 'start' command starts recording", any(m["rec"] == 1 for m in rec_on),
              f"rec values {[m['rec'] for m in rec_on]}")

        await asyncio.sleep(6)
        during = [json.loads(r.decode()) for _, r in notes]
        ts = [m["t"] for m in during if m["rec"] == 1]
        check("T11 session timer counts up while recording", len(ts) >= 5 and ts == sorted(ts) and ts[-1] >= 6,
              f"t = {ts}")
        lastm = during[-1]
        check("T12 desk-still pod: no steps/jumps/sprints, time counted as rest",
              lastm["steps"] == 0 and lastm["jumps"] == 0 and lastm["spr"] == 0 and lastm["rest"] >= lastm["act"],
              f"steps {lastm['steps']}, jumps {lastm['jumps']}, spr {lastm['spr']}, act {lastm['act']}, rest {lastm['rest']}")

        notes.clear()
        await client.write_gatt_char(CONTROL, b"stop", response=True)
        elapsed = time.time() - t_start
        await asyncio.sleep(2)
        after = [json.loads(r.decode()) for _, r in notes]
        check("T13 'stop' command stops recording", after and after[-1]["rec"] == 0,
              f"rec values {[m['rec'] for m in after]}")
        new_last = after[-1]["last"] if after else None
        check("T14 'last' session ID goes up by one", new_last == last_before + 1,
              f"{last_before} -> {new_last}")

        summary = await read_json(client, SESSION)
        check("T15 Session summary is the new session with every field",
              summary.get("id") == new_last and SESSION_FIELDS <= set(summary),
              summary)
        check("T16 summary duration matches wall clock (+/-2 s)", abs(summary.get("dur", 0) - elapsed) <= 2,
              f"dur {summary.get('dur')} s vs {elapsed:.1f} s")
        check("T17 summary act + rest = dur", summary.get("act", 0) + summary.get("rest", 0) == summary.get("dur"),
              f"{summary.get('act')} + {summary.get('rest')} vs {summary.get('dur')}")

        notes.clear()
        await client.write_gatt_char(CONTROL, b"jump", response=True)
        await asyncio.sleep(2)
        junk = [json.loads(r.decode()) for _, r in notes]
        check("T18 unknown command ignored, pod keeps running",
              junk and junk[-1]["rec"] == 0 and junk[-1]["last"] == new_last, f"{len(junk)} updates after")
        await client.stop_notify(LIVE)

    await asyncio.sleep(2)
    t1 = time.time()
    dev = await BleakScanner.find_device_by_name("SockPod", timeout=15)
    ok = False
    if dev:
        async with BleakClient(dev, timeout=20) as client:
            again = await read_json(client, SESSION)
            ok = again.get("id") == new_last
    check("T19 pod advertises again after disconnect; session still readable", ok,
          f"reconnected in {time.time() - t1:.1f} s" if ok else "")


try:
    asyncio.run(main())
except Exception as e:
    check("T99 test run finished without errors", False, repr(e))
if out_dir:
    with open(f"{out_dir}/ble_results.json", "w") as f:
        json.dump(results, f, indent=1)
print(f"\n{sum(r['pass'] for r in results)}/{len(results)} passed")
