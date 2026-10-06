# Song palette (score guidance, part 1)

Status: song files and palette weighting are implemented and measured on the synthetic corpus. Real playing with
a loaded song is still to be tested. Raw results: [`baseline/phase5-palette/`](baseline/phase5-palette/).

```sh
./build/bass2midi_baseline --palette --out docs/baseline/phase5-palette   # ~4 min
./build/bass2midi_songinfo <song.gp|song.gp5>                             # tracks, tuning, bass palette
```

This follows the CLAUDE.md order: *Song palette* comes before *Timeline-guided*. A palette is the set of notes
of the song's bass part. No tempo or transport is involved. Free mode, with an empty palette, stays the default.
In Free mode the results are identical to `multires-yin+nsm` in [`baseline/phase2-multires/`](baseline/phase2-multires/):
all 480 single-pluck cases match row for row.

## Song files (`song/`, library `bass2midi_song`)

The library is non-real-time, has no JUCE dependency, and links zlib. Both formats are implemented from their
observable structure. No third-party parser code is used.

| Format | Container | Content used |
|---|---|---|
| `.gp` (Guitar Pro 7/8) | ZIP, `Content/score.gpif` (XML) | tracks, tuning (`Tuning`/`Pitches`), master bars → bars → voices → beats → notes; rhythms with dots and tuplets; ties; grace notes (no metric time); tempo automations |
| `.gp5` (Guitar Pro 5.00/5.10) | sequential little-endian binary | header, measure headers (time signatures), tracks (strings, tuning, MIDI program, percussion flag), all beats and notes. Every other field is skipped by its size: chords, texts, beat/note effects, mix tables (tempo changes are kept) |

- The bass track is picked by `findBassTrack`, in this order:
  - not a percussion track;
  - "bass-like": instrument type or MIDI program 32–39, a name containing "bass", or a lowest string below C2;
  - the most notes.
- The palette is every attacked note of that track. Tie continuations are skipped.
- Pitch is the string tuning plus the fret, i.e. the sounding pitch in MIDI numbers. That is the same note
  space the tracker detects, before any MIDI transposition.
- Corrupt input fails cleanly:
  - bounds-checked reads;
  - sanity limits on measure, track and beat counts;
  - a limit on the ZIP entry size;
  - a limit on XML nesting depth.
  - Unit tests feed every truncated prefix and bit-flipped variants.

Verification against an external reference parse (PyGuitarPro, used only as a black box):

| File | Result |
|---|---|
| user's `.gp` (GP 8.1) | bass track 484 notes (11 ties), 20 pitches F#1–D#3: identical |
| user's `.gp5` (5.10) | bass "Donald Dunn - Bass" 407 notes (17 ties), palette G1 A1 C2 D2 E2 F2 F#2 G2 with identical counts; every note's start, pitch and tie flag identical |
| 16 generated `.gp5` files (5.00 and 5.10) | every note of every track identical. The files contain chords, mix tables with tempo/volume, strokes, tremolo bar, bends, grace notes, all harmonic types, trills, tremolo picking, fingering, tuplets, dotted notes, rests, markers, repeats, time-signature changes and a second voice |

The user's song files are not committed. The unit tests use generated fixtures (`tests/data/`, regenerated with
`make_song_fixtures.py`).

## Palette weighting (soft, never a gate)

Pipeline stages: *pitch candidates* (tracker) and *confidence/hysteresis decision* (state machine). The palette is
applied after candidate estimation and before the final decision. The raw estimate stays available for diagnostics.

**Tracker (`MultiResolutionPitchTracker::setPalette`, `paletteOctaveRelief`)**

Without a palette, a short window may only decide if it could rule out the lower octave (2P examined). For
E2–C#3 this means the full 53 ms window.

With a palette, a short-window candidate may be used early if:
- the palette contains its note, and
- the palette contains neither of the two lower octaves.

