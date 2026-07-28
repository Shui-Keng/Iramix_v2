# Candidate Dependency Inventory

No dependency is approved for production merely by appearing here. Every entry
requires a pinned version, license record, security owner, update policy, and
three-OS build proof.

| Area | Candidate | Current purpose | Decision |
|---|---|---|---|
| UI runtime | Eclipse Temurin 21.0.11+10 | Java desktop runtime | Pinned |
| Java build | Gradle 9.6.1 | UI build and packaging | Pinned |
| Rendering | Skiko 0.150.1 | Java Skia/window integration | Pinned; Windows spike in progress |
| Build | CMake + Ninja | Iramix build graph | Accepted |
| Unit tests | To be selected | Test framework | Open |
| Audio files | To be selected | WAV/AIFF/FLAC read/write | Open |
| Sample-rate conversion | To be selected | Boundary conversion | Open |
| FFT | To be selected | Spectral analysis and DSP | Open |
| Serialization | Deterministic binary schema-v3 spike implemented; SQLite plus schema layer remains a production candidate | Session persistence | Open |
| Windows audio | Steinberg ASIO SDK 2.3 | Optional external Phase 0 host probe | Proprietary selected; signing pending |
| Linux audio | System JACK client API | Native Phase 0 callback probe | Spike-only; version/license pin pending |
| Plugin API | CLAP | Plugin hosting | Planned |
| Plugin API | VST3 SDK | Plugin hosting | Planned |
| Plugin API | Apple Audio Unit | macOS plugin hosting | Planned |

## Dependency policy

- No GPL dependency may enter a closed-source product path without explicit
  licensing and legal approval.
- Audio callback code may not call a dependency unless its real-time behavior is
  understood and tested.
- Dependencies using a different build system are wrapped as immutable,
  reproducible artifacts.
- Every dependency receives automated license and vulnerability inventory.
- Updating Skiko/Skia or a plugin SDK requires compatibility tests,
  not only a successful build.

## ASIO SDK spike record

- The SDK is not vendored in this repository. The optional
  `IRAMIX_ASIO_SDK_PATH` CMake cache path must point to a separately supplied
  checkout.
- Steinberg has offered ASIO under a dual proprietary/GPLv3 model since
  October 2025. Iramix has selected the free proprietary license because the
  planned commercial product is closed-source; this path has no source-code
  disclosure obligation. GPLv3 is explicitly not the selected path.
- Official distribution requires an ASIO developer license agreement with
  Steinberg through the Developer Portal. That agreement has not been signed,
  so builds containing the ASIO SDK remain experimental/development-only until
  signing is complete.
- Technical work and Phase 0 measurement may continue while signing is
  pending. The signing task gates only a distributable public release build.
- Use of the ASIO name or logo is optional but governed separately by
  Steinberg's trademark and usage rules. The default Iramix policy is silent
  protocol support without ASIO branding or logo until branding is
  deliberately approved.
- Primary references:
  [Steinberg dual-license announcement](https://ocl-steinberg-live.steinberg.net/_storage/asset/808575/storage/master/Press%20Release%20-%202025-10-15%20-%20OBS%20Partnership-%20EN.pdf)
  and [Steinberg Developer Portal](https://www.steinberg.net/developers/).
