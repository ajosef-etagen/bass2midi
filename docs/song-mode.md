# Song Mode (attack-triggered expected notes)

Status: Phase 1 prototype. Implemented and measured on synthetic performances. The first real DI takes
were checked against a pseudo-score. **Not yet validated with a real song played along its Guitar Pro
file.**

```sh
./build/bass2midi_songmode_eval --out docs/baseline/song-mode-synthetic        # ~2 min
./build/bass2midi_songmode_eval --song <file.gp5> --seconds 40 --out <dir>     # a song's own bass line
./build/bass2midi_songinfo <file.gp5> --timeline                               # expected notes in playing order
```

## The hypothesis

> If the song position and the expected MIDI note are known, a bass attack can trigger the correct MIDI
> note with substantially lower latency than full real-time low-frequency pitch detection.

The hypothesis holds where the attack is detected. On synthetic performances the median note-on latency
falls from 38.9 ms to 4.8 ms, and p95 from 77.2 ms to 7.6 ms. On two real DI takes, attack-triggered notes
come about 40 ms (median) earlier than in Free mode.

The bottleneck is not pitch. It is detecting attacks reliably on a real, ringing bass, and keeping the
score position in step. Both are covered below.

## Critical review of the proposal

| Proposal | Assessment | What was done |
|---|---|---|
| Song position from a band microphone: beat tracking, chroma, online DTW | This is the hardest and riskiest part: live band mix, bleed, arrangement changes, research-grade alignment. It is not needed to test the hypothesis. The bass player's own attacks already carry the timing. | Deferred. The **attack-driven note follower** replaces it (below). No microphone is needed. The microphone path stays an option for re-entry after long breaks. |
| Phase 1: "manually controlled song position" | A fixed clock drifts and has no recovery. | Manual start ("Start at bar" plus "Restart here"). From there the follower re-aligns itself. |
| "Coarse pitch validates the expected note before sending" | Physically impossible in a few ms. One period of A1 is 18 ms, of E1 24 ms. Every pitch estimate needs at least about one period, and in practice more. | The note is sent on the attack. Pitch validates **afterwards** (about 20–55 ms later). It either confirms (raising confidence and refining the tempo) or corrects (Note Off + Note On of the detected note, then re-alignment). |
| Confidence model with four values | Sensible. | Position confidence: follower, updated from pitch verdicts. Expected-note choice: timing fit with a tempo ratio. Attack strength: ghost filter, unpitched-attack retraction. Pitch plausibility: verdict and correction. |
| "Low confidence: fallback" | Essential. | Below confidence 0.5 there is no prediction; the normal Free pitch path plays. The follower keeps tracking from those notes and recovers. |
| Look-ahead of 1–3 s | Cheap. | The whole timeline is precomputed off the audio thread. The UI shows the next notes. |
| Latency target "almost immediately" | Yes: about 3–6 ms algorithmic latency (detection ≤ 1 ms, velocity window 2 ms, block). | — |
| Not stated | **Repeated notes and legato changes** have no level rise on a ringing string. Detecting them is the actual difficulty. Wrong predictions are **audible**: a wrong note sounds until it is corrected. | Measured and documented. A periodicity-break detector exists (experimental switch). |
| Not stated | Song Mode contradicts the rule in CLAUDE.md "never emit a note solely because the score expects it". | Now an explicit, opt-in mode with its own rules in CLAUDE.md: an acoustic attack is always required, pitch validation and correction are always active, and there is a Free fallback. |

## How it works

```text
bass input -> AttackDetector (1 ms sub-hops) --attack--> SongFollower.onAttack --expected note--> NoteStateMachine.triggerPredicted -> MIDI
           -> framer -> MultiResolutionPitchTracker -> NoteStateMachine (Free path, release, corrections)
                                                    \-> verdict (3 clear frames after the attack) -> SongFollower.onPlayedNote
```

The whole chain is `dsp/BassToMidiProcessor`. The app and `bass2midi_songmode_eval` run identical code.

### Timeline (`song::expectedNotes`)

