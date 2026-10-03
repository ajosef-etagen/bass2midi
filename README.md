# Bass2MIDI

Real-time, monophonic electric bass to MIDI for live use on macOS:

```text
Electric bass -> audio interface -> Bass2MIDI (standalone app) -> virtual MIDI port "Bass2MIDI" -> MainStage
```

**Status: Phase 0.** The app opens the audio interface, publishes the virtual MIDI port, can send a test note and
shows a live reference pitch readout and timing diagnostics. It does **not** convert bass to MIDI yet: the note
state machine is Phase 1. See [`docs/phase0-baseline.md`](docs/phase0-baseline.md) for the reference measurements
and [`CLAUDE.md`](CLAUDE.md) for goals, architecture and the development plan.

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

## Checking the MainStage route (Phase 0)

1. Start Bass2MIDI and pick the audio interface and the bass input channel.
2. In MainStage, set a software instrument channel strip's MIDI input to `Bass2MIDI`.
3. Click **Send test note (C3)** in Bass2MIDI: the MainStage instrument should play a short C3.

## Licensing note

Bass2MIDI's own license is not chosen yet. JUCE is dual-licensed (AGPLv3 or a commercial JUCE license); the chosen
JUCE license determines how the app may be distributed.
