# Bar playback and the live score follower

Status: experimental, both opt-in. Free mode is the default and unchanged. A full `bass2midi_baseline` run is
identical row for row with and without this code.

Both features build on the song timeline (`song::expectedNotes(song, track, bars)`). It contains:
- the bass notes in playing order, with repeats unrolled and the tempo map applied;
- one `TimelineBar` per played bar;
- per note, the string and fret (tablature) and an `anchorWeight`.

## Owner's observation behind both

On a live take the player keeps the bar's root and leaves out or changes most fills of the Guitar Pro file
(see [`song-mode.md`](song-mode.md), "Bar-wise view"):
- the first note of a bar is as written 72 % of the time;
- the other notes are as written 51 % of the time.

So the bar, not the single note, is the unit to follow, and the root (downbeat) is the evidence to follow it by.

## Tablature (`song::assignFingering`)

- Notes keep the string/fret of the Guitar Pro file when it has them.
- Otherwise a Viterbi pass picks one of the playable 4-string EADG positions (frets 0–20) per note. The cost is
  hand movement, string changes and high frets, so the result needs few position changes.
- On the two test songs the computed fingering agrees with the file's own tab for 80 % and 95 % of the notes.
  The rest are equivalent positions, for example fret 5 on A instead of open D.

### Song key

`song::transposeTrack` moves the chosen track by ±12 semitones before the palette, timeline and tablature are
built. In the app this is the **Song key** setting, saved per song.
- File fingerings shift along the same string; where that leaves frets 0–20 the note is fingered anew.
- Notes that would fall below E1 move up an octave, because a 4-string EADG bass cannot play them.

## Anchor weights

`anchorWeight` (0..1) says how likely the player is to play a note as written:

| metric position | weight |
|---|---|
| first note of the bar | 1.0 |
| beat 3 of a 4/4 bar | 0.7 |
| other beats | 0.5 |
| eighth off-beats | 0.3 |
| anything finer | 0.2 |

- Notes of at least one beat get +0.15; notes shorter than half a beat get −0.1. The result is limited to 0.1..1.
- A later note in the bar with the root's pitch class gets +0.1. The root is the bar's first note, used as a
  chord proxy.
- Chord symbols in the file are not used yet.

## Bar playback (`Mode::bar`, `BarPlayer`)

Owner's idea: the player plays the roots; Bass2MIDI plays the bar's written notes, including the fills the
player leaves out.

- An attack near the expected downbeat starts the next bar.
  - The first downbeat: any attack.
  - Later downbeats: an attack between 0.2 beat early and 0.4 beat late. Fills one eighth early are not taken as
    the downbeat.
- The bar's notes then play at the live tempo:
  - the tempo comes from the downbeat spacing (EMA, max step 15 %);
  - velocity follows the downbeat attack.
- A bar is never started without the player's attack. Nothing plays more than one bar ahead. Without a downbeat the
  playback stops after the current bar.
- Playback follows the player. After a stop, an attack on a later bar's downbeat at the running clock resumes there
  (the band played on). Any other attack starts the bar that was awaited: the player was late or paused. A weak
  click (more than 30 velocity units below the last downbeat) is ignored. If the late downbeat implies a tempo
  within range (≥ 0.6 × score), that tempo is learned. A player 20 % slower than the file used to lose almost
  every bar (15 of 80 notes); now 74 of 80 notes play on time, with one stop at the start.
- The pitch path checks the root (pitch class). Two mismatching roots in a row fall back to Free mode; two
  matching roots resume.

Synthetic result (`bass2midi_barmode_eval`, [`baseline/bar-mode-synthetic/`](baseline/bar-mode-synthetic/)):
- on the built-in groove and on 40 real bars of the owner's song, all written notes play on time (p50 ≈ 7 ms,
  p95 ≈ 13 ms);
- pauses, accelerando, ritardando, a 20 % slower player, a player who stops, wrong roots and extra fills by the
  player behave as intended.

On the owner's live take (full bass line, not only roots) bar playback loses the bar:
- many fills fall into the downbeat window;
- many bars start with a variation of the written root.

It is meant for playing the roots, and has not yet been tested with a take played that way.

## Live score follower (`ScorePositionTracker`)

It answers the question "where in the song am I?" without playing anything. Its output is shown in the
tablature window and the diagnostics. MIDI output is unchanged.

