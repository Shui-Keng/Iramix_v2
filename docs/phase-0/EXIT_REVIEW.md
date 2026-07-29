# Phase 0 Exit Review — 2026-07-29

This review synthesizes 43 result documents, the risk register, and the
task board. It introduces **no new measurement** and inherits every
evidence boundary those documents declare.

## Verdict

Phase 0's **technical** validation is done, and the architecture it was
built to test is safe to commit to. Phase 0 **as specified** is not done:
three of its ten required outputs depend on user research that has never
started, and two of the seven risks at priority 16 or above have neither
measured evidence nor an accepted fallback.

Recommended decision: **begin Phase 1 engineering on the validated
architecture, and do not mark Phase 0 complete.** The two are separable.
Nothing engineering can measure is still blocking; what is missing is
product validation and a capacity answer, and neither is closed by
writing more code.

The distinction matters because the gap is not where a reader would
guess. The engine, persistence, and plugin layers carry more evidence
than Phase 0 asked for. The product brief this was all built to serve has
never been shown to a composer.

## Required outputs

The ten outputs Phase 0 declared, each judged against evidence rather
than activity.

| # | Required output | Status |
|---|---|---|
| 1 | Validated product brief and v1 scope | **Not met.** Drafts exist (`docs/product/`), but "validated" meant interviews with three composers and three sound designers. P0-009 never started. The brief itself says these questions require interviews and prototype tests during Phase 0 |
| 2 | Interaction prototypes for arrangement, mixer, launcher, device panel | **Not met.** None exist. The raster spike renders a dense arrangement *scene* as a rendering fixture; it is not an interaction prototype and was never presented as one |
| 3 | Audio callback spike on all three operating systems | **Windows only.** Core Audio and JACK probes are written but have never run on target hardware (R-13, accepted) |
| 4 | Skia rendering spike on all three operating systems | **Met**, with declared limits: raster is bit-identical and guarded on all three CI OSes; GPU backend identification covers Direct3D, Metal, and — because hosted Linux has no display — Skiko's software fallback under Xvfb. No real Linux GPU has ever run it |
| 5 | Immutable audio-graph spike | **Met**, and exceeded. See ADR-0003 below |
| 6 | Plugin process-isolation feasibility spike | **Met**, and exceeded: real VST3 plugins hosted in the bridge child, crash and hang recovery against real plugins, real parameters through `IEditController`, MIDI to a real instrument |
| 7 | Benchmark results on declared reference machines | **Windows only**, by decision (R-13). Hosted CI is used for portability and sanitizers, never for timing |
| 8 | Dependency and licensing inventory | **Met**, with four obligations open (L-1 to L-4) that gate any distributable build |
| 9 | Architecture decisions and risk register | **Met** |
| 10 | Phase 1 backlog with estimates and owners | **Partially met.** [`PHASE_1_BACKLOG.md`](PHASE_1_BACKLOG.md) delivers the backlog with structural sizing. **Owners are unassigned** — this project has no recorded team, which is itself the subject of R-01 |

Six met, one partial, three not met. All three unmet outputs are product
or user research; none is blocked by engineering.

## Risks at priority 16 and above

The register's own escalation rule: any risk at priority 16 or above must
have measured evidence or an accepted fallback before Phase 1 begins.

| Risk | Prio | Rule satisfied? |
|---|---:|---|
| R-01 Scope exceeds available team capacity | 25 | **No.** The response is "enforce the v1 scope contract and validate team size". Neither happened. There is no recorded team size, so the risk cannot even be evaluated |
| R-02 Plugin crash or hang destabilizes audio | 20 | Yes — measured against real third-party plugins, both fault kinds |
| R-13 No macOS or Linux hardware | 20 | Yes — explicitly accepted, with the reasoning recorded |
| R-15 No native hardware audio interface | 20 | Yes — explicitly accepted; the 64-frame budget is recorded as unvalidated rather than met or failed |
| R-03 Skiko/Skia GPU behavior differs by OS | 16 | Yes — partially measured, with the remaining gaps named as accepted proxies rather than claimed |
| R-04 Linux plugin editors fail under Wayland | 16 | Yes — cannot be closed without Linux hardware; the generic-editor fallback is recorded as a requirement, not a contingency |
| R-10 Custom UI consumes capacity needed by audio engine | 16 | **No.** The response is "limit widget set and measure delivery velocity at week 4". No widget-set limit is recorded and no velocity was measured |

**Two of seven fail the project's own escalation rule, and both are
capacity risks rather than technical ones.** R-01 and R-10 are the same
question asked twice: can the available team build this scope, and is the
custom UI eating the engine's budget? Phase 0 spent eight weeks answering
whether the architecture works and never answered whether it can be
staffed.