- The bass track in playing order. Simple repeats with a play count and alternate endings are unrolled.
  Codas, segnos, D.C./D.S. and nested repeats are not. Use "Start at bar" there.
- The score's tempo map is applied.
- Tied notes are merged.
- Simultaneous notes are reduced to the lowest one.
- Grace notes are dropped.

The user's songs give 473 notes over 110 bars and 390 notes over 62 bars.

### AttackDetector

Three detection functions on 1 ms sub-hops:

- **Transient:** 4th-order high-pass above 800 Hz, energy max-held over 26 ms, rise ≥ 12 dB over the
  12 ms minimum.
- **Level:** full-band peak, max-held, rise ≥ 9 dB.
- **Periodicity break** (optional): the residual `x[n] − x[n−P]` of the sounding note's period. It must go
  from ≤ −14 dB (steady) to ≥ −4 dB for 2 ms, and the level must be within 15 dB of the note's peak.

Common rules:

- The hold is needed because one 1 ms peak of a low note varies by more than 20 dB within a period.
  Without it, take 3 gave 498 "attacks" for 33 notes.
- Refractory time 45 ms.
- Velocity is the peak over the 2 ms after detection.

### SongFollower

The position is the index of the next expected note. At each attack:

1. The live time since the previous attack is scaled by the tempo ratio. The adaptive live/score ratio is
   bounded to 0.6–1.6.
2. It is compared with the score distances to the next 4 notes; the closest wins. This skips missed
   attacks.
3. An attack earlier than 40 % of the distance to the next note counts as extra: no prediction, the
   position stays.
4. Far beyond the window means a pause: take the next note.

Pitch verdicts raise or lower the confidence (rate 0.35).

On a mismatch the follower searches ±8 notes for a place where:
- the last 2 played notes fit the score, and
- while still confident, the score time fits the time since the last match.

Without the time check, short repeated figures (A1 A1) made it jump to wrong places.

### State machine and processor safeguards

- A predicted Note On is immediate. The usual pairing guarantee holds: Note Off first.
- The machine's own onset detection ignores the same pluck for 60 ms (no duplicate).
- **Correction:** another note in clear frames (clarity ≥ 0.93) for 20 ms within 150 ms after the trigger
  replaces it.
- Frames whose window still holds mostly the previous note are hidden from this rule. Without that,
  correct predictions were "corrected" back to the ringing previous note.
- **Ghost filter:** an attack more than 20 dB below the previous accepted attack (within 3 s) is left to
  the pitch path.
- **Unpitched retraction:** if no voiced frame (clarity ≥ 0.8) appears within the longest analysis window
  plus 20 ms, the predicted note is ended and the follower position restored. This catches dead-note
  clicks and other non-note events.

A fixed 60 ms deadline retracted real low notes; the window-based one does not.

## Results: synthetic performances

Source: [`baseline/song-mode-synthetic/`](baseline/song-mode-synthetic/), 48 kHz, 64-sample blocks. Latency
is measured from the true onset to the block in which the Note On is sent.

Four bass lines, 216 notes per performance:
- E1/E2 octave pump in eighths at 120 BPM;
- a walking quarter line;
- repeated D2 16ths at 100 BPM;
- an upper melody.

| performance | mode | first Note On correct | first wrong | missed | p50 / p95 / max ms |
|---|---|---|---|---|---|
| as written (±15 ms jitter) | Free | 166 | 0 | 50 | 38.9 / 77.2 / 92.2 |
| | Song | 172 | 0 | 44 | 29.7 / 71.0 / 92.2 |
| | Song + re-pluck detection | **216** | 0 | 0 | **4.8 / 7.6 / 9.5** |
| tempo +8 % | Song + re-pluck | 216 | 0 | 0 | 4.8 / 7.4 / 10.5 |
| 5 % notes skipped | Song + re-pluck | 203 of 203 | 0 | 0 | 4.7 / 7.9 / 9.6 |
| 10 % ghost clicks | Song + re-pluck | 216 | 0 | 0 | 4.6 / 7.4 / 11.7 |
| 10 % notes varied | Free | 175 | 0 | 41 | 38.2 / 76.3 / 90.6 |
| | Song + re-pluck | 195 | **19** | 4 | 4.8 / 44.6 / 97.9 |
| start 4 notes off | Song + re-pluck | 202 | **14** | 0 | 4.8 / 43.8 / 103.8 |

