# ADR-0001: C++20 Engine, Java 21 UI, CMake, and Gradle

Status: Accepted  
Date: 2026-07-27

## Context

Iramix targets Windows, macOS, and Linux without JUCE. The real-time engine
needs native platform APIs and predictable execution. The UI needs rapid,
cross-platform development and GPU-accelerated rendering.

## Decision

- Engine and native-host source uses C++20.
- Desktop UI source uses Java 21.
- CMake describes C++ targets.
- Gradle describes Java targets and packages the desktop runtime.
- Ninja is the default local and CI generator.
- Platform-specific signing and packaging may use native tooling invoked from
  CMake or CI.
- Java and native dependencies are pinned and verified independently.

## Consequences

- Supported compilers must have a reliable C++20 implementation.
- Release packaging includes a controlled Java runtime.
- The Java and C++ processes require a versioned compatibility protocol.
- The project does not adopt C++ modules until all supported toolchains and IDEs
  demonstrate stable behavior.
