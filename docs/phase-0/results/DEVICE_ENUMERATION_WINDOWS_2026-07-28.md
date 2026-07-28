# WASAPI Device Enumeration and Session Restore — 2026-07-28

Status: P0-012 device enumeration and session-driven device selection
verified on Windows hardware; the restored stream opens but the probe
declines to measure it (see below). Week 6 remains open.

## Scope

[`DEVICE_CONFIGURATION_RESTORATION`](DEVICE_CONFIGURATION_RESTORATION_2026-07-28.md)
decided what a session *should* open, but nothing produced a real
inventory and no backend consumed the decision. This closes both:

- `enumerateDevices()` produces `AvailableAudioDevice` records from actual
  WASAPI endpoints;
- `--capture-device PROJECT` writes a session whose device record names the
  hardware currently in use;
- `--restore-device PROJECT` reopens that session, enumerates, resolves,
  and opens **the device the resolver selected** rather than the default
  endpoint.

Enumeration deliberately reports only what the endpoint actually answers:

- device ID and friendly name from `IMMDevice`/`IPropertyStore`;
- channel count from the mix format;
- buffer bounds from `IAudioClient3::GetSharedModeEnginePeriod`;
- sample rates as the shared-mode mix rate plus every candidate the device
  accepts under `IsFormatSupported` in exclusive mode.

Nothing is inferred from a device name or class. An endpoint that will not
activate, or that reports no usable rate or period, is left out of the
inventory rather than advertised as usable — the resolver would otherwise
select something that cannot be opened.

Core Audio and JACK report `supported=false` rather than an empty
inventory. That distinction matters: an empty inventory is
indistinguishable from a machine with no audio hardware, and the resolver
would then report a missing backend for a session that is perfectly
restorable on that platform.

## Local Windows result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.
Windows-only per accepted risk R-13.

Real enumeration, eight active endpoints:

```text
device_inventory count=8
device id="{0.0.0.00000000}.{abb542a8-...}" name="Headphones (QuietBuds 3)"
  inputs=0 outputs=2 buffer_frames=480..480 rates=48000
device id="{0.0.0.00000000}.{2a56d4f2-...}" name="CABLE Input (VB-Audio Virtual Cable)"
  inputs=0 outputs=2 buffer_frames=128..480 rates=48000
device id="{0.0.0.00000000}.{4095b5ff-...}" name="CABLE In 16ch (VB-Audio Virtual Cable)"
  inputs=0 outputs=2 buffer_frames=128..480 rates=48000
device id="{0.0.0.00000000}.{618d2f42-...}" name="Speaker (Realtek(R) Audio)"
  inputs=0 outputs=2 buffer_frames=512..512 rates=48000
device id="{0.0.1.00000000}.{4b2903fe-...}" name="Stereo Mix (Realtek(R) Audio)"
  inputs=2 outputs=0 buffer_frames=480..480 rates=48000
device id="{0.0.1.00000000}.{688982a0-...}" name="Microphone Array (Realtek(R) Audio)"
  inputs=2 outputs=0 buffer_frames=480..480 rates=48000
device id="{0.0.1.00000000}.{d670f117-...}" name="CABLE Output (VB-Audio Virtual Cable)"
  inputs=2 outputs=0 buffer_frames=480..480 rates=48000
device id="{0.0.1.00000000}.{f1624ddc-...}" name="Headset (QuietBuds 3)"
  inputs=1 outputs=0 buffer_frames=160..160 rates=16000
```

The inventory is not uniform, which is the point: buffer bounds differ per
endpoint (480..480, 128..480, 512..512, 160..160) and one endpoint runs at
16 kHz mono. These are exactly the constraints the resolver exists to
renegotiate against, and none of them were representable in the synthetic
test fixture.

Capture and restore round trip:

```text
device_capture device="{0.0.0.00000000}.{abb542a8-...}"
name="Headphones (QuietBuds 3)" rate=48000 buffer=480 outputs=2

device_restore backend=WASAPI enumerated=8
stored_device="{0.0.0.00000000}.{abb542a8-...}" stored_rate=48000
stored_buffer=480 status=restored
resolved_device="{0.0.0.00000000}.{abb542a8-...}" resolved_rate=48000
resolved_buffer=480 reason=""
buffer=480 status=unsupported_actual_buffer_size backend=WASAPI_shared
stream_buffer=1056 period_min=480 period_max=480 period_fundamental=480
```

