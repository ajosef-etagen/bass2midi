# Bass2MIDI — Claude Code Guide

## Project purpose

Bass2MIDI is a macOS-first, real-time, **monophonic electric bass to MIDI** converter, built as an independent JUCE project. [Warf](https://github.com/dferreiramarques/warf) (MIT), a JUCE/VST3 audio-to-MIDI plugin with YIN-based pitch tracking, was the starting idea; it was evaluated and not forked (VST3-only, which MainStage cannot host; fixed 60 Hz window floor that cannot reach E1, nor F1 at 48 kHz and above). No Warf code is included. Its default analysis framing is reproduced from its documented parameters as the `warf-like-yin` measurement reference.

The product goal is not a general audio-to-MIDI system. It is a dependable live-performance tool for a four-string electric bass connected to an audio interface and routed into MainStage. Musical stability is more important than chasing an implausibly low latency number: a slightly later correct note is better than a fast octave error or a false retrigger.

Primary signal path:

```text
Electric bass -> audio interface -> Bass2MIDI -> virtual MIDI output -> MainStage
```

V1 is delivered as a **standalone macOS app** that opens the audio interface itself and publishes a virtual CoreMIDI source named `Bass2MIDI`, which MainStage selects as a MIDI input. MainStage only hosts Audio Units, so a VST3 plugin cannot sit in this path.

## Repository layout

| Path | Contents | Depends on |
| --- | --- | --- |
| `dsp/` | Real-time DSP core (`bass2midi_dsp`): framing, FFT, pitch tracking, attack detection, note state machine, Song Mode follower, `BassToMidiProcessor` (the full chain used by app and evaluation) | C++20 only, no JUCE |
| `song/` | Guitar Pro song files (.gp, .gp5) → tracks, notes, song palette (non-real-time) | `dsp/`, zlib |
| `app/` | Standalone JUCE app: audio device, virtual MIDI port, sender thread, song list, diagnostics UI | JUCE 8, `dsp/`, `song/` |
| `eval/` | Offline evaluation: synthetic corpus, reference setups, `bass2midi_baseline`, `bass2midi_songmode_eval` | `dsp/`, `song/` |
| `tests/` | doctest unit tests (`bass2midi_tests`), generated song fixtures in `tests/data/` | `dsp/`, `eval/`, `song/` |
| `docs/` | Design notes and versioned evaluation results | - |

Build and test (DSP/tests need no JUCE; add `-DBASS2MIDI_BUILD_APP=OFF` to skip the app):

```sh
cmake -S . -B build -G Ninja -DBASS2MIDI_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure
./build/bass2midi_baseline --out docs/baseline/<name>   # full corpus, ~90 s; --quick for a subset
./build/bass2midi_baseline --palette --out docs/baseline/<name>   # Free vs. song-palette study
./build/bass2midi_songmode_eval --out docs/baseline/<name>        # Free vs. Song Mode on synthetic performances
```

## Scope

### V1 goals

- Support one simultaneously sounding note only.
- Optimize for four-string bass in EADG standard tuning.
- Cover approximately E1 (41.20 Hz) through G4 (392.00 Hz); tune the useful operating range from recorded test material.
- Accept fingerstyle and pick playing.
- Produce MIDI Note On, Note Off, and velocity.
- Provide a virtual MIDI output usable by MainStage.
- Detect note attacks quickly, track sustained notes reliably, and release notes predictably.
- Suppress octave mistakes, duplicate triggers, and unstable note changes.
- Make latency, confidence, and decision state observable in diagnostics/tests.

### Explicit non-goals for V1

- Polyphonic tracking, chords, or double-stops.
- Alternate tunings, five/six-string basses, guitar, vocals, or arbitrary audio.
- Slap-specific modeling, harmonic-articulation interpretation, or automatic technique classification.
- Perfect slide, vibrato, legato, or mute transcription.
- Audio effects processing, amp simulation, recording, or a DAW replacement.
- Machine-learning inference on the audio thread.
- Plugin formats (VST3/AU) for V1. The standalone app with a virtual CoreMIDI port is the V1 target; add an Audio Unit only when it offers a concrete workflow benefit.

## Architectural principles

Preserve a separation between real-time DSP/state and user-interface or host integration code. A change must not make note decisions depend on UI timing, logging, memory allocation, locks, filesystem access, or MIDI-device enumeration.

Preferred processing pipeline:

```text
Audio input
  -> DC removal / high-pass conditioning / level normalization
  -> envelope and transient (attack) detection
  -> adaptive analysis-window selection
  -> pitch candidates (McLeod Pitch Method preferred, or modified YIN)
  -> harmonic and octave validation
  -> confidence scoring and temporal smoothing
  -> note quantization with hysteresis/debouncing
  -> MIDI state machine (Note On / velocity / Note Off)
  -> virtual MIDI output / plugin MIDI events
```

### Component boundaries

- **Input conditioning:** Remove DC and subsonic contamination without damaging the E1 fundamental. Keep filter delays documented and measurable.
- **Attack detector:** Emit an onset candidate from energy/envelope changes. It may start or prioritize pitch estimation, but must not emit MIDI alone.
- **Pitch tracker:** Return candidate frequency, clarity/confidence, period estimate, and enough diagnostic data to identify harmonic and octave choices.
- **Window controller:** Select the smallest reliable window based on pitch range, attack state, and recent stable period. Avoid a single fixed large window (the `warf-like-yin` and `full-range-yin` references show its cost).
- **Validator:** Combine pitch confidence, harmonic evidence, continuity with the prior note, and signal level. Explicitly penalize one- and two-octave jumps unless an onset and strong evidence justify them.
- **Note state machine:** Own all MIDI lifecycle decisions. It must guarantee matched Note On/Off pairs, prevent duplicate Note Ons, and define retrigger and legato behavior.
- **MIDI adapter:** Translate decisions into messages for the virtual CoreMIDI source, via a lock-free queue and a dedicated sender thread (never call the MIDI API from the audio callback). Do not put DSP decisions here.
- **Diagnostics:** Capture bounded, lock-free metrics/events suitable for an editor/debug view and offline evaluation. Never print or allocate from the audio callback.

## Real-time constraints

The audio callback is sacred:

- No heap allocation, deallocation, locks, blocking waits, file/network I/O, synchronous logging, Objective-C/UI calls, or device discovery.
- Preallocate buffers/state during initialization or controlled reconfiguration.
- Use fixed-capacity queues/ring buffers for cross-thread diagnostics or MIDI handoff.
- Keep algorithms bounded: no unbounded searches, retry loops, or data-dependent allocation.
- Handle host block sizes and sample rates without assuming 44.1 kHz.
- Make latency explicit: report analysis lookback, hop scheduling, filtering/group delay, decision delay, and host/plugin delay separately where possible.
- Avoid changing parameters that reallocate DSP resources while processing; stage safe configuration swaps off the audio thread.

Use `double` where estimator precision materially benefits low-frequency period measurement; use the existing project/JUCE conventions otherwise. Profile before introducing SIMD or architecture-specific code.

## Latency and quality targets

Measure end-to-end **algorithmic note-on latency** from a defined physical/audio onset to the emitted MIDI Note On. Always report sample rate, buffer size, input level, articulation, and the latency percentile—not only a best-case value.

| Playing range | Target median note-on latency | Upper target (p95) |
| --- | ---: | ---: |
| D/G strings | < 8–10 ms | < 12 ms |
| A string | < 12 ms | < 15 ms |
| low E string | < 15–20 ms | < 25 ms |

These targets are ambitious. Do not claim compliance until measured using the test protocol below. If a lower note needs a longer observation window, favor a correct, stable decision over a speculative early MIDI event.

Quality targets for representative clean input are:

- No unmatched MIDI notes.
- No duplicate Note On for one sustained pluck.
- Near-zero octave errors in the curated clean corpus; track the exact count rather than using vague claims.
- False-trigger and missed-note rates recorded by string, articulation, dynamic range, and sample rate.
- Stable held notes despite normal bass harmonics and decay.

## Pitch-tracking policy

Evaluate both the McLeod Pitch Method (MPM) and a bass-adapted YIN implementation against the same corpus. Measure every candidate against the reference setups; do not choose a tracker based on theory alone.

- Keep the valid period/frequency bounds explicit and sample-rate aware.
- Bias candidate selection toward continuity, but permit a real new note after an onset.
- Validate a suspected fundamental against harmonic structure; do not simply choose the strongest FFT peak.
- Treat octave changes as high-cost transitions. Require stronger evidence than for a nearby semitone change.
- Use short/early windows only for provisional decisions. Confirm or correct them through bounded temporal evidence without generating MIDI chatter.
- Make threshold values named parameters with units and document the intended trade-off. Avoid unexplained magic constants.

## MIDI behavior

- Use a single authoritative active-note state.
- On a new accepted note, send the necessary Note Off before its Note On unless explicit legato behavior is implemented and tested.
- Compute velocity from a calibrated early-attack envelope/energy feature, not from the entire decay.
- Define MIDI channel, note range, velocity curve, and minimum velocity as configurable non-real-time parameters.
- Note Off should use sustained low-level evidence plus release timing/hysteresis; never turn a note off merely because one pitch frame is uncertain.
- Design the state machine now so a later Pitch Bend mode can be added without breaking discrete-note behavior. Pitch Bend itself is deferred until the discrete tracker is verified.

## Optional score-guided recognition

Many live songs have an existing MIDI bass part, chart, or known note set. Bass2MIDI may use that information as a **soft musical prior** to improve recognition, particularly to reject octave errors and implausible note changes. This mode must remain optional: Free mode remains the default for improvisation and unknown material.

Supported guidance levels, in increasing order of specificity:

| Mode | Input | Recognition behavior |
| --- | --- | --- |
| Free | No musical input | Accept any plausible note in the configured EADG range. |
| Key/scale | Key or allowed scale tones | Prefer compatible pitch classes; do not hard-reject a confident outside tone. |
| Song palette | Set of notes found in a song/MIDI file | Increase confidence for known notes and penalize other candidates. |
| Timeline-guided | Timestamped/beat-aligned MIDI notes | Give the expected note or local phrase a time-dependent confidence bonus. |

### Design rules

- Guidance is a prior, not a replacement for audio evidence. Never emit a MIDI note solely because the score expects it.
- Default to a **soft weighting** model, not a hard note gate. A player must be able to play variations, fills, intentional chromatic tones, and transpositions without their notes disappearing.
- Apply the prior after candidate estimation and before the final confidence/hysteresis decision. Keep raw acoustic confidence available for diagnostics.
- Let strong acoustic evidence override a weak or stale score expectation; define and test this override threshold explicitly.
- Keep score parsing, MIDI-file loading, transport handling, and timeline lookup off the audio thread. Publish a precomputed, immutable lookup structure to real-time code.
- Song palette mode should be implemented before timeline-guided mode because it needs no tempo/transport synchronization and already provides substantial octave-error suppression.
- Timeline-guided mode must specify its synchronization source (host transport, MainStage control, internal clock, or external MIDI clock), count-in/start behavior, tempo changes, song-position jumps, loops, and fallback behavior when transport is unavailable.
- Expose a clearly visible bypass/fallback to Free mode. A failed or missing score must never prevent ordinary audio-to-MIDI operation.

### Song Mode (score-triggered notes, opt-in)

Song Mode goes beyond a prior and is a **separate, explicitly enabled operating mode** (see `docs/song-mode.md`). A detected bass attack triggers the song's expected next note at once, before the pitch is known. It exists for the latency gain. Pitch detection cannot beat about one period of the note plus analysis time, whereas an attack can be seen in about 1–3 ms. Rules:

- A MIDI note is only ever emitted in response to an acoustic event of the player: an attack, or a pitch-path decision. Never autoplay the score or advance it on a clock.
- The pitch path always runs. It validates every predicted note afterwards and corrects a contradicted one (Note Off + Note On). The correction rule and its timing are named parameters with tests.
- Predictions are gated by an explicit position confidence. Below the threshold, Free-mode pitch detection decides. Unpitched attacks (dead notes) are retracted.
- Position follows the player (attacks, timing against score distances, pitch verdicts, re-alignment), not a fixed clock. Repeats are unrolled off the audio thread. The timeline is an immutable array handed to the audio thread by pointer and freed only after the audio thread has acknowledged a newer one.
- Report Song Mode with its costs: wrong notes (first Note On wrong, corrections), re-alignments and retractions, next to the latency, against Free mode on the same signal (`bass2midi_songmode_eval`). Include deviations from the score (variations, skipped notes, ghost notes, wrong start).
- Free mode stays the default and must remain unchanged by Song Mode code (verify with the full baseline).

### Score-guidance metrics

Evaluate guidance against the same passages in Free and guided modes. Record correct-note rate, missed-note rate, false-trigger rate, octave-error rate, latency percentiles, and the number of acoustic candidates altered or rejected by the prior. Include deliberate variations and chromatic notes so guidance is not optimized merely by making the system over-restrictive.

## Coding guidelines

- Use clear modern C++20. Keep `dsp/` free of JUCE so it stays testable anywhere; JUCE types belong in `app/`.
- Prefer small, deterministic DSP units with explicit inputs, outputs, units, and reset behavior.
- Keep sample counts internally; convert to milliseconds only at boundaries/UI/metrics.
- Name frequency in `Hz`, periods in `samples` or `seconds`, levels in `dBFS` or linear units, and times in `ms`/`samples` explicitly.
- Document every added latency source and every cross-thread boundary.
- Keep public interfaces narrow. Avoid global mutable DSP state.
- Add parameter validation and safe defaults for all externally supplied settings.
- Do not silently change MIDI semantics, latency, or default thresholds; add tests and explain the impact in the change description.
- Do not copy third-party code without explicit approval from the project owner; when approved, retain attribution and license notices. Reference implementations are reimplemented from papers or documented parameters.

## Test strategy

### Unit tests

Test conditioning, onset detection, pitch candidate estimation, window selection, octave validation, confidence smoothing, velocity mapping, and the MIDI state machine independently. Include boundaries: silence, noise, tiny buffers, sample-rate changes, startup/reset, abrupt level changes, note transitions, and malformed configuration.

### Offline regression corpus

Build and version a labeled corpus of DI bass recordings and synthetic signals. Include each open string and fretted notes, soft/medium/hard plucks, finger/pick, different pickup tones, release/mute behavior, vibrato/slides, noise, and 44.1/48/96 kHz where practical. Store annotations for intended note, onset, release, and known difficult passages.

The test runner should emit machine-readable results (for example CSV or JSON) and a human-readable summary. Each case must record expected vs. emitted notes, onset/release timing, octave errors, duplicate triggers, missed notes, false triggers, and confidence traces when diagnostics are enabled.

### Real-time and host validation

- Run allocation and lock instrumentation around the audio path.
- Test common interface buffer sizes, at least 32, 64, 128, and 256 samples, at 44.1 and 48 kHz.
- Verify the virtual MIDI port and routing in MainStage with a minimal instrument patch.
- Exercise long sessions, device/sample-rate changes, transport restart, plugin reload, and parameter changes.
- Treat manual live-playing tests as essential but not a substitute for reproducible regression tests.

## Metrics and reporting

Every performance claim must be based on a named corpus and configuration. Track:

- Note-on latency: median, p95, p99, minimum, maximum.
- Note-off latency and stuck-note count.
- Correct-note rate, missed-note rate, false-trigger rate, and duplicate-trigger rate.
- Octave-error count/rate, separately from other wrong-note errors.
- Pitch error in cents before quantization, where ground truth exists.
- Velocity correlation/monotonicity across controlled dynamics.
- CPU use per audio block, worst-case callback duration, allocations, and xruns/glitches.
- Results segmented by string/range, technique, dynamic level, sample rate, and buffer size.

Do not average away failures: preserve per-case output and inspect worst cases.

## Development phases

### Phase 0 — Project skeleton and reference baseline

Set up the DSP library, standalone app with virtual MIDI port, test runner and offline evaluation tool. Measure plain-YIN reference setups (`warf-like-yin`, `full-range-yin`) on a synthetic corpus and keep the results versioned (`docs/baseline/`, analysis in `docs/phase0-baseline.md`). Preserve this reproducible baseline before tracker work.

### Phase 1 — Bass-focused instrumentation and guardrails

Constrain the operating range to EADG, add bounded diagnostics, implement/verify the authoritative MIDI state machine, and create the first labeled regression corpus. Establish CI-friendly offline tests.

### Phase 2 — Fast reliable onset and pitch candidates

Add attack detection and adaptive windows. Prototype MPM and modified YIN behind a common interface, then choose from measured results. Add harmonic validation and octave-error suppression.

### Phase 3 — Musical stabilization

Tune confidence scoring, hysteresis, debounce/retrigger logic, release behavior, and velocity mapping using the corpus plus live tests. Optimize only after behavior is demonstrably correct.

### Phase 4 — MainStage workflow and performance hardening

Deliver reliable virtual MIDI routing/host behavior, validate common interfaces and buffer sizes, eliminate real-time violations, and publish a latency/accuracy matrix with known limitations.

### Phase 5 — Deferred expressive control

After V1 is stable, add Song palette score guidance, then evaluate timeline-guided MIDI support alongside pitch bend for slides/vibrato, optional technique-specific handling, broader tuning/range support, and a dedicated Audio Unit only when it offers a concrete workflow benefit.

## Claude Code working rules

Before changing DSP behavior, identify the affected pipeline stage, its latency impact, and the test cases that prove the change. Prefer small, measurable commits. Run the narrowest relevant unit/regression tests first, then the full DSP regression suite for tracker or state-machine changes.

When diagnosing a musical issue, request or preserve a short DI recording plus sample rate, buffer size, interface, bass/string, playing technique, and expected MIDI result. Do not tune thresholds solely to one recording.

When an implementation trade-off is unresolved, present measured evidence in this order: correctness/stability, p95 latency, worst-case callback cost, then code complexity. The project prioritizes playable, repeatable results over algorithm novelty.