Notes on the table:
- "Missed" in Free mode means repeated same-note plucks. They are a known limit of Free mode.
- Plain Song mode (transient + level only) hardly helps there either. Repeated and legato notes have no
  rise, so the attack is not seen.
- **Wrong notes are the price:**
  - A varied note sounds as the written note for about 40–45 ms, then it is corrected. This hit 19
    first Note Ons, most of the roughly 22 varied notes.
  - A wrong start costs 14 wrong notes before re-alignment.
  - A ghost click with re-pluck detection gives a short blip (18 clicks), but no follow-up error.
- Free mode is unchanged: the full `bass2midi_baseline` run is identical row for row.

## Real DI takes (pseudo-score)

The four earlier takes have no song and no labels. As a stand-in, each take's own Free-mode notes of at
least 100 ms served as the "score". This exaggerates Song Mode's chances (the score is exactly what Free
mode hears), but it exposes false and missed attacks on real audio.

| take | score notes | predictions | first Note On correct / wrong | wrong Note Ons | Song before Free (median) |
|---|---|---|---|---|---|
| 3 (open strings, E/A passage) | 27 | 32 | 21 / 6 | 15 | 39.8 ms |
| 4 (chromatic run, E string) | 26 | 28 | 25 / 1 | 14 | 42.2 ms |
| 5 (chromatic runs, A/D/G) | 92 | 59 | 80 / 9 | 39 | ≈ 0 ms (most notes still via Free) |
| 6 (E-string riff, repeated D3) | 36 | 22 | 25 / 10 | 26 | ≈ 0 ms |

Re-pluck detection makes the real takes worse: more extra attacks and more wrong notes. The switch is
therefore off by default.

Failure classes found in the traces:

1. **Precursor events.** A weak "attack" (velocity 20–50) comes 50–70 ms before the actual pluck: fretting
   finger, a finger touching the string, or a mute. The unpitched retraction takes it back and the real
   pluck re-triggers, but the note is doubled.
2. **Events without a new note.** Damping or release noise is taken as an attack, and the next note is
   predicted too early (e.g. D2 0.6 s early in take 3).
3. **Missed attacks on a ringing string.** Real fingerstyle re-plucks and legato changes often raise
   neither level nor high-band energy: 10–15 dB over 40–60 ms. The periodicity break sees them
   (residual −30 dB → 0 dB) but also fires about 3× too often on the takes.

Attack detector alone, defaults, on the takes:

| | take 3 | take 4 | take 5 | take 6 |
|---|---|---|---|---|
| attacks / Free notes ≥ 100 ms | 39 / 27 | 44 / 26 | 126 / 92 | 53 / 37 |
| attack before Free Note On, median | 48 ms | 49.5 ms | 41 ms | 44 ms |

## First song take: "Something's got a hold on me" (live, with mistakes)

The owner's live DI take of the song whose `.gp` file is loaded: 195 s, 44.1 kHz, 16 bit. The audio is not stored
in the repository. There are no labels, so two references were used:
- the score, aligned to the take's Free-mode notes by sequence alignment (Needleman-Wunsch on pitches, then a
  piecewise-linear time warp through the matched notes);
- Free mode's pitch as "what was played".

```sh
./build/bass2midi_analyze take.wav --song song.gp                 # Song Mode events + attacks CSV
./build/bass2midi_analyze take.wav --song song.gp --mode palette  # Free mode with the song palette
```

**How closely the take follows the score.** Repeated notes were collapsed, because Free mode merges re-plucks.
Of 295 played pitch changes, only **66 %** are what the score has at that point. The rest:
- 51 are other notes;
- 50 are extra notes;
- 83 written notes were not played;
- around bar 80 a section of about 16 s is skipped.

This is a live interpretation, not a transcription error.

