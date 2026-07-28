# Device Configuration Restoration — 2026-07-28

Status: P0-012 device restoration verified locally and on hosted three-OS
CI plus sanitizers; Week 6 remains open.

## Scope

Schema v4 stored a device configuration but nothing consumed it. This adds
the decision layer: given a stored configuration and the hardware actually
enumerated on this machine, what should be opened?

`resolveDeviceConfiguration` is **pure logic over an injected device
inventory**. It never enumerates or touches a device itself. That is a
deliberate design choice, not a shortcut: it makes every restoration path
— including hardware this project will never own — testable on the one
machine available (R-13), and it keeps backend-specific enumeration out of
the restoration rules.

| Status | Meaning |
|---|---|
| `restored` | Every stored field honored exactly |
| `adjusted` | Same device, but rate/buffer/channels renegotiated |
| `substituted` | Stored device absent, another on the same backend used |
| `unavailableBackend` | Backend not present; **nothing** is selected |
| `unconfigured` | Session carried no device configuration |

## Fail-closed rules

- **A session never migrates to a different audio backend.** If no device
  is present on the stored backend, the result is `unavailableBackend`
  with an empty resolved configuration. A caller that ignores the status
  still cannot open the wrong subsystem.
- The same holds for `unconfigured`: nothing is selected, so a session
  without a stored device does not acquire arbitrary hardware.
- A missing **input** device does not block restoration — playback
  continues without capture, and the substitution is reported. A missing
  **output** device falls back to the backend default, also reported.
- A stored zero means "ask the backend", so adopting a value for it is not
  counted as an adjustment: only a value the session actually asked for
  and could not get is reported as renegotiated.
- Inventory order is the caller's preference order; the first entry on a
  backend is that backend's default.
- Sample-rate renegotiation picks the nearest supported rate, preferring
  the higher rate on an exact tie, so the choice is deterministic rather
  than dependent on enumeration order.
- Every difference from the stored configuration is accumulated into a
  human-readable `reason`, empty only when the status is `restored`.

## Local Release result

Source commit: `5dd0b2d` (working tree, device resolver applied)

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.
Windows-only per the accepted R-13 position; no timing figure is claimed
here in any case, as this is a correctness result.

```text
Device resolution: devices=3, restored=1, adjusted=1, substituted=2,
unavailable_backends=1, unconfigured=1, rate_ties_resolved=1,
reserialized=1
```

The fixture enumerates three devices across two backends and covers: an
exact restore; simultaneous rate, buffer, and channel renegotiation; an
equidistant sample rate resolving to the higher option; a retired output
device; a retired input device; a session recorded on a backend absent
from this machine; and an unconfigured session. A resolved configuration
is written back into a document and re-serialized, so restoration cannot
produce a device record that fails schema validation.

Full local suite passes:

```text
100% tests passed out of 6
```

## CI and sanitizer results

GitHub Actions:
[`30356879372`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30356879372)

All five jobs passed: Windows/macOS/Ubuntu build, `ctest`, and
`gradle check`, plus ASan/UBSan and TSan with no diagnostics.

The macOS session suite ran in 0.70 s, consistent with the 0.79 s of the
previous run against the 500 ms autosave window. The pacing correction in
[`AUTOSAVE_CHECKPOINT_COMPACTION`](AUTOSAVE_CHECKPOINT_COMPACTION_2026-07-28.md)
is therefore stable across runs rather than a single lucky pass.

## Evidence boundary

This proves the restoration decision rules, their fail-closed behavior on
an absent backend, deterministic sample-rate renegotiation, and that a
resolved configuration is round-trippable.

It does not prove:

- **that any audio device actually opens.** No backend consumes a resolved
  configuration yet. The WASAPI, Core Audio, and JACK probes still
  configure themselves independently, so this layer decides what *should*
  be opened and nothing acts on it;
- that a real enumeration populates `AvailableAudioDevice` correctly — the
  inventory is supplied by tests, and no backend enumeration code exists
  to produce one;
- behavior on device hot-plug, device loss during playback, or exclusive
  mode contention;
- anything about macOS or Linux devices beyond the shared decision logic
  (R-13);
- plugin state restoration, which remains stored but unconsumed and is
  now owned by P0-013, since consuming a state blob requires a running
  plugin host.