The session's stored device was found among eight endpoints, resolved
exactly (`status=restored`, empty reason), opened by ID, and the audio
client initialized in shared mode at the session's stored 480-frame
period. **That is the P0-012 claim, and it holds.**

## Two latent defects this exposed

Both live in the probe's shared-mode path, which had never executed on
this machine: the probe requests 64, 128, and 256 frames, none of which
match this endpoint's 480-frame period, so every configuration had always
fallen through to exclusive mode. Restoration was the first thing to ask
for the device's own native period.

**1. `AUDCLNT_STREAMFLAGS_NOPERSIST` is invalid for
`InitializeSharedAudioStream`** — fixed here. The call failed with
`AUDCLNT_E_INVALID_STREAM_FLAG` (`0x88890021`). This was not guessed: a
throwaway diagnostic tried the flag combinations directly against this
endpoint.

```text
flags=EVENTCALLBACK|NOPERSIST    period=480 hr=0x88890021 FAIL
flags=EVENTCALLBACK              period=480 hr=0x00000000 OK
flags=NOPERSIST                  period=480 hr=0x88890021 FAIL
flags=none                       period=480 hr=0x00000000 OK
```

The exclusive-mode `IAudioClient::Initialize` call does accept the flag and
is unchanged. The exclusive path still measures identically
(`buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256`).

**2. The buffer-size assertion is exclusive-mode-shaped** — not fixed
here. After a successful shared-mode init the probe requires
`GetBufferSize() == requestedFrames`. That holds in exclusive mode, where
the buffer is the period, but a shared-mode stream returns the ring size
(1056 for a 480-frame period), so the probe reports
`unsupported_actual_buffer_size` and declines to measure.

This is left alone deliberately. Correcting it means changing what the
probe measures per callback in shared mode, which is audio-callback
measurement semantics and belongs to **P0-008**, not to session
restoration. Fixing it hastily here would risk publishing latency figures
whose meaning had quietly changed.

## CI and sanitizer results

GitHub Actions:
[`30359826999`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30359826999)

All five jobs passed. Getting there took one more Windows-only fix, worth
recording because it is a second instance of the same class of problem as
the macOS `uintmax_t` failure: **a construct that compiles on the one
toolchain available locally and not on the one CI uses.**

`functiondiscoverykeys_devpkey.h` sat before `windows.h` and
`mmdeviceapi.h` in alphabetical include order. It uses
`DEFINE_PROPERTYKEY` without defining it, so MSVC failed where MinGW
tolerated the ordering.

Reordering the includes would have fixed the compile error, but reading
the SDK headers on this machine — which happens to have the same
10.0.26100.0 SDK the CI job uses — showed a second problem behind it:
without `INITGUID` the macro only *declares* the key, leaving the symbol
to whichever import library a toolchain supplies. Whether MSVC's default
libraries provide it could not be checked from here, so reordering risked
moving the failure from compile time to link time.

`PKEY_Device_FriendlyName` is therefore defined locally from the published
SDK value, which removes both toolchain-dependent assumptions rather than
one. Confirmed on CI: MSVC compiles and links it, and friendly names still
resolve locally.

## Evidence boundary

This proves real WASAPI enumeration, that a session's stored device record
selects and opens that specific endpoint out of eight, and that the
resolver's output is directly openable.

It does not prove:

- **that audio was rendered through the restored device.** The stream
  initialized; the probe then declined to measure it for the reason above.
  No callback ran on the restored configuration;
- any macOS or Linux enumeration — both report `supported=false`, and
  neither can be implemented or tested here (R-13);
- enumeration behavior on device hot-plug, device loss, or exclusive-mode
  contention with another process;
- that the reported exclusive-mode rate list is complete — it is the
  subset of a fixed candidate list the endpoint accepted, not an
  exhaustive capability query;
- restoration of a stored **input** device against real hardware; the
  capture path records output only;
- anything about sessions whose stored rate differs from the probe's fixed
  48 kHz workload. That case is refused explicitly rather than run at the
  wrong rate.