This applies only to frames where the free tracker has no valid result, i.e. right after an attack while the
long windows still straddle the onset. As soon as the octave-safe window is valid, its result wins exactly as in
Free mode. A wrong early guess is therefore corrected, not locked in. A unit test plays E1 against a palette that
contains only E2 and checks this.

**State machine (`NoteStateMachine::setPalette`)**

A note outside a non-empty palette needs more consistent frames:
- `offPaletteConfirmMs` = 10 ms more;
- `offPaletteOctaveConfirmMs` = 20 ms more on top, when it lies 12 or 24 semitones from a palette note (the
  typical octave error).

This applies to Note On from silence, after an onset, and to changes without an onset. The override threshold
is therefore explicit: consistent acoustic evidence for 10 ms (30 ms for octave relatives) always beats the
palette. No note is suppressed.

Palette notes are decided exactly as in Free mode. A palette change never ends a sounding note. Diagnostics:
`getPaletteDelayedDecisions()`, `Result::paletteRelieved`.

Latency impact:
- palette notes: lower (see below);
- off-palette notes: +10 ms, or +30 ms when they are octave relatives of palette notes;
- CPU: at most the free tracker's work per frame. The scan continues to the octave-safe rung exactly as without a
  palette.

## Results (synthetic corpus, 44.1/48/96 kHz, 1,200 single plucks per setup)

The palettes are built around the notes actually played:
- `palette-in`: the played notes plus a minor-pentatonic neighbourhood;
- `palette-off-chromatic`: neighbours only, as when a chromatic passing tone is played;
- `palette-off-written-octave-up/down`: the written part lies an octave above or below what is played. This is
  the hardest case, because the written note *is* the octave error.

Single plucks at 48 kHz, latency of the first Note On (p50 / p95). The 44.1 and 96 kHz rows are equivalent. All
setups at all rates:
- correct first note in 1,200 of 1,200 plucks;
- no octave or other wrong notes;
- no extra Note Ons;
- no Note Ons on noise.

| Setup | E string | A string | D/G strings |
|---|---|---|---|
| free | 39.5 / 39.5 | 32.0 / 34.5 | 22.0 / 39.5 |
| palette-in | 39.5 / 39.5 | 32.0 / 34.5 | **14.5 / 27.0** |
| palette-off-chromatic | 49.5 / 49.5 | 42.0 / 44.5 | 32.0 / 49.5 |
| palette-off-written-octave-up/down | 69.5 / 69.5 | 62.0 / 64.5 | 52.0 / 69.5 |

Two-note sequences: all pass in every setup, except `repeat-ringing`. That is the known limitation of the
current onset detector and is unchanged with or without a palette.

| Setup | change-fourth p50 / p95 | change-octave p50 / p95 |
|---|---|---|
| free | 32.0 / 50.6 | 60.6 / 80.5 |
| palette-in | 29.5 / 40.6 | 60.6 / 80.5 |
| palette-off-chromatic | 43.1 / 63.1 | 73.1 / 93.0 |
| palette-off-written-octave | 64.5 / 85.5 | 94.5 / 115.5 |

Reading:
- For the notes of the song, the palette brings the D/G-string median from 22 to 14.5 ms and p95 from 39.5 to
  27 ms. This is the first configuration whose median is near the D/G target.
- E1–C#2 cannot gain: their sub-octave lies below the operating range, so they are already octave-safe on the
  shortest window that can measure them.
- The cost falls only on notes the song does not contain. They are bounded and still correct.
- The synthetic corpus produces no octave errors even in Free mode. So it shows the price of the octave penalty,
  not its benefit. Whether 10/20 ms is the right trade-off must be measured on real DI takes with a song file:
  - a passage played as written;
  - deliberate variations and chromatic notes;
  - a part played an octave off.

## Open points

- Real-take evaluation with the user's songs (correct-note rate, octave errors, latency, delayed decisions)
  in Free vs. palette mode, as required by CLAUDE.md.
- Key/scale mode would reuse the same palette mechanism: pitch classes over the range.
- Timeline-guided mode is deferred until the palette is validated live.
