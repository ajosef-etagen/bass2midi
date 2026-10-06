# Phase 1 — Note state machine (first playable version)

Status: first measurement, synthetic corpus only. Raw results: [`baseline/phase1-statemachine/`](baseline/phase1-statemachine/)
(`summary.md`, `cases.csv`, `sequences.csv`, `noise.csv`). The two reference setups in that run reproduce the
Phase 0 results row for row (960/960 identical), so the numbers are directly comparable.

```sh
./build/bass2midi_baseline --out docs/baseline/phase1-statemachine   # ~4-5 min
```

## What changed

`dsp/NoteStateMachine` is now the single authority over Note On / Note Off / velocity, and the app sends its
events to the virtual MIDI port. Pipeline stage: *note quantization, hysteresis, MIDI state machine*. The pitch
estimator is unchanged (`full-range-yin`: plain YIN, 38–420 Hz, window 2 × longest period, 2.5 ms hop).

| Mechanism | Rule (defaults) | Why |
|---|---|---|
| Level envelope | max hop peak over the last **25 ms** | longer than the E1 period (24.3 ms); a bare 2.5 ms hop peak ripples > 10 dB within one low-note period and looked like new attacks |
| Gate | Note On needs envelope ≥ **−45 dBFS**; release threshold 6 dB lower | user-adjustable in the app; relative to peaks, not window RMS |
| Note On | **2** consecutive frames on one note, clarity ≥ 0.80, within ±40 cents | ±40 cents gives hysteresis: a neighbour note must be ≥ 60 cents away |
| Note change | different note for **10 ms**; octave jump (±12/24) for **40 ms**; with a pending onset only 2 frames | octave changes are high-cost transitions (CLAUDE.md) |
| Onset | envelope rises ≥ **9 dB** over its 20 ms minimum | arms a re-attack and restarts velocity measurement |
| Same-note re-attack | onset pending and **0.75 × window** elapsed, pitch still the same | wait until the window holds the new attack |
| Note Off | envelope below the release threshold for **30 ms**, or no countable pitch for **200 ms** | one uncertain frame never ends a note |
| Velocity | attack peak since the onset, −40 dBFS → 20 … −6 dBFS → 127, linear in dB | early-attack feature, not the decay |

Guarantees (unit-tested, including 20,000 pseudo-random frames): never two Note Ons without a Note Off, every
Note Off matches the sounding note and its channel, Note Off precedes the next Note On, and channel change, output
disable and device stop end a sounding note.

### Latency impact

- Note On: unchanged estimator, plus `attackFrames` (2 frames = up to 2.5 ms beyond the reference rule's 2 frames,
  i.e. the same). Measured p50 ≈ 48–50 ms, p95 ≈ 52–55 ms, the same as `full-range-yin` with the reference rule.
- Note change without onset: +5 ms (4 frames instead of 2); octave jumps without onset: +35 ms by design.
- Note Off: up to 25 ms (envelope hold) + 30 ms (release hold) after the level drops.

## Results (all 3 sample rates, 480 single plucks, 144 sequences)

| | `full-range-yin` (reference rule) | `full-range-yin+nsm` |
|---|---|---|
| single plucks, medium level: correct first note | 240/240 | 240/240 |
| single plucks, soft level (peak ≈ −30 dBFS): correct | **0/240** (−40 dBFS RMS gate) | **240/240** |
| octave errors / extra Note Ons | 0 / 0 | 0 / 0 |
| Note On latency p50 / p95 / max | 49.9 / 52.7 / 55.2 ms | 49.9 / 52.7 / 55.2 ms |
| velocity, medium vs. soft plucks (p50) | fixed 100 | 95 vs. 32 (ranges 88–102 vs. 26–39, no overlap) |
| change by a fourth | 36/36, p50 50.2 ms | 36/36, p50 55.2 ms |
| change by an octave (no new attack level) | 36/36, p50 52.7 ms | 36/36, p50 87.6 ms |
| repeat, muted 30 ms between | 36/36 | 36/36, p50 50.1 ms |
| repeat while ringing | 33/36 (see below) | **0/36**: merged into one note |
| noise at −50/−30 dBFS: Note Ons | 0 | 0 |

**Repeat while ringing.** The second pluck raises the level by only about 4 dB in this corpus, below the 9 dB
onset threshold, so the state machine keeps the first note. The reference rule passes most of these only because
the pitch briefly drops out at the re-pluck and it ends notes after 4 unvoiced frames, the same behaviour that
chops notes on real dropouts. A reliable repeated-note trigger needs a better attack detector (spectral flux or
phase/period discontinuity), which is Phase 2. Until then: mute briefly between repeated notes.

**Octave changes** take ~35 ms longer than other changes when no clear new attack is detected. This is the
intended trade-off against octave errors, which a synthetic corpus cannot provoke; it should be re-tuned once real
DI recordings exist.

## Still open

- Real DI corpus (the most important next input): open strings and fretted notes, finger and pick,
  soft/medium/hard, repeated notes, mutes, slides, with interface, sample rate and gain noted.
- Phase 2: attack detection and adaptive windows to cut the ~50 ms latency, plus onset-based repeated-note triggering.
- MainStage test with real playing; callback cost measured on the Mac (shown live in the app).
