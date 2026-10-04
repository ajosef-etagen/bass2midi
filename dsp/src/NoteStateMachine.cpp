#include "bass2midi/NoteStateMachine.h"
#include "bass2midi/MidiNoteUtils.h"

#include <algorithm>
#include <cmath>

namespace bass2midi
{
    namespace
    {
        bool isOctaveJump (int from, int to) noexcept
        {
            const int distance = std::abs (to - from);
            return distance == 12 || distance == 24;
        }
    }

    bool NoteStateMachine::Settings::isValid() const noexcept
    {
        const auto nonNegative = [] (double v) { return v >= 0.0 && std::isfinite (v); };

        return frameIntervalSeconds > 0.0 && frameIntervalSeconds < 1.0
            && nonNegative (onsetSettleMs)
            && midiChannel >= 1 && midiChannel <= 16
            && lowestNote >= 0 && highestNote <= 127 && lowestNote <= highestNote
            && transposeSemitones >= -48 && transposeSemitones <= 48
            && minClarity > 0.0 && minClarity <= 1.0
            && toleranceCents > 0.0 && toleranceCents < 50.0
            && envelopeHoldMs > 0.0 && std::isfinite (envelopeHoldMs)
            && std::isfinite (gateOpenDbfs) && gateOpenDbfs <= 0.0
            && nonNegative (gateHysteresisDb)
            && nonNegative (releaseHoldMs) && nonNegative (unvoicedReleaseMs)
            && attackFrames >= 1
            && nonNegative (noteChangeConfirmMs) && nonNegative (octaveChangeConfirmMs)
            && onsetRiseDb > 0.0 && std::isfinite (onsetRiseDb)
            && onsetLookbackMs > 0.0 && std::isfinite (onsetLookbackMs)
            && nonNegative (onsetValidMs)
            && std::isfinite (velocityFloorDbfs) && std::isfinite (velocityCeilDbfs)
            && velocityCeilDbfs > velocityFloorDbfs
            && minVelocity >= 1 && minVelocity <= 127;
    }

    int NoteStateMachine::framesFor (double ms) const noexcept
    {
        return std::max (1, static_cast<int> (std::ceil (ms * 1.0e-3 / settings.frameIntervalSeconds - 1.0e-9)));
    }

    bool NoteStateMachine::setSettings (const Settings& newSettings) noexcept
    {
        if (! newSettings.isValid())
            return false;

        settings = newSettings;
        releaseHoldFrames = framesFor (settings.releaseHoldMs);
        unvoicedReleaseFrames = framesFor (settings.unvoicedReleaseMs);
        changeFrames = std::max (settings.attackFrames, framesFor (settings.noteChangeConfirmMs));
        octaveFrames = std::max (changeFrames, framesFor (settings.octaveChangeConfirmMs));
        lookbackFrames = std::min (maxLookbackFrames - 1, framesFor (settings.onsetLookbackMs));
        holdFrames = std::min (maxLookbackFrames, framesFor (settings.envelopeHoldMs));
        onsetValidFrames = framesFor (settings.onsetValidMs);
        settleFrames = framesFor (settings.onsetSettleMs);
        return true;
    }

    void NoteStateMachine::reset() noexcept
    {
        soundingNote = -1;
        soundingOutputNote = -1;
        lastVelocity = 0;
        candidateNote = -1;
        candidateFrames = 0;
        quietFrames = 0;
        unvoicedFrames = 0;
        onsetFramesLeft = 0;
        framesSinceOnset = 1 << 20;
        attackPeakLinear = 0.0;
        ringWritePos = 0;
        ringCount = 0;
    }

    double NoteStateMachine::toDbfs (double linear) noexcept
    {
        return linear > 1.0e-9 ? 20.0 * std::log10 (linear) : -180.0;
    }

    int NoteStateMachine::velocityFromPeak (double peakLinear, const Settings& s) noexcept
    {
        const double position = (toDbfs (peakLinear) - s.velocityFloorDbfs) / (s.velocityCeilDbfs - s.velocityFloorDbfs);
        const double clamped = std::clamp (position, 0.0, 1.0);
        return s.minVelocity + static_cast<int> (std::lround (clamped * (127 - s.minVelocity)));
    }

    void NoteStateMachine::addNoteOff (Output& out) noexcept
    {
        out.events[static_cast<size_t> (out.count++)] = { NoteEvent::Type::noteOff, soundingChannel, soundingOutputNote, 0 };
        soundingNote = -1;
        soundingOutputNote = -1;
        quietFrames = 0;
        unvoicedFrames = 0;
    }

    void NoteStateMachine::addNoteOn (Output& out, int note) noexcept
    {
        lastVelocity = velocityFromPeak (attackPeakLinear, settings);
        soundingNote = note;
        soundingOutputNote = note + settings.transposeSemitones;
        soundingChannel = settings.midiChannel;
        out.events[static_cast<size_t> (out.count++)] = { NoteEvent::Type::noteOn, soundingChannel, soundingOutputNote, lastVelocity };

        candidateNote = -1;
        candidateFrames = 0;
        quietFrames = 0;
        unvoicedFrames = 0;
        onsetFramesLeft = 0; // this attack is consumed
    }

