#pragma once

#include "bass2midi/NotePalette.h"
#include "bass2midi/PitchCandidate.h"

#include <array>
#include <cstdint>

namespace bass2midi
{
    struct NoteEvent
    {
        enum class Type : std::uint8_t { noteOn, noteOff };

        Type type = Type::noteOn;
        int channel = 1;   // 1..16
        int note = 0;      // 0..127
        int velocity = 0;  // 1..127 for Note On, 0 for Note Off
    };

    // Per-frame input: the pitch estimate of the analysis window plus a short-term level.
    struct FrameFeatures
    {
        PitchCandidate pitch;
        double hopPeakLinear = 0.0; // max |x| over the newest hop of the frame (the samples new since the last frame)
    };

    // The single authority over the MIDI note lifecycle (Note On / Note Off / velocity).
    //
    // Guarantees: at most one sounding note; every Note On is matched by exactly one Note Off on the
    // same channel and note; a note change always emits Note Off before Note On; no Note On is
    // repeated for one sustained pluck unless a new attack (onset) is detected.
    //
    // Level: the envelope is the highest hop peak over the last envelopeHoldMs. The hold must exceed
    // the longest period (E1: 24.3 ms); a bare 2.5 ms hop peak ripples by > 10 dB within one period
    // of a low note and would look like new attacks. Adds up to envelopeHoldMs to Note Off latency.
    //
    // Per frame (see processFrame):
    //  1. Onset: the envelope rises by >= onsetRiseDb over its lowest value in the last onsetLookbackMs.
    //     An onset arms a re-attack decision and restarts the velocity peak measurement.
    //  2. Release: Note Off only on sustained low level (envelope below gateOpenDbfs - gateHysteresisDb for
    //     releaseHoldMs) or on unvoiced frames for unvoicedReleaseMs. A single uncertain pitch frame
    //     never ends a note.
    //  3. Pitch: frames count only if YIN clarity >= minClarity and the estimate lies within
    //     toleranceCents of a semitone inside [lowestNote, highestNote] (implicit hysteresis: a
    //     neighbouring note must be >= 100 - toleranceCents cents away from the current one).
    //  4. Note On from silence after attackFrames consistent frames. While a note sounds, a different
    //     note needs noteChangeConfirmMs (octave jumps: octaveChangeConfirmMs) of consistent frames,
    //     or only attackFrames when an onset is pending. The same note is re-attacked when an onset
    //     is pending and the analysis window has settled on the new attack (onsetSettleMs).
    //
    //  5. Song palette (optional, setPalette): a note outside a non-empty palette needs
    //     offPaletteConfirmMs more consistent frames for any Note On or change, plus
    //     offPaletteOctaveConfirmMs when it lies 12/24 semitones from a palette note (the typical
    //     octave error). This is a bounded delay, never a rejection: consistent acoustic evidence for
    //     that long always overrides the palette. Palette notes are decided exactly as in Free mode.
    //
    // Real-time: no allocation, no locks; processFrame is O(onset lookback) per frame.
    class NoteStateMachine
    {
    public:
        struct Settings
        {
            double frameIntervalSeconds = 0.0025; // analysis hop duration (set by the host)
            double onsetSettleMs = 40.0;          // wait after an onset before judging a same-note re-attack;
                                                  // ~0.75 x analysis window so the window holds mostly new signal
            int midiChannel = 1;                  // 1..16
            int lowestNote = 26;                  // D1: E1 minus a whole tone for detuned/slack strings
            int highestNote = 69;                 // A4: G4 plus a whole tone
            int transposeSemitones = 0;           // added to the played note on output, -48..48; notes that
                                                  // would leave 0..127 are not played
            double minClarity = 0.80;             // YIN clarity (1 - d') required to count a frame
            double toleranceCents = 40.0;         // max distance of a counted frame from its semitone
            double envelopeHoldMs = 25.0;         // level = max hop peak over this span (> longest period)
            double gateOpenDbfs = -45.0;          // envelope level needed to start a note
            double gateHysteresisDb = 6.0;        // release threshold = gateOpen - hysteresis
            double releaseHoldMs = 30.0;          // low level this long -> Note Off
            double unvoicedReleaseMs = 200.0;     // no countable pitch this long -> Note Off
            int attackFrames = 2;                 // consistent frames for Note On from silence / after onset
            double noteChangeConfirmMs = 10.0;    // consistent new note while sounding, no onset
            double octaveChangeConfirmMs = 40.0;  // same, for +-12/24 semitone jumps (high-cost transition)
            double changeMinClarity = 0.93;       // a frame only counts towards a change without onset at this clarity
            double changeMaxDropDb = 20.0;        // ... and with the analysis-window RMS at most this far below the
                                                  // highest window RMS since the note started (release glides while a
                                                  // finger lifts are quieter and murkier). Window RMS, not the hop
                                                  // envelope: the envelope holds the old level for envelopeHoldMs.
            double onsetRiseDb = 9.0;             // envelope rise that counts as a new attack
            double onsetLookbackMs = 20.0;        // window for the rise reference (lowest recent envelope)
            double onsetValidMs = 120.0;          // how long an onset may still trigger a (re)attack
            double velocityFloorDbfs = -40.0;     // attack peak mapped to minVelocity
            double velocityCeilDbfs = -6.0;       // attack peak mapped to 127
            int minVelocity = 20;                 // 1..127
            double offPaletteConfirmMs = 10.0;    // extra confirmation for a note outside a non-empty song palette
            double offPaletteOctaveConfirmMs = 20.0; // further extra when it is an octave from a palette note

