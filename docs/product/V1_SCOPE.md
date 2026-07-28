# Version 1 Scope

This document is a scope contract. Moving an item into v1 requires removing an
item of comparable cost or explicitly changing the schedule.

## Must ship

### Core session

- Audio, instrument, group, effect-return, and master tracks
- Linear arrangement
- Tempo, meter, marker, and loop maps
- Audio and MIDI recording
- Non-destructive clip editing
- Routing, sends, solo, mute, pan, and gain
- Sample-accurate automation
- Undo, redo, autosave, recovery, and project migration
- Freeze, bounce, render in place, mixdown, and stem export

### Composition

- Piano roll
- MPE and per-note expression
- Retrospective MIDI capture
- Comping and take lanes
- Articulation maps
- Track templates
- Scene launcher synchronized with the arrangement

### Sound design

- Universal parameter modulation for internal devices
- Macro, LFO, envelope follower, step, random, and key-track modulators
- Serial, parallel, layer, and multiband device containers
- Sampler and wavetable instrument
- EQ, dynamics, saturation, delay, and reverb essentials
- Fast resampling and audio-to-sampler workflow
- Searchable preset and sample browser

### Plugin and platform

- CLAP and VST3 on all supported operating systems
- Audio Unit hosting on macOS
- Out-of-process scanning
- Plugin crash isolation and recovery
- Windows x64, macOS Apple Silicon, and Linux x64 release builds

## Should ship if quality allows

- Basic spectral selection and repair
- Chord and scale assistance
- Project notes
- Controller mapping
- Generic plugin editor
- Basic accessibility support beyond keyboard navigation

## Explicitly after v1

- Full notation editor
- Video scoring suite
- ARA
- AAX
- Dolby Atmos authoring
- Cloud projects and real-time collaboration
- Public scripting API
- Mobile applications
- Generative AI features

## v1 release gate

v1 is not defined by feature count. It ships only when:

- all must-ship workflows have automated project round-trip tests;
- the reference sessions meet the real-time and UI budgets;
- project recovery and migration drills pass;
- critical plugin failures stay isolated;
- accessibility and installer requirements have a signed-off baseline;
- no known issue can corrupt project or media data.

