# Iramix Product Brief

Status: Draft for Phase 0 validation  
Owner: Product and engineering  
Last updated: 2026-07-27

## Product statement

Iramix is a cross-platform DAW for composers and sound designers who want to
move quickly between composition, arrangement, modulation, routing, and
resampling without breaking creative flow.

The product combines:

- a linear arrangement for complete compositions;
- a launcher for nonlinear sketching and live variation;
- universal modulation and composable device containers;
- contextual editing with minimal modal dialogs;
- strong MIDI expression and orchestral articulation workflows;
- resilient plugin hosting and project recovery.

Iramix is not intended to clone the user interface or proprietary behavior of
another DAW. Bitwig Studio and Studio One are reference points for workflow
qualities, not implementation specifications.

## Primary users

### Sound designer

Needs fast routing, modulation, layering, resampling, preset variation, and
clear visualization of signal flow.

Success scenario: creates a layered evolving sound, resamples it, slices the
result, and saves a reusable preset without leaving the main workspace.

### Composer

Needs reliable recording and editing, tempo and meter maps, articulation
management, expressive MIDI, large templates, and fast navigation.

Success scenario: sketches with scenes, commits material to the arrangement,
edits orchestral expression, and exports stems without rebuilding routing.

### Hybrid producer

Needs audio, MIDI, third-party plugins, comping, automation, mixing, and
delivery in one application.

## Product principles

1. **One action should have one obvious home.**
2. **Drag-and-drop is a first-class command surface.**
3. **Modulation is visible, inspectable, and reversible.**
4. **Arrangement and launcher share one musical timeline model.**
5. **Audio continuity is more important than UI continuity.**
6. **A plugin failure must not destroy the session.**
7. **Advanced features remain discoverable without slowing basic work.**
8. **Projects remain openable across releases through explicit migrations.**

## Initial success metrics

- A new user can record, edit, arrange, and export a short composition after a
  guided session of no more than 30 minutes.
- A sound designer can map a modulator to any supported internal parameter in
  three interactions or fewer.
- A plugin crash does not terminate the main process.
- Crash recovery loses no more than five seconds of acknowledged edits.
- The reference low-latency session runs for two hours without an engine-caused
  dropout.

## Open product questions

- Does the first public release need a launcher, or can it enter private alpha
  after the linear workflow is complete?
- Which composer workflow is more valuable first: articulation maps or chord
  assistance?
- Should generic plugin editors expose modulation for every automatable
  parameter?
- What is the minimum Linux distribution support window?
- Which project interchange formats are required for v1?

These questions require interviews and prototype tests during Phase 0.

