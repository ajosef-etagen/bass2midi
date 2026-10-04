# First real DI recordings

Four DI takes from the project owner's bass. They were recorded in Logic: 48 kHz, 24 bit, mono, 13–38 s each,
peaks −0.9 to −4 dBFS. The audio is not stored in the repository. The analyser works on any WAV file:

```sh
./build/bass2midi_analyze <take.wav> --out analysis/   # prints the emitted notes; writes *_events.csv, *_frames.csv
```

It runs exactly the app chain: `MultiResolutionPitchTracker`, then `NoteStateMachine` with default settings,
2.5 ms hop, gate −45 dBFS.

Content, as inferred from the output; there are no labels yet:

| take | content |
|---|---|
| 3 | open strings E, A, D, G held about 4 s each, then a passage on the E and A strings |
| 4 | chromatic run on the E string upwards, continuing higher |
| 5 | chromatic runs on the A, D and G strings |
| 6 | E-string riff (E1, F#1, G1, G#1, A1), then repeated D3 notes |

## What the synthetic corpus could not show

Sustained notes are tracked cleanly. For example, the open strings in take 3 are held 4 s each without a break.
The errors are short wrong notes (20–90 ms) at note transitions. They come from two sources found in the frame
traces:

1. **Release glides.** When the fretting finger lifts, the decaying string glides through neighbouring pitches.
   This happens 20–30 dB below the attack, with YIN clarity 0.86–0.96. The state machine took these frames as
   note changes, e.g. G1 → D#1 → F1 → G#1 for a G1 → G#1 step. Lifting a finger can also let the open string
   ring briefly, so short open-string notes (often A1) appear inside chromatic runs.
2. **Attack frames over a noisy background.** In the first milliseconds of a pluck, the longest window holds
   −40 dB of noise plus the start of the new note. It returned a wrong pitch (C2 instead of G#1) with clarity
   0.92. The silence-edge check did not fire, because neither part of the window is near-silent.

## Fixes

| Fix | Rule (default) |
|---|---|
| YIN level-change check (all tracker rungs) | no pitch when the window start (integration segment) and the newest quarter differ by more than **12 dB** |
| No-onset note changes in the state machine | a frame only counts towards a change without a new attack if clarity ≥ **0.93** and the analysis-window RMS is at most **20 dB** below the highest window RMS since the note started |

Notes started by a new attack (onset) are unaffected. Regression tests cover both rules. Both were developed on
these four takes only, so they must be re-checked on further recordings. CLAUDE.md warns against tuning to one
recording.

## Results on the four takes

There are no labels yet, so notes shorter than 100 ms serve as a proxy for spurious notes. At 80 BPM the shortest
intended notes are eighths of about 375 ms.

| take | notes before | short before | notes after | short after |
|---|---|---|---|---|
| 3 | 45 | 16 | 33 | 6 |
| 4 | 49 | 18 | 32 | 6 |
| 5 | 152 | 46 | 115 | 23 |
| 6 | 72 | 32 | 60 | 23 |
| total | 318 | 112 | 240 | 58 |

The long-note sequences were compared before and after. The removed long notes inspected so far were spurious,
for example the A3 and the duplicate D2 between C#2 and D#2 in take 5. A full check needs labels.

On the synthetic corpus the `multires-yin+nsm` results are unchanged: same notes, same latencies, 100 % sustain
frames.

## Open points from these takes

- **Gain.** Peaks reach −1 dBFS, and the velocity curve (−40 … −6 dBFS → 20 … 127) saturates: most notes come out
  at velocity 127. Either record and play about 6 dB lower, or make the velocity range adjustable or calibrated.
- **Gate.** Between notes the background sits around −40 to −50 dBFS in some takes, above the default release
  threshold (−51 dBFS). Notes then end late: the D2 in take 5 is held about 230 ms after the string is muted. For
  this setup the gate should be about −35 dBFS, or release should also be relative to the note's own level.
- **Labels.** The exercise contents (notes, tempo) would turn the proxy into real correct/missed/wrong counts.