    NoteStateMachine::Output NoteStateMachine::processFrame (const FrameFeatures& frame) noexcept
    {
        Output out;
        const double gateCloseDb = settings.gateOpenDbfs - settings.gateHysteresisDb;
        const auto ringAt = [this] (const std::array<double, maxLookbackFrames>& ring, int framesAgo)
        {
            return ring[static_cast<size_t> ((ringWritePos - framesAgo + maxLookbackFrames) % maxLookbackFrames)];
        };

        // Envelope: max hop peak over the hold span (this frame included).
        const double hopDb = toDbfs (frame.hopPeakLinear);
        double levelDb = hopDb;
        for (int i = 1; i < std::min (holdFrames, ringCount + 1); ++i)
            levelDb = std::max (levelDb, ringAt (hopPeakDb, i));

        // 1. Onset: rise over the lowest envelope in the lookback window (refractory for one lookback).
        double recentMinDb = levelDb;
        for (int i = 1; i <= std::min (lookbackFrames, ringCount); ++i)
            recentMinDb = std::min (recentMinDb, ringAt (envelopeDb, i));

        hopPeakDb[static_cast<size_t> (ringWritePos)] = hopDb;
        envelopeDb[static_cast<size_t> (ringWritePos)] = levelDb;
        ringWritePos = (ringWritePos + 1) % maxLookbackFrames;
        ringCount = std::min (ringCount + 1, maxLookbackFrames);

        ++framesSinceOnset;
        const bool onset = levelDb >= settings.gateOpenDbfs
                        && levelDb - recentMinDb >= settings.onsetRiseDb
                        && framesSinceOnset > lookbackFrames;
        if (onset)
        {
            onsetFramesLeft = onsetValidFrames;
            framesSinceOnset = 0;
            attackPeakLinear = 0.0; // velocity measures this attack only
        }
        else if (onsetFramesLeft > 0)
        {
            --onsetFramesLeft;
        }

        attackPeakLinear = std::max (attackPeakLinear, frame.hopPeakLinear);

        // 2. Release on sustained low level.
        if (soundingNote >= 0)
        {
            quietFrames = levelDb < gateCloseDb ? quietFrames + 1 : 0;
            if (quietFrames >= releaseHoldFrames)
            {
                addNoteOff (out);
                attackPeakLinear = 0.0; // back to silence: the next note measures its own attack
                candidateNote = -1;
                candidateFrames = 0;
                return out;
            }
        }

        // 3. Is this frame countable pitch evidence?
        const double levelNeeded = soundingNote >= 0 ? gateCloseDb : settings.gateOpenDbfs;
        int note = -1;
        bool withinTolerance = false;

        if (frame.pitch.valid && frame.pitch.clarity >= settings.minClarity && levelDb >= levelNeeded
            && frame.pitch.frequencyHz > 0.0)
        {
            const double exact = midi::noteFromFrequencyHz (frame.pitch.frequencyHz);
            note = static_cast<int> (std::lround (exact));
            withinTolerance = std::abs (exact - note) * 100.0 <= settings.toleranceCents;
            const int outputNote = note + settings.transposeSemitones;
            if (note < settings.lowestNote || note > settings.highestNote || outputNote < 0 || outputNote > 127)
                note = -1;
        }

        if (note < 0)
        {
            candidateNote = -1;
            candidateFrames = 0;
            if (soundingNote >= 0 && ++unvoicedFrames >= unvoicedReleaseFrames)
            {
                addNoteOff (out);
                attackPeakLinear = 0.0;
            }
            return out;
        }

        unvoicedFrames = 0;

        if (! withinTolerance)
        {
            // Between semitones (slide, bend, attack transient): neither evidence for a note nor silence.
            candidateFrames = 0;
            return out;
        }

        if (note == candidateNote)
            ++candidateFrames;
        else
        {
            candidateNote = note;
            candidateFrames = 1;
        }

        // 4. Decisions.
        if (soundingNote < 0)
        {
            if (candidateFrames >= settings.attackFrames)
                addNoteOn (out, note);
            return out;
        }

        if (note == soundingNote)
        {
            // Same pitch: re-attack only after an onset, once the window holds mostly the new attack
            // (settleFrames counted including the onset frame).
            if (onsetFramesLeft > 0 && framesSinceOnset + 1 >= settleFrames && candidateFrames >= settings.attackFrames)
            {
                addNoteOff (out);
                addNoteOn (out, note);
            }
            return out;
        }

        const int required = onsetFramesLeft > 0 ? settings.attackFrames
                           : isOctaveJump (soundingNote, note) ? octaveFrames
                                                               : changeFrames;
        if (candidateFrames >= required)
        {
            addNoteOff (out);
            addNoteOn (out, note);
        }

        return out;
    }

    NoteStateMachine::Output NoteStateMachine::allNotesOff() noexcept
    {
        Output out;
        if (soundingNote >= 0)
            addNoteOff (out);
        reset();
        return out;
    }
}