This is not a reason to delay Phase 1 engineering. It is a reason not to
declare Phase 0 complete, and to treat scope decisions as unvalidated
until someone answers them.

## ADR-0003 decision

ADR-0003 states its own exit condition: *"This evidence does not replace
the two-hour callback soak or target-hardware backend evidence, so this
ADR remains proposed."* One of those two has since been delivered.

Its five validation criteria:

| Criterion | Status |
|---|---|
| Allocation instrumentation reports zero allocations in the callback | **Met.** 1,799,407 measured callbacks across two independent Windows backends, zero allocations, sustained over 80 minutes of soak |
| Lock instrumentation reports no blocking synchronization | **Met.** Same runs, zero tracked blocking locks |
| Repeated graph replacement produces no use-after-free under sanitizers | **Met.** 5,001 publications under ASan/UBSan and TSan |
| Latency compensation remains correct after routing changes | **Partial.** PDC is verified at graph level (`pdc_peak=2`, `pdc_peak_frame=3`) on a fixture the result document itself calls "an initial graph-level fixture, not yet the full plugin" case |
| The engine survives command-queue saturation with explicit diagnostics | **Met.** Saturation, lateness, and overflow are counted rather than silently dropped, verified through a live device callback |

Recommendation: **accept ADR-0003 for Windows Tier-1, recording two
qualifications** — PDC after routing changes is verified only on a
graph-level fixture, and macOS/Linux backends remain unmeasured (R-13).

The decision is worth taking rather than deferring. The immutable-plan
design is now load-bearing for the persistence, plugin, and device layers
built on top of it; leaving it "proposed" while shipping Phase 1 against
it would be a fiction. But accepting it is a project decision, not a
measurement, so it is recorded here as a recommendation for sign-off
rather than applied unilaterally.

## What Phase 0 proved

**Real-time engine.** Editable session and executable graph are separate
models; the compiler emits immutable plans published atomically; plans
co-own their nodes so topology changes cannot invalidate what the
callback is using; reclamation is acknowledgement-based and off-thread.
Zero allocations, zero locks, zero deadline misses across 1.8 million
callbacks on two backends.

**Persistence.** Write-ahead journal with monotonic undo/redo, fixed-window
autosave on an injected clock, checksummed envelopes with atomic replace,
revisioned backup rotation, fail-closed automatic restore, and schema v4
carrying media references, MIDI, device configuration, and plugin state.
Verified locally and on three-OS CI including sanitizers.

**Plugin isolation.** A bridge child hosts real VST3 plugins driven only
through `processBlock`/`restoreState`/`captureState`. The host survives
child crash and hang, degrading to counted silence. Bridge overhead p99
19.1 µs. 201 real installed plugins scanned out of process. Real
parameters through `IEditController`; MIDI to a real instrument. A third
reason for isolation was found by accident: a plugin writes to stdout,
which is the transport the Java/C++ boundary uses.

**Rendering.** Skia raster output is bit-identical across Windows x86-64,
macOS arm64, and Linux x86-64, and the comparison guards every CI leg.
The adverse finding is as valuable: `TextLine.make` does not consult the
font manager, so font fallback is application work Iramix must implement,
and measured text extents must never be hard-coded.

## What Phase 0 did not prove

Grouped by cause, because the causes are not equivalent.

**Blocked by absent hardware, accepted (R-13, R-15).** macOS and Linux
performance of any kind; Core Audio and JACK callbacks; a real Linux GPU
backend; Linux plugin editor embedding under Wayland; the 64-frame buffer
budget.

**Delivered as an explicit proxy rather than the real event.** Literal OS
sleep/wake (context recreation stands in); device loss and TDR (a managed
`RenderException` stands in); cross-monitor recovery (every available
environment has one `GraphicsDevice`). Each is labelled in its own result
document and none is claimed as the real thing.

**Simply not attempted.** User interviews; interaction prototypes; team
capacity; a widget-set limit; delivery velocity. These are the ones worth
noticing, because no hardware and no proxy is standing between the
project and doing them.

**Known-open engineering, small.** Four licensing obligations (L-1 to
L-4) gating any distributable build; repeated topology churn under a live
device callback; real HiDPI surfaces, IME, accessibility, and focus.

## Evidence boundary of this review

- It measures nothing. Every figure is quoted from a result document and
  carries that document's boundary unchanged.
- "Met" means the declared exit evidence exists, not that the feature is
  production-ready. Phase 0 validates architecture, not products.
- The verdict rests on the register's priority scores, which are
  human judgements that have not been recalibrated since they were set.
- Owner assignment and calendar estimates are absent from the Phase 1
  backlog because the project has no recorded team. Any schedule read
  into it would be invented.
