# Phase 1 Backlog — 2026-07-29

Derived from [`EXIT_REVIEW.md`](EXIT_REVIEW.md). Every item traces to a
validated spike, an open risk, or an unmet Phase 0 output.

## How to read the sizing

Sizes are **structural**, not calendar: they describe how much of the
system an item touches, not how long it takes. This project has no
recorded team, so any day or week figure here would be invented.

- **S** — one layer, no schema or protocol change.
- **M** — one layer plus its tests and a result document, or a change
  behind an existing boundary.
- **L** — crosses a process, schema, or protocol boundary, or needs new
  evidence on hardware the project does not have.

**Owners are unassigned because there is only one.** As of 2026-07-29 the
team is one developer working with an AI assistant, so every item below
has the same owner and naming them would add nothing. That is not a
scheduling detail — it is R-01, priority 25, still open. Read the sizes
as relative effort within a single-person queue, never as work that can
run in parallel.

## P1-A — Blockers on committing to Phase 1 at all

These are not engineering tasks. They are the reason Phase 0 is not
complete, and they gate scope decisions rather than code.

| ID | Item | Size | Traces to |
|---|---|---|---|
| ~~P1-A1~~ | ~~Interview three composers and three sound designers; rank the top ten workflow problems~~ — **withdrawn 2026-07-29**: no target users are reachable, accepted as R-16 | — | P0-009, required output 1 |
| P1-A2 | Build low-fidelity interaction prototypes for arrangement, mixer, launcher, and device panel | M | Required output 2 |
| P1-A3 | ~~Record actual team size~~ — **done 2026-07-29: one developer plus an AI assistant.** What remains is the decision it forces: cut `V1_SCOPE.md`'s 34 must-ship items to what one developer can deliver, or explicitly accept the overrun | S | R-01, priority 25, still fails the escalation rule |
| P1-A4 | Declare a v1 widget-set limit and start measuring delivery velocity against it | S | R-10, priority 16, fails the escalation rule |

P1-A2 survives A1's withdrawal on its own merits. It was originally
paired with the interviews, but the prototypes de-risk the renderer, the
input model, and the widget-set question in R-10, and none of that needs
an interviewee. What it no longer delivers is validation: without users
the prototypes test whether the *toolkit* can express the workspace, not
whether the workspace is the right one.

A3 and A4 are the only remaining items in this group that can close a
priority-16+ risk, which makes them cheap and disproportionately
valuable.

## P1-B — Distribution gates

Nothing shippable can be built until these close.

| ID | Item | Size | Traces to |
|---|---|---|---|
| P1-B1 | Skiko native attribution — **partially done 2026-07-29**: `NOTICE.md` carries the verified components, and inspecting the artifact found at least seven third-party components where this entry had assumed two. Remaining: obtain Skiko's upstream third-party manifest, and scan the four target jars that were never fetched | M | L-1, R-14 |
| P1-B2 | Verify the JACK client library licence — **instrumented 2026-07-29**: the Ubuntu CI job now prints the Debian copyright `License:` lines, `pkg-config --libs jack`, and the shipped `.so`/`.a` files. Remaining: read the first log and paste the actual SPDX identifiers into `DEPENDENCIES.md` | S | L-2 |
| ~~P1-B3~~ | ~~Pin CI actions to immutable SHAs~~ — **done 2026-07-29**: all four `uses:` entries pinned to full commit SHAs with version comments | S | L-3, closed |
| P1-B4 | Resolve the two coexisting `org.jetbrains:annotations` versions | S | L-4 |
| P1-B5 | Sign the Steinberg ASIO developer agreement before ASIO enters a public build | S | R-09 |
| P1-B6 | Automate the vulnerability inventory | M | P0-010 |

## P1-C — Turning validated spikes into product

Each of these has a spike behind it that proved the approach works. The
work is making it a feature, not proving it again.

| ID | Item | Size | Traces to |
|---|---|---|---|
| P1-C1 | Session-editing API for plugin state: give `SessionController`/`JournaledSession` a caller that invokes the capture-into-autosave path during real editing | M | P0-013, R-12 — the transport exists and nothing calls it |
| P1-C2 | Application-driven font fallback, since `TextLine.make` does not consult the font manager | L | R-07, the raster spike's adverse finding |
| P1-C3 | Generic plugin editor fallback, required rather than contingent because native Wayland has no cross-client reparenting | L | R-04 |
| P1-C4 | Arrangement, mixer, launcher, and device panel as real UI on the validated renderer | L | Depends on P1-A2 |
| P1-C5 | Multicore graph scheduling on top of the stable single coordinator | L | R-05, priority 15, still only "stabilize first" |
| P1-C6 | Real HiDPI surfaces, IME, accessibility, and focus behavior | L | R-07, entirely untouched |

## P1-D — Evidence that needs hardware the project lacks

Listed so they are not mistaken for engineering backlog. Each needs a
purchase or a borrow before it becomes a task.

| ID | Item | Size | Traces to |
|---|---|---|---|
| P1-D1 | Core Audio and JACK callback probes and soaks on target hardware | L | R-13 |
| P1-D2 | 64-frame buffer validation on an interface with a native ASIO driver | L | R-15 |
| P1-D3 | Real Linux GPU backend, and Wayland plugin editor embedding | L | R-03, R-04 |
| P1-D4 | Cross-monitor and mixed-DPI recovery, on a machine with two displays | M | R-03 |
| P1-D5 | Literal OS sleep/wake and device loss/TDR, replacing the accepted proxies | M | R-03 |

## P1-E — Smaller known-open engineering

| ID | Item | Size | Traces to |
|---|---|---|---|
| P1-E1 | Repeated topology churn sustained against a live device callback, not just under sanitizers | M | The WASAPI soak measured one plan swap |
| P1-E2 | PDC correctness after routing changes on the full plugin case, not the graph-level fixture | M | ADR-0003 criterion 4, the one partial |
| P1-E3 | Run-to-run variance for the callback-delivery deficit, if the WASAPI/ASIO difference is ever to be claimed as a magnitude | S | Both soak documents refuse to claim it from one run each |
| P1-E4 | Decode CLAP metadata once headers are available | S | P0-013 |
| P1-E5 | Acoustic loopback or driver xrun evidence, so "zero dropouts" stops meaning "zero the probe can see" | M | Both soak documents |

## Sequencing note

P1-B is independent and can run at any time. P1-D cannot start at all
without hardware. P1-C1, P1-E1, and P1-E2 are the items where the
existing evidence is closest to running out, so they are the natural
first engineering work.

P1-A no longer gates scope the way it was written to. With A1 withdrawn
under R-16, nothing in the backlog can validate the v1 scope, so v1
contents will be committed on assumption. That is the accepted position,
not an oversight — but it means P1-C4 (the real workspace UI) is the
largest item in this backlog and the one built on the least evidence.
Doing P1-A2 first is the cheapest available hedge.
