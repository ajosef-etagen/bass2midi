# Bass2MIDI

Real-time, monophonic electric bass to MIDI for live use on macOS:

```text
Electric bass -> audio interface -> Bass2MIDI (standalone app) -> virtual MIDI port "Bass2MIDI" -> MainStage
```

**Status: Phase 2 (part 1) plus song palette.** The app converts the bass on the selected input channel into MIDI
Note On/Off with velocity on the virtual port `Bass2MIDI`, using pitch-adaptive analysis windows. Optionally a
song's Guitar Pro file (.gp / .gp5) supplies the notes of the bass part as a soft prior
([`docs/phase5-palette.md`](docs/phase5-palette.md)). Known limits (measured on a
synthetic corpus, see [`docs/phase2-multires.md`](docs/phase2-multires.md) and
[`docs/phase1-statemachine.md`](docs/phase1-statemachine.md)):

- Note On latency: about 15 ms for high notes, 22 ms median on the D/G strings, 32 ms on the A string,
  40 ms on the low E string.
- Re-plucking the **same** note while it still rings is merged into one note; mute briefly between repeated notes.
  Octave jumps without a clear new attack take about 30 ms longer (octave-error protection).
- Use 48 kHz (or at least 128 samples buffer at 96 kHz): low notes cost up to ~0.7 ms per analysis frame.
- With a song selected, notes of the song are found sooner (D/G strings: 14.5 ms median instead of 22 ms); notes
  that are *not* in the song still play, but about 10 ms later (30 ms if they are an octave of a song note).
- Only synthetic signals have been measured so far; real DI recordings are needed for tuning.

See [`docs/phase0-baseline.md`](docs/phase0-baseline.md) for the reference measurements and [`CLAUDE.md`](CLAUDE.md)
for goals, architecture and the development plan.

## Build

Requirements: CMake ≥ 3.22, a C++20 compiler, Ninja (optional), zlib (part of macOS; `zlib1g-dev` on Debian/Ubuntu).
JUCE 8 and doctest are fetched by CMake.

```sh
# macOS: app + tests
cmake -S . -B build -G Ninja
cmake --build build
open build/app/Bass2MIDIApp_artefacts/Release/Bass2MIDI.app

# DSP library, tests and evaluation only (any OS, no JUCE)
cmake -S . -B build -G Ninja -DBASS2MIDI_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure
./build/bass2midi_baseline --quick
./build/bass2midi_songinfo song.gp5        # tracks and bass palette of a Guitar Pro file
```

On Linux the app also builds (needs `libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev`), but macOS is the target platform.

## Using it with MainStage

1. Start Bass2MIDI **before** MainStage (the virtual port exists only while the app runs).
2. Pick the audio interface, enable the bass input channel and select it under **Analysed input**.
   Recommended: 48 kHz, 64 or 128 samples buffer; use the same sample rate in MainStage.
3. Set **Gate**: mute the strings, read the noise level under *Analysed input → peak*, and set the gate a few dB
   above it (default −45 dBFS). Plucks must exceed the gate to start a note.
4. **Transpose** (±24 semitones, double-click resets to 0) shifts the sent notes, e.g. +12 for bass patches that
   are mapped an octave low. Detection is unaffected; a sounding note keeps its output note until its Note Off.
5. In MainStage, set a software instrument channel strip's MIDI input to `Bass2MIDI` and the channel to the one
   chosen in Bass2MIDI (default 1).
6. **Test note (MIDI 48)** checks the route; then play. The big readout shows the MIDI note being sent.
7. Optional, per song: **Add songs...** adds Guitar Pro files (`.gp` from Guitar Pro 7/8, `.gp5` from Guitar Pro 5)
   to the song list; **Song** (or the `<` / `>` buttons) selects the current song, **Bass track** the part to use
   (picked automatically). The palette line shows the notes Bass2MIDI now expects. **Free (no song guidance)** is
   the default and always one click away; a missing or unreadable file also means Free mode. The list and the
   selection are saved. Guidance is soft: notes outside the song are delayed slightly, never suppressed.

Note names: Bass2MIDI calls MIDI 60 "C4" (scientific pitch, open low E = E1 = MIDI 28). Logic and MainStage call
MIDI 60 "C3" by default, so the same notes appear one octave lower there (open low E = "E0"); MainStage can be
switched to the C4 convention in its MIDI settings. The MIDI numbers are identical; the readout shows the number.

The virtual port does not appear in Audio MIDI Setup's MIDI Studio window; it is listed in the MIDI input choices of
other apps (MainStage, or e.g. the free *MIDI Monitor* for checking).

## Licensing note

Bass2MIDI's own license is not chosen yet. JUCE is dual-licensed (AGPLv3 or a commercial JUCE license); the chosen
JUCE license determines how the app may be distributed.
