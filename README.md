# Bass2MIDI

Real-time, monophonic electric bass to MIDI for live use on macOS:

```text
Electric bass -> audio interface -> Bass2MIDI (standalone app) -> virtual MIDI port "Bass2MIDI" -> MainStage
```

**Status: Phase 1, first playable version.** The app converts the bass on the selected input channel into MIDI
Note On/Off with velocity on the virtual port `Bass2MIDI`. Known limits of this version (measured in
[`docs/phase1-statemachine.md`](docs/phase1-statemachine.md)):

- Note On latency is about one analysis window, roughly 50 ms (plain YIN; faster attack detection is Phase 2).
- Re-plucking the **same** note while it still rings is often merged into one note; mute briefly between repeated
  notes. Octave jumps without a clear new attack take about 40 ms longer (octave-error protection).
- Only synthetic signals have been measured so far; real DI recordings are needed for tuning.

See [`docs/phase0-baseline.md`](docs/phase0-baseline.md) for the reference measurements and [`CLAUDE.md`](CLAUDE.md)
for goals, architecture and the development plan.

## Build

Requirements: CMake ≥ 3.22, a C++20 compiler, Ninja (optional). JUCE 8 and doctest are fetched by CMake.

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
```

On Linux the app also builds (needs `libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev`), but macOS is the target platform.

## Using it with MainStage

1. Start Bass2MIDI **before** MainStage (the virtual port exists only while the app runs).
2. Pick the audio interface, enable the bass input channel and select it under **Analysed input**.
   Recommended: 48 kHz, 64 or 128 samples buffer; use the same sample rate in MainStage.
3. Set **Gate**: mute the strings, read the noise level under *Analysed input → peak*, and set the gate a few dB
   above it (default −45 dBFS). Plucks must exceed the gate to start a note.
4. In MainStage, set a software instrument channel strip's MIDI input to `Bass2MIDI` and the channel to the one
   chosen in Bass2MIDI (default 1).
5. **Send test note (C3)** checks the route; then play. The big readout shows the MIDI note being sent.

The virtual port does not appear in Audio MIDI Setup's MIDI Studio window; it is listed in the MIDI input choices of
other apps (MainStage, or e.g. the free *MIDI Monitor* for checking).

## Licensing note

Bass2MIDI's own license is not chosen yet. JUCE is dual-licensed (AGPLv3 or a commercial JUCE license); the chosen
JUCE license determines how the app may be distributed.
