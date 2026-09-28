#!/usr/bin/env python3
"""Checks for the descriptor probe. See run.sh."""

import json
import pathlib
import re
import sys

OK, BAD = "  ok  ", " FAIL "


def say(good, msg):
    print(f"[{OK if good else BAD}] {msg}")
    return good


def static_checks(fw_dir: pathlib.Path) -> bool:
    """Read the firmware's sources directly, so these cannot go stale."""
    srcs = sorted(fw_dir.glob("*.cpp"))
    if not srcs:
        return say(False, f"no sources found under {fw_dir}")
    blob = "\n".join(p.read_text() for p in srcs)

    if "hostlink::Host" not in blob:
        say(True, "no hostlink::Host declared — skipping HostLink checks")
        return True

    good = True

    managed = re.findall(r"\.Manage\(\s*(\w+)\s*\)", blob)
    if not managed:
        good = say(False,
                   "Presets manages nothing. The descriptor will report "
                   '"size":0,"components":[] and the web programmer will '
                   'hang at "reconnect and verify firmware" after a '
                   "successful flash. Manage a Pager — see the README.")
    else:
        say(True, f"Presets manages: {', '.join(sorted(set(managed)))}")

    # A Pager only backs the knobs if it is also attached to the ControlLoop.
    pagers = re.findall(r"^\s*(?:static\s+)?Pager\s+(\w+)\s*[({]", blob, re.M)
    for name in pagers:
        used = re.search(rf"\.?Use\(\s*{re.escape(name)}\s*\)", blob)
        if name not in managed:
            good = say(False,
                       f"Pager '{name}' is never presets.Manage()d, so its "
                       "knob values reach neither presets nor the host.")
        elif not used:
            good = say(False,
                       f"Pager '{name}' is managed but never loop.Use()d — "
                       "knobs would still read the raw pots, so preset and "
                       "host edits would change state nothing reads.")
        else:
            say(True, f"Pager '{name}' is both managed and attached")

    if not pagers:
        say(True, "no Pager declared (fine if another component holds state)")

    return good


def json_checks(path: pathlib.Path) -> bool:
    raw = path.read_text()
    try:
        d = json.loads(raw)
    except json.JSONDecodeError as e:
        return say(False, f"descriptor is not valid JSON: {e}")
    say(True, f"descriptor is valid JSON ({len(raw)} bytes)")

    good = True
    if "error" in d:
        good = say(False, f"descriptor carries an error: {d['error']!r}")

    comps = d.get("components", [])
    if not comps:
        good = say(False,
                   '"components" is empty — the module reports no controls. '
                   'The programmer will hang at "reconnect and verify '
                   'firmware".')
    else:
        names = ", ".join(c.get("id", "?") for c in comps)
        say(True, f"{len(comps)} component(s): {names}")

    if d.get("size", 0) <= 0:
        good = say(False, '"size" is 0 — no serialized state to verify.')
    else:
        say(True, f'serialized state is {d["size"]} bytes')

    fields = [f for c in comps for f in c.get("fields", [])]
    if not fields:
        good = say(False, "no fields — no knob is addressable by the host.")
    else:
        say(True, f"{len(fields)} field(s): "
                  + ", ".join(f.get("id", "?") for f in fields))

    return good


if __name__ == "__main__":
    mode, target = sys.argv[1], pathlib.Path(sys.argv[2])
    ok = static_checks(target) if mode == "static" else json_checks(target)
    sys.exit(0 if ok else 1)
