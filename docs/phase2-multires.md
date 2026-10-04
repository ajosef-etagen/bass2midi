# Phase 2 (part 1) — Pitch-adaptive analysis windows

Status: synthetic corpus only. Raw results: [`baseline/phase2-multires/`](baseline/phase2-multires/). The three
earlier setups reproduce the Phase 1 run row for row (1,440/1,440 identical), so all numbers are comparable.

```sh
./build/bass2midi_baseline --out docs/baseline/phase2-multires   # ~7 min
```

## What changed

Pipeline stages: *adaptive analysis-window selection* and *pitch candidates* (CLAUDE.md). The note state machine
is unchanged.

**`dsp/MultiResolutionPitchTracker`** runs a ladder of YIN estimators ("rungs") on the newest samples of each
2.5 ms frame. Rung lowest frequencies: 150, 120, 96, 76, 60, 48 Hz plus the full range (38 Hz). Each window is
1.5 × the rung's longest period (integration length = half the longest lag). The shortest rung that passes all of
these wins:

| Check | Rule (default) | Failure it prevents |
|---|---|---|
| clarity | ≥ 0.93 for rungs shorter than the full range | low-confidence frames of short windows on weak-fundamental tones (0.90 let a few through) |
| edge guard | period ≤ 94 % of the rung's longest lag | a period just beyond the lag range read as a clipped minimum (A#1 read as B1 by the 60 Hz rung) |
| octave safety | the lag at 2 × period was examined by the sub-octave check, or 2 × period exceeds the longest period of the operating range | a short window reporting the 2nd harmonic of a low note (E2 for E1) |
| sub-octave check | prefer the d′ minimum near 2τ if d′(2τ) < 0.5 · d′(τ) | YIN's classic octave-up error on a weak fundamental |
| silence edges | no pitch if the integration segment (window start) or the newest quarter is > 20 dB below the window RMS | windows straddling an attack or a mute compared near-silence with itself and reported random notes with clarity ≈ 1 |

The full-range rung is the fallback and always qualifies, so coverage never gets worse than the single window.
Only the decision gets earlier where a shorter window suffices. All checks are named settings with units and
validation. The new YIN options default to off, which is why the reference setups are unchanged.

Two of these checks were added because the evaluation caught regressions while tuning:

- **Edge guard.** A#1 frames in the sustain region were read as B1.
- **Silence edges.** A re-pluck after a 30 ms mute produced a wrong Note On in 23 of 36 cases, for example
  D2 → B3 → D2.

Both have regression tests that fail without the fix.

## Results (3 sample rates; 480 single plucks, 144 two-note sequences per setup)

| | `full-range-yin+nsm` (Phase 1) | `multires-yin+nsm` |
|---|---|---|
| single plucks correct / octave / extra Note Ons | 480 / 0 / 0 | 480 / 0 / 0 |
| first Note On p50 / p95 / max (all) | 49.9 / 52.7 / 55.2 ms | **27–29.5 / 41.8 / 42.0 ms** |
| low E only (E1–G#1): p50 / max | 47.6 / 55.2 ms | 39.5 / 39.5 ms |
| A string (A1–C#2): p50 / max | 50.1 / 55.2 ms | 32.0 / 34.5 ms |
| D/G strings (D2–G4): p50 / p95 / max | 47.7 / 52.7 / 52.7 ms | 22.0 / 39.5 / 42.0 ms |
| sustain frames correct | 100 % | 100 % (107,840 frames) |
| change by a fourth: pass, p50 | 36/36, 55.2 ms | 36/36, 32.0 ms |
| change by an octave: pass, p50 | 36/36, 87.6 ms | 36/36, 60.6 ms |
| repeat after 30 ms mute: pass, p50 | 36/36, 50.1 ms | 36/36, 29.5 ms |
| repeat while ringing | 0/36 | 0/36 (unchanged, needs a better onset detector) |
| noise −50 / −30 dBFS: Note Ons | 0 | 0 |

Per note (48 kHz, weak fundamental) the first Note On comes after roughly:

- 14.5 ms: C#4, E4, G4
- 20–22 ms: E3, G3, A#3
- 27 ms: C#2, C#3
- 32–35 ms: G1, A#1, A#2
- 39.5 ms: E1, E2, G2

E2 and G2 are slow because their lower octave (E1, G1) is still a bass note and has to be ruled out, which needs a
window holding about three periods.

### Against the CLAUDE.md targets

| range | target p50 / p95 | measured p50 / p95 | one period of the open string |
|---|---|---|---|
| D/G strings | < 8–10 / < 12 ms | 22.0 / 39.5 ms | D2 13.6 ms, G2 10.2 ms |
| A string | < 12 / < 15 ms | 32.0 / 34.5 ms | A1 18.2 ms |
| low E string | < 15–20 / < 25 ms | 39.5 / 39.5 ms | E1 24.3 ms |

Still above every target. As noted in Phase 0, the targets for low positions are shorter than one period, and
this method needs roughly 1.5–3 periods. The open decision about the targets stands.

## Real-time cost

YIN now transforms both real signals with one complex FFT, about 25 % faster. Per 2.5 ms analysis frame, on the
Linux build machine with an optimised build, cost depends on how many rungs run:

| sample rate | high note (first rung wins) | low note (all 7 rungs) | single full-range YIN, for comparison |
|---|---|---|---|
| 44.1 kHz | ~18 µs | ~0.30 ms avg, 0.55 ms worst | ~0.15 ms |
| 48 kHz | ~17 µs | ~0.31 ms avg, 0.67 ms worst | ~0.15 ms |
| 96 kHz | ~38 µs | ~0.68 ms avg, 1.34 ms worst | ~0.35 ms |

At most one frame falls into a 64-sample block at 48 kHz (budget 1.33 ms). At 96 kHz with 64-sample blocks
(0.67 ms budget) the worst case does not fit. **Use 48 kHz, or 128+ samples at 96 kHz.** The app shows the actual
worst callback time on the Mac.

## Still open

- Repeated notes while the string still rings (needs a spectral-flux or period-discontinuity onset detector).
- Real DI recordings to validate the octave safety and the thresholds above. The synthetic corpus cannot provoke
  real octave errors.
- Optional further CPU work: skip rungs that cannot win given the previous frame, or halve the inverse FFT size.
