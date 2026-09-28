# descriptor_probe

Renders and validates this firmware's HostLink descriptor natively — no
hardware, no flashing.

```sh
tools/descriptor_probe/run.sh                 # the Makefile's default FW
tools/descriptor_probe/run.sh duet            # a specific firmware
tools/descriptor_probe/run.sh duet --json   # also dump the descriptor
```

With no firmware named, it reads the Makefile's `FW ?=` default, so it checks
whatever `make` would build.

Exits non-zero on any problem, so it works as a pre-flash gate.

## What it is for

A firmware whose `Presets` manages nothing renders a descriptor reading
`"size":0,"components":[]` — no controls, no state. The module boots, makes
sound, lights its rings and enumerates on USB with the right product string,
but the web programmer hangs at **"reconnect and verify firmware"** after an
otherwise successful flash. The symptom reads like a USB or browser
permission fault, and it isn't. See the top-level [README](../../README.md).

## How it works

Two layers, because they fail differently.

**Static checks** (`check.py static`) parse `src/<fw>/*.cpp` directly, so they
cannot go stale. They catch the bug above and its near miss — a `Pager` that
is `presets.Manage()`d but never `loop.Use()`d, which leaves the knobs
reading raw pots while presets and host edits write state nothing reads. If
the firmware declares no `hostlink::Host` these are skipped: a module with no
host link and no presets is a legitimate shape (the SDK's `examples/kick`).

**Descriptor checks** (`check.py json`) build `mirrors/<fw>.cpp` against
`lib/alchemy-sdk/stubs` and the SDK's own host-build target, render the real
descriptor, and assert it parses, carries no `error` key, and exposes
components, non-zero size, and addressable fields. This catches failures the
static pass cannot see — a bad `SeeAlso` reference, a manual section missing
a title, buffer overflow, layout drift.

Note that a descriptor build failure does **not** return 0. It degrades to a
minimal descriptor carrying an `error` root key with the reason, which is why
the checker looks for that key rather than an empty result.

## Keeping it honest

The block between the `MIRROR` markers in `mirrors/<fw>.cpp` is a deliberate
copy of that firmware's knobs, pages and jacks — it is not compiled from
`src/`, so it can drift. Update it when you add or rename a control.

Drift degrades the second layer only. The static checks read `src/<fw>/`
directly, so the check that catches the reconnect bug stays correct even if
nobody has touched the mirror in months.

## Adding a firmware

Copy an existing mirror to `mirrors/<name>.cpp` and edit the `MIRROR` block:
`kInfo` (module id, name, version), the surfaces, and `Compose()`, which
stands in for everything `main()` does before the first `loop.Tick()`. Then
point `kPageRefs` and `kJackRefs` at your pages and jacks. Nothing outside
those markers should need to change.

The mirror must live here, not in `src/<name>/` — the Makefile compiles every
`.cpp` in the firmware's folder, and this one is host-only and has its own
`main()`.

Build artifacts land in `build/descriptor-probe/`, which is gitignored.
Delete that directory to force a rebuild of the SDK host library.