**Song Mode on the take** (defaults):
- 188 predictions: 105 confirmed by the pitch path, **83 wrong**, then corrected about 40–60 ms later.
- Most wrong predictions lie 1–2 semitones from the played note: in chromatic runs the position is one note off.
- Raising the prediction threshold (confidence 0.7 / 0.8 / 0.9) does not raise precision above about 55–60 %. It
  only lowers coverage, because the deviations come without warning.
- A stricter skip rule (`skipMargin`) did not help either. Default unchanged.

**One fix from this take.** On the long written notes of the intro (one note every 6–12 s), the player re-plucks
the same pitch in rhythm. A rule ("an early pitch-path note equal to the next written note counts as it") took
such a re-pluck as the next written bar, seconds early, and everything after it was shifted. The rule now
applies only near the expected time (`earlyMatchToleranceSeconds` / `earlyMatchToleranceFraction`). Matches
went from 60 to 146; the synthetic results are unchanged.

**Bar-wise view** (suggested by the owner). Only bars with alignment anchors within ±2.5 s are counted:

| | notes | played as written |
|---|---|---|
| first note of the bar (downbeat) | 64 | **72 %** |
| other notes | 290 | 51 % |
| downbeat, previous bar's pitches exactly as written | 11 | 91 % (10) |
| downbeat, previous bar ≥ 75 % as written | 8 | 50 % |
| downbeat, previous bar < 75 % | 35 | 71 % |

The bar is the more robust unit: downbeats are mostly right. Predicting every downbeat would still send a
wrong note on about one bar in four. "Previous bar exactly as written" looks like a usable gate, but 11 cases
are too few to decide.

**Free mode with the song palette on the same take:**
- 509 of 515 notes are identical to Free mode;
- 39 notes come more than 5 ms earlier (up to 50 ms);
- no wrong notes from prediction.

It is the robust option for live interpretations.

## Requirement status

| # | Requirement | Status |
|---|---|---|
| 1 | Song position from the band microphone (beats, chroma, DTW) | **deferred** (see review); attack-driven follower with tempo ratio and re-alignment instead |
| 2 | Bass attack detection, velocity, timing from the player | done (AttackDetector) |
| 3 | Expected note with timing tolerance, no autoplay | done (SongFollower; only attacks trigger notes) |
| 4 | Coarse pitch plausibility, nearby candidates, re-alignment | done, after the Note On (physically unavoidable) |
| 5 | Note On/Off, velocity, virtual MIDI | done (unchanged MIDI path); pitch bend/CC later |
| 6 | Note On almost immediately after the attack | done: ~5 ms synthetic; on real audio only where the attack is detected |
| 7 | Confidence model | done (position / timing / attack strength / pitch verdict); combined as the prediction gate |
| 8 | Look-ahead | done (precomputed timeline, UI shows the next 4 notes) |
| 9 | Free / Song mode | done (switch; low confidence falls back to Free automatically) |
| 10 | Initial scope | macOS, 4-string EADG, monophonic, .gp/.gp5; any time signature in the timeline |
| 11 | Debug view | done (position bar/beat, upcoming notes, confidence, tempo, attack velocity, predicted vs. heard note, trigger delay, counters) |
| 12 | Real-time safety | done: no allocation or locks in the callback; the timeline is an immutable array published by atomic pointer, freed only after the audio thread acknowledges a newer one |
| 13 | Phase order | phase 1 done; phases 2–3 (microphone) deliberately not started |

## Next step: ground truth

All open points need the same thing: **DI recordings of a song played along its Guitar Pro file.** For
example, the Blues Brothers bass line, at the song tempo with a click or the original:

- once as written;
- once with deliberate variations, fills and ghost notes;
- the click or mix on a second channel, so the start can be aligned.

With these, Song Mode can be evaluated against the real score: correct-note rate, wrong notes, latency
percentiles, and the number of corrections and re-alignments. Then:

1. Decide the re-pluck default and tune its thresholds against labelled attacks, not pseudo-labels.
2. Discriminate precursor and mute events from plucks: the spectral shape of the first milliseconds,
   or a pitch-onset check before re-triggering an already sounding note.
3. Decide whether a microphone/clock position source is still needed. A MIDI "restart at bar" from
   MainStage may be enough.
