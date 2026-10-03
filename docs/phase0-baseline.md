# Phase 0 — Reference baseline

Status: first measurement, synthetic corpus only. Raw results: [`baseline/phase0-synthetic/`](baseline/phase0-synthetic/)
(`summary.md`, per-case `cases.csv`, `noise.csv`). Regenerate with:

```sh
cmake -S . -B build -G Ninja -DBASS2MIDI_BUILD_APP=OFF && cmake --build build
./build/bass2midi_baseline --out docs/baseline/phase0-synthetic
```

## Why there is no Warf fork

[Warf](https://github.com/dferreiramarques/warf) was evaluated as the intended starting point and rejected as a
code base:

- **Wrong delivery format for the workflow.** Warf is VST3-only. MainStage hosts Audio Units only, so a VST3 plugin
  cannot sit in `bass -> interface -> Bass2MIDI -> virtual MIDI -> MainStage`. Bass2MIDI V1 is a standalone app that
  publishes a virtual CoreMIDI source instead.
- **Range.** Its window is sized for a 60 Hz floor (2048 samples at 44.1/48 kHz), which limits the lag search to
  ~43 Hz at 44.1 kHz and ~47 Hz at 48 kHz: E1 (41.2 Hz) is unreachable, and so is F1 (43.7 Hz) at 48 kHz.
- **Real-time hygiene.** Its plugin wrapper takes a lock and calls the MIDI device API from the audio callback.
- **Little reusable substance.** The DSP is about 200 lines (plain YIN, O(N²) difference function, and a
  frame-count debounce). Everything Bass2MIDI needs beyond that (onset detection, adaptive windows, octave
  validation, the note state machine) does not exist there.

Its default framing is kept as a measurement reference (`warf-like-yin`), reimplemented from its documented
parameters. No Warf code is in this repository.

## What was measured

**Estimator.** One independent YIN implementation (`dsp/`, FFT-based difference function, verified against the
direct O(N·L) formula in the unit tests) in two reference setups:

| setup | window @ 48 kHz | hop @ 48 kHz | lag range @ 48 kHz | purpose |
|---|---|---|---|---|
| `warf-like-yin` | 2048 (42.7 ms) | 512 (10.7 ms) | 46.9 Hz – 24 kHz | what a generic 60 Hz-floor tracker does |
| `full-range-yin` | 2528 (52.7 ms) | 120 (2.5 ms) | 38 – 421 Hz | plain YIN stretched to cover E1–G4 |

**Note decision.** A reference debounce rule (not the Bass2MIDI state machine): frame accepted when YIN clarity
≥ 0.85, window RMS ≥ −40 dBFS and within 35 cents of a semitone; Note On after 2 consecutive accepted frames on the
same note; Note Off after 4 rejected frames. These equal Warf's defaults (Sensitivity 0.5).

**Corpus** (`eval/SyntheticBass.*`, deterministic): every semitone E1–G4 (40 notes) × 44.1/48/96 kHz × two
timbres (full fundamental, fundamental −18 dB as with a bridge pickup) × two levels (medium: peak ≤ 0.3 ≈ −10 dBFS;
soft: peak ≤ 0.03 ≈ −30 dBFS) = 480 cases per setup. Plucks are harmonic series with per-partial decay, 1 ms
attack, a 4 ms pluck-noise burst and a −90 dBFS noise floor. Plus 10 s of white noise at −50 and −30 dBFS per setup
and sample rate.

**Latency definition.** Decision time of the first Note On (end sample of the frame that triggers it) minus the
known onset sample. This is *algorithmic* latency only; see "End-to-end budget" below for what adds to it.

## Results

Medium level, all sample rates (240 cases per setup):

| setup | correct first Note On | missed | octave errors | extra Note Ons | p50 | p95 | max |
|---|---|---|---|---|---|---|---|
| `full-range-yin` | 240 | 0 | 0 | 0 | 49.9 ms | 52.7 ms | 55.2 ms |
| `warf-like-yin` | 230 | 9 (E1; F1 at 48/96 kHz) | 0 | 0 | 49.3 ms | 62.5 ms | 62.5 ms |

Plus one wrong note for `warf-like-yin`: E1 at 44.1 kHz was reported as F1 after 225 ms (its longest searchable period, 1024 samples, is shorter than the E1 period of 1070 samples).

- **Soft level: all 480 soft cases missed in both setups.** The −40 dBFS RMS gate is evaluated over the whole
  window, which at onset still contains pre-onset silence; a pluck peaking around −30 dBFS never passes. A fixed
  absolute gate is therefore an input-gain-staging problem as much as a DSP one.
- **Steady-state accuracy is not the problem.** In the sustain region `full-range-yin` is correct on 100 % of
  105,120 frames, median error 0.01 cents. No octave errors occurred on this corpus.
- **No false triggers** on noise at −50 or −30 dBFS (0 valid frames in 10 s for each setup and rate).
- **Sample rate does not matter** for `full-range-yin` (windows scale with the rate). The `warf-like-yin` numbers
  differ between 44.1 and 48 kHz only because its hop is 11.6 ms vs 10.7 ms.

## Interpretation

1. **Latency ≈ window length.** Plain YIN only reaches clarity ≥ 0.85 when the window is dominated by the new note,
   so the decision lands roughly one window after the onset: ~50 ms. That is 2.5–6× over the targets
   in `CLAUDE.md`. A smaller hop does not help; a shorter or onset-aligned window does.
2. **The synthetic corpus is too clean to judge octave robustness.** Zero octave errors here says nothing about a
   real DI bass (inharmonicity, string beating, pickup comb filtering, finger/pick noise). The labelled DI corpus
   is the prerequisite for any octave-validation work.
3. **The gate must be relative or onset-aware**, not a fixed absolute RMS level over a long window.

## Physical limit of the latency targets (decision needed)

Periodicity-based estimators (YIN, MPM) need to see roughly 1–2 periods of the fundamental after the onset.
For the open strings that already exceeds most of the current targets:

| open string | f0 | 1 period | 2 periods | target median (CLAUDE.md) |
|---|---|---|---|---|
| E1 | 41.2 Hz | 24.3 ms | 48.5 ms | < 15–20 ms |
| A1 | 55.0 Hz | 18.2 ms | 36.4 ms | < 12 ms |
| D2 | 73.4 Hz | 13.6 ms | 27.2 ms | < 8–10 ms |
| G2 | 98.0 Hz | 10.2 ms | 20.4 ms | < 8–10 ms |

Meeting them for low notes would mean deciding from less than one period, i.e. inferring the fundamental from the
attack transient or from higher harmonics, which is exactly where octave errors come from. Options:

- keep the targets for fretted notes higher on each string and express the open/low-position target in periods
  (e.g. "≤ 1.5 periods + 3 ms");
- or accept provisional early decisions with later correction, which MIDI cannot undo without an audible retrigger.

The targets were deliberately left unchanged here; this is a product decision.

## End-to-end budget beyond the algorithm

| source | size | notes |
|---|---|---|
| interface input latency | device-specific, typ. 1–5 ms | the app shows the driver-reported value |
| host block | ≤ 1 block: 32 / 64 / 128 / 256 samples = 0.67 / 1.33 / 2.67 / 5.33 ms at 48 kHz | analysis frames do not depend on block size (tested) |
| MIDI sender thread | ≤ 1 ms (1 ms poll) | lock-free queue from the audio thread |
| MainStage | its own buffer and instrument latency | outside Bass2MIDI |

## Not yet measured

- Real DI recordings (no corpus yet). Needed: open strings and fretted notes, finger/pick, soft/medium/hard,
  mutes/releases, at 44.1/48/96 kHz, with the interface model and gain setting recorded.
- Callback cost on a Mac (the app measures average/worst callback time live; FFT size 4096 per 2.5 ms hop at 48 kHz).
- Virtual MIDI port and test-note route into MainStage (needs a Mac).
