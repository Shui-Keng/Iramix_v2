# Phase 0 Audio Callback Probe Runbook

This runbook produces comparable initial and Week 2 exit evidence on Windows,
macOS, and Linux. All published measurements must use a Release build, a real
audio endpoint, 48 kHz, and separate 64/128/256-frame configurations.

## Common evidence requirements

Record beside every output log:

- OS version, CPU, memory, audio interface, and driver/server version;
- power mode and whether the machine was on battery;
- build type and source revision;
- requested and actual sample rate and buffer size;
- p50/p95/p99/maximum callback duration;
- expected and observed callback counts;
- late-wakeup count and ten equal-duration time buckets;
- driver xrun/resync signals where the backend exposes them;
- allocation, deallocation, and tracked blocking-lock counters;
- any unsupported buffer size exactly as reported.

The ten late-wakeup buckets divide each measured configuration into consecutive
10% windows. They allow a later report to distinguish clustered startup/end
behavior from events spread through the soak without storing an unbounded
timestamp list in real-time memory.

## Initial screening duration

Use 200 seconds per buffer:

```text
3 buffers × 200 seconds = 600 seconds = 10 minutes per operating system
```

This is screening evidence only. It does not replace the two-hour Week 2 exit
soak.

## macOS — Core Audio

The probe uses the default output device's native Core Audio device I/O
callback. It temporarily requests 48 kHz and each exact buffer size, then
restores the original nominal sample rate and buffer size before exit.

```sh
cmake --preset release
cmake --build --preset release
mkdir -p build/audio-probe
./build/release/iramix_audio_probe --seconds-per-buffer 200 \
  | tee build/audio-probe/coreaudio-initial.txt
```

If a buffer size is rejected, retain the error and device details. Do not
substitute a nearby size or report an unsupported configuration as measured.

## Linux — JACK

Install the JACK development package before configuring. On Debian/Ubuntu:

```sh
sudo apt-get install libjack-jackd2-dev
```

Run a JACK server at 48 kHz against the real reference audio interface before
starting the probe. Do not use the dummy backend for performance acceptance.
The probe requests each exact server buffer size, registers two output ports,
connects them to physical playback ports when available, and restores the
previous JACK buffer size after each configuration.

```sh
cmake --preset release
cmake --build --preset release
mkdir -p build/audio-probe
./build/release/iramix_audio_probe --seconds-per-buffer 200 \
  | tee build/audio-probe/jack-initial.txt
```

`physical_connections=0` is valid for callback mechanics but not sufficient
for final hardware-backed acceptance. Any `xruns`, frame mismatches, missing
cadence, or JACK server shutdown must remain visible in the report.

## Week 2 two-hour exit soak

After all three operating systems have accepted initial logs, run 2,400
seconds per buffer:

```text
3 buffers × 2,400 seconds = 7,200 seconds = 2 hours per operating system
```

The unit is 2,400 seconds **per configuration**, not the 7,200-second
total. A buffer size that no available device can deliver is skipped and
recorded as unvalidated rather than soaked; on the Windows reference
machine that is 64 frames (R-15), so its soak is 4,800 seconds.

Run the machine idle. Nothing else may be started for the duration —
this hardware's tail has already been measured widening under concurrent
load, so a contaminated log must be discarded rather than reported.

Commands:

```powershell
# Windows, named native hardware ASIO driver preferred
.\build\windows-msvc\Release\iramix_asio_probe.exe `
  --driver "Driver Name" `
  --buffers 128,256 `
  --seconds-per-buffer 2400
```

`--buffers` takes a comma-separated subset of `64,128,256` and refuses
anything else, so a typo cannot silently soak the wrong period for forty
minutes. The chosen list is echoed as `requested_buffers=` in the header
line. Omit the flag to run all three.

```sh
# macOS Core Audio or Linux JACK
./build/release/iramix_audio_probe --seconds-per-buffer 2400
```

## Execution schedule

| Target slot | Activity | Gate |
|---|---|---|
| 2026-07-28 | macOS initial 10-minute Core Audio run | Reference Mac and output device available |
| 2026-07-29 | Linux initial 10-minute JACK run | JACK 48 kHz on reference interface |
| 2026-07-30 | Compare all initial logs and freeze probe revision | Windows/macOS/Linux logs present |
| 2026-07-29 | Windows ASIO soak — **done** | Ran at 128 and 256 frames for 2,400 s each on an idle machine; 64 skipped per R-15. See [`results/AUDIO_CALLBACK_SOAK_WINDOWS_2026-07-29.md`](results/AUDIO_CALLBACK_SOAK_WINDOWS_2026-07-29.md) |
| 2026-08-01 | macOS two-hour Core Audio run | Initial Core Audio configurations accepted |
| 2026-08-02 | Linux two-hour JACK run | Physical connections active and initial xrun behavior accepted |
| 2026-08-03 | Week 2 evidence review | All raw logs, machine manifests, and exception notes present |

These are engineering slots, not claims that the runs occurred. A missed
prerequisite moves the slot and keeps P0-008 open.