            bool isValid() const noexcept;
        };

        struct Output
        {
            std::array<NoteEvent, 2> events {};
            int count = 0;
        };

        enum class State : std::uint8_t { idle, sounding };

        // Real-time safe. Invalid settings are rejected (returns false) and the previous ones kept.
        // Changing the MIDI channel or transposition never orphans a note: its Note Off uses the
        // channel and output note of its Note On.
        bool setSettings (const Settings& newSettings) noexcept;
        const Settings& getSettings() const noexcept { return settings; }

        // Real-time safe. Played-note space (before transposition); an empty palette is Free mode.
        // Takes effect for the next decision; a sounding note is never ended by a palette change.
        void setPalette (const NotePalette& newPalette) noexcept { palette = newPalette; }
        const NotePalette& getPalette() const noexcept { return palette; }

        // Real-time safe. Clears all state WITHOUT emitting events; use allNotesOff to end a note.
        void reset() noexcept;

        Output processFrame (const FrameFeatures& frame) noexcept;

        // Ends the sounding note (if any) and resets. Call on stop, device change, or panic.
        Output allNotesOff() noexcept;

        // Diagnostics (read on the same thread, or copied out through atomics by the host).
        State getState() const noexcept { return soundingNote >= 0 ? State::sounding : State::idle; }
        int getSoundingNote() const noexcept { return soundingNote; }             // played (detected) note
        int getSoundingOutputNote() const noexcept { return soundingOutputNote; } // note sent as MIDI
        int getLastVelocity() const noexcept { return lastVelocity; }
        bool isOnsetPending() const noexcept { return onsetFramesLeft > 0; }
        // Decisions the palette postponed: a candidate reached the Free-mode frame count but was off-palette.
        // Counts since construction (not cleared by reset).
        int getPaletteDelayedDecisions() const noexcept { return paletteDelayedDecisions; }

        // Pure helpers, exposed for tests.
        static int velocityFromPeak (double peakLinear, const Settings& s) noexcept;
        static double toDbfs (double linear) noexcept;

    private:
        static constexpr int maxLookbackFrames = 64;

        int framesFor (double ms) const noexcept;
        void addNoteOff (Output& out) noexcept;
        void addNoteOn (Output& out, int note) noexcept;
        int palettePenaltyFrames (int note) const noexcept;
        bool decide (int candidateCount, int freeRequired, int note) noexcept;

        Settings settings;

        // Derived frame counts (from settings.frameIntervalSeconds).
        int releaseHoldFrames = 12, unvoicedReleaseFrames = 80, changeFrames = 4, octaveFrames = 16;
        int lookbackFrames = 8, holdFrames = 10, onsetValidFrames = 48, settleFrames = 16;
        int offPaletteFrames = 4, offPaletteOctaveFrames = 12;
        NotePalette palette;
        int paletteDelayedDecisions = 0;

        int soundingNote = -1, soundingOutputNote = -1, soundingChannel = 1, lastVelocity = 0;
        int candidateNote = -1, candidateFrames = 0;
        int quietFrames = 0, unvoicedFrames = 0;
        int onsetFramesLeft = 0, framesSinceOnset = 1 << 20;
        double attackPeakLinear = 0.0;
        double noteRmsPeakLinear = 0.0; // highest analysis-window RMS since the sounding note started

        // Ring buffers of the last maxLookbackFrames hop peaks and envelope values (dBFS).
        std::array<double, maxLookbackFrames> hopPeakDb {}, envelopeDb {};
        int ringWritePos = 0, ringCount = 0;
    };
}