**Tempo clock**
- The player sets the start bar and the BPM (default: the file's tempo at that bar).
- The clock starts with the first played note, aligned to that bar's first written note, or with **Start clock**.

**Position correction by anchors**
- Every note the pitch path decides is compared with the written notes near each of 12 position hypotheses
  (score position, tempo, log weight).
- The likelihood of a note under a hypothesis is the product of:
  - pitch: 1 for the same note, 0.6 for an octave;
  - timing: Gaussian, σ = 80 ms;
  - anchor weight: 0.3 + 0.7 · w;
  - with a background floor of 0.05.
- A matching hypothesis moves towards the written time (gain 0.6 × w). Its tempo is updated from matches at least
  1 s apart.
- Fills the score does not have match nothing and change nothing.

**The clock waits for the player**
- The position never runs more than one beat past the next written downbeat or half-bar note that has not been
  played yet (`holdBeats` = 1, `waitMinAnchorWeight` = 0.7). There it stops and shows "waiting for your next note".
- The next played note continues from there. It may be any note up to a bar ahead, and the match places the
  position on it.
- A short wait (≤ 2 beats, `pauseBeats`) means the player is slower than the clock: the tempo is still measured,
  with an EMA step of 0.5. A longer wait is a pause, and no tempo is measured across it.
- A player 20 % slower than the set BPM is never more than one beat behind the display. After about 7 bars the
  tempo is learned and the position is on time.
- Waiting on every beat (`waitMinAnchorWeight` 0.5) and a shorter hold (½ beat) lost the position on the live take,
  because the band played on while the player varied the line. Downbeats and half bars with a one-beat hold cost
  little there (see below).

**Several candidates**
- Hypotheses far below the best (< −6 log) are replaced by variations of the best one.
- They can also be replaced by jump candidates: strong anchors with the played pitch within ±16 bars, nearest
  first, with a penalty of 3 + 0.15 per bar of distance.
- If the best hypothesis explains 6 notes in a row poorly, jump candidates are forced in (2–4 jumped wrongly on
  the live take).
- Confidence is the weight share of hypotheses within ¼ beat of the best.

**Real time**: fixed arrays; per note O(12 × notes in a 1-beat window); per block two binary searches.

**Measured on the owner's live take**, a band tempo of about 145 BPM:
- Reference: the score aligned to the take by sequence alignment (see `song-mode.md`).
- Method: an ad-hoc harness that is not yet in `eval/`. It feeds the take's Free-mode notes to the tracker and
  compares the position with the aligned score time at 174 aligned notes.

| start, BPM set | version | median error | ≤ ½ beat | ≤ 2 beats |
|---|---|---|---|---|
| bar 18, 130–160 | without waiting | 0.15–0.16 beat | 76–78 % | 93–95 % |
| bar 18, 130–160 | waits for the player | 0.15–0.17 beat | 75–78 % | 93–95 % |
| bar 4, 120 | without waiting | 0.17 beat | 66 % | 83 % |
| bar 4, 120 | waits for the player | 0.21 beat | 66 % | 83 % |

An earlier version of this note gave 81–84 % for bar 18. That was a different harness setup; the numbers above
come from one setup run on both versions.

The position holds even with a set BPM up to about ±10 % off, because the roots correct it. Unit tests
(`ScoreTrackerTests`) cover:
- the clock;
- roots only at 8 % off tempo plus foreign fills;
- a skipped section found again by a jump;
- the clock waits when the player stops and continues with the player;
- a player 20 % slower than the BPM;
- validation.

## App

1. Select a song. Choose the **start bar** and check the **BPM**, then press **Go to bar**. This re-arms all
   followers at that bar; after changing the BPM, press it again.
2. **Show tablature** opens a resizable window:
   - two rows of four bars, G-D-A-E;
   - fret numbers, with anchors slightly larger;
   - a red position line, the next note in orange, played notes dimmed;
   - a header with bar, beat, BPM, status and the next four notes with string/fret.
3. Play. The clock starts with the first note. **Stop clock** stops it, and **Go to bar** re-arms it.
4. **Mode**:
   - Free: the default.
   - Song Mode.
   - Bar playback: your downbeat starts the bar, the song's notes play.

   The score follower runs in every mode. The tablature shows the position of the active mode:
   - Song Mode: the note follower. The line sits on the last note played and moves only with the next attack.
   - Bar playback: the bar player. The line moves through the bar being played and stops at the awaited downbeat.
   - Free: the score follower, which waits for the player.

## Next steps

- A DI take that plays **only the roots** along the file, to measure bar playback on real audio.
- Drive bar playback (and Song Mode predictions) from the score follower's position and confidence instead of
  their own counters. This is item 6 of the follower specification: predicted notes, attack triggering and wrong-note
  correction.
- Move the ad-hoc live evaluation harness into `eval/`.
- Use chord symbols as anchors, when files have them.
