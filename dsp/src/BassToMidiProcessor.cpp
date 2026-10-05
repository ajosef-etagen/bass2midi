#include "bass2midi/BassToMidiProcessor.h"
#include "bass2midi/MidiNoteUtils.h"

#include <algorithm>
#include <cmath>

namespace bass2midi
{
    bool BassToMidiProcessor::prepare (double newSampleRateHz, const Settings& newSettings)
    {
        if (! (newSampleRateHz > 0.0) || ! (newSettings.hopSeconds > 0.0) || newSettings.validationFrames < 1)
            return false;

        settings = newSettings;
        sampleRateHz = newSampleRateHz;
        const int hop = std::max (1, static_cast<int> (std::lround (sampleRateHz * settings.hopSeconds)));
        if (! tracker.prepare (sampleRateHz, settings.tracker) || ! framer.prepare (tracker.getFrameSamples(), hop))
            return false;

        settings.notes.frameIntervalSeconds = static_cast<double> (hop) / sampleRateHz;
        settings.notes.onsetSettleMs = settings.onsetSettleWindowFraction * 1000.0 * tracker.getFrameSamples() / sampleRateHz;
        if (! noteMachine.setSettings (settings.notes))
            return false;
        noteMachine.reset();

        settings.attacks.gateDbfs = settings.notes.gateOpenDbfs;
        if (! attackDetector.prepare (sampleRateHz, settings.attacks) || ! follower.setSettings (settings.follower)
            || ! barPlayer.setSettings (settings.bars))
            return false;

        validationFramesNeeded = settings.validationFrames;
        validationWindowSamples = static_cast<int> (std::lround (settings.validationWindowMs * 1.0e-3 * sampleRateHz));
        unpitchedDecisionSamples = tracker.getFrameSamples()
                                 + static_cast<int> (std::lround (settings.unpitchedDecisionMs * 1.0e-3 * sampleRateHz));
        samplePosition = 0;
        validating = false;
        acceptedAttackPeak = 0.0;
        acceptedAttackSample = -(std::int64_t { 1 } << 40);
        diagnostics = {};
        return true;
    }

    bool BassToMidiProcessor::setNoteSettings (int midiChannel, double gateOpenDbfs, int transposeSemitones) noexcept
    {
        auto updated = settings.notes;
        updated.midiChannel = midiChannel;
        updated.gateOpenDbfs = gateOpenDbfs;
        updated.transposeSemitones = transposeSemitones;
        if (! noteMachine.setSettings (updated))
            return false;
        settings.notes = updated;
        attackDetector.setGateDbfs (gateOpenDbfs);
        return true;
    }

    void BassToMidiProcessor::setPalette (const NotePalette& palette) noexcept
    {
        tracker.setPalette (palette);
        noteMachine.setPalette (palette);
    }

    void BassToMidiProcessor::setTimeline (const ExpectedNote* notes, int count) noexcept
    {
        setTimeline (notes, count, nullptr, 0);
    }

    void BassToMidiProcessor::setTimeline (const ExpectedNote* notes, int count, const TimelineBar* bars, int barCount) noexcept
    {
        follower.setTimeline (notes, count);
        barPlayer.setTimeline (notes, count, bars, barCount);
        timelineBarCount = bars != nullptr && notes != nullptr && count > 0 ? barCount : 0;
        pendingBarStart = 0;
        validating = false;
    }

    void BassToMidiProcessor::emitBar (const BarPlayer::Output& o, std::int64_t sample, Output& out) noexcept
    {
        for (int i = 0; i < o.count && out.count < maxEventsPerBlock; ++i)
        {
            const auto& e = o.events[static_cast<std::size_t> (i)];
            out.events[static_cast<std::size_t> (out.count++)] = { e, sample, e.type == NoteEvent::Type::noteOn, false };
        }
    }

    void BassToMidiProcessor::setSongPosition (int index) noexcept
    {
        follower.setPosition (index);
        validating = false;
    }

    void BassToMidiProcessor::emit (const NoteStateMachine::Output& o, std::int64_t sample, bool predicted, bool correction, Output& out) noexcept
    {
        for (int i = 0; i < o.count && out.count < maxEventsPerBlock; ++i)
        {
            const auto& e = o.events[static_cast<std::size_t> (i)];
            const bool on = e.type == NoteEvent::Type::noteOn;
            out.events[static_cast<std::size_t> (out.count++)] = { e, sample, predicted && on, correction && on };
        }
        diagnostics.soundingPlayedNote = noteMachine.getSoundingNote();
        diagnostics.soundingOutputNote = noteMachine.getSoundingOutputNote();
        diagnostics.lastVelocity = noteMachine.getLastVelocity();
    }

    void BassToMidiProcessor::allNotesOff (Output& out) noexcept
    {
        const auto off = noteMachine.allNotesOff();
        if (! noteMachineSilent)
            emit (off, samplePosition, false, false, out);
        noteMachineSilent = false;
        BarPlayer::Output bo;
        if (barActive())
            barPlayer.start (barPlayer.getBar(), bo); // end the note, wait for the next downbeat
        else
            barPlayer.stop (bo);
        emitBar (bo, samplePosition, out);
        validating = false;
    }

    void BassToMidiProcessor::process (const float* samples, int numSamples, Output& out) noexcept
    {
        if (sampleRateHz <= 0.0 || numSamples <= 0)
            return;
        const std::int64_t blockEnd = samplePosition + numSamples;

        // Mode changes and bar-playback (re)starts. The bar player owns its notes; leaving bar mode
        // ends them.
        if (mode != appliedMode)
        {
            if (appliedMode == Mode::bar)
            {
                BarPlayer::Output bo;
                barPlayer.stop (bo);
                emitBar (bo, blockEnd, out);
            }
            if (mode == Mode::bar && pendingBarStart < 0)
                pendingBarStart = std::max (0, barPlayer.getBar());
            appliedMode = mode;
            validating = false;
        }
        barPlayer.setOutput (settings.notes.midiChannel, settings.notes.transposeSemitones);
        attackDetector.setUsePeriodicity (settings.attacks.usePeriodicity || (barActive() && settings.barModePeriodicity));
        if (barActive() && pendingBarStart >= 0)
        {
            BarPlayer::Output bo;
            barPlayer.start (pendingBarStart, bo);
            emitBar (bo, blockEnd, out);
            pendingBarStart = -1;
        }
        // Bar playback output starts (anchor or resumed trust): end a Free-mode note that was sent;
        // from then on the Free path runs silently (its notes are the player's, used for the
        // periodicity period) until Free output takes over again.
        const auto endFreeNoteForBar = [&]
        {
            if (barActive() && barPlayer.isOutputting() && ! noteMachineSilent)
            {
                if (noteMachine.getSoundingNote() >= 0)
                    emit (noteMachine.allNotesOff(), blockEnd, false, false, out);
                noteMachineSilent = true;
            }
        };
        endFreeNoteForBar();

        // 1. Attacks (all modes, for diagnostics; Song Mode acts on them at once).
        AttackDetector::Attack attack;
        if (attackDetector.process (samples, numSamples, attack))
        {
            ++diagnostics.attackCount;
            diagnostics.lastAttackSample = attack.detectedAtSample;
            diagnostics.lastAttackVelocity = attack.velocity;

            const bool recentReference = attack.detectedAtSample - acceptedAttackSample
                                      <= static_cast<std::int64_t> (settings.ghostReferenceSeconds * sampleRateHz);
            const bool ghost = recentReference
                            && attack.peakLinear < acceptedAttackPeak * std::pow (10.0, -settings.ghostRelativeDb / 20.0);
            if (barActive())
            {
                BarPlayer::Output bo;
                if (barPlayer.onAttack (seconds (attack.detectedAtSample), attack.velocity, bo))
                {
                    endFreeNoteForBar();
                    if (barPlayer.isOutputting())
                        diagnostics.lastTriggerLatencySamples = blockEnd - attack.detectedAtSample;
                    validating = true; // verdict on the anchor pitch
                    validationAttackSample = attack.detectedAtSample;
                    validationIndex = barAnchorVerdict;
                    validationCandidate = -1;
                    validationRun = 0;
                    validationPredicted = false;
                    validationVoiced = false;
                }
                emitBar (bo, blockEnd, out);
            }
            if (songActive() && ghost)
                ++diagnostics.ghostAttacks;
            if (songActive() && ! ghost)
            {
                acceptedAttackPeak = attack.peakLinear;
                acceptedAttackSample = attack.detectedAtSample;
                const auto prediction = follower.onAttack (seconds (attack.detectedAtSample));
                diagnostics.lastPredictedIndex = prediction.index;
                diagnostics.lastPredictedNote = prediction.valid ? prediction.midiNote : -1;
                if (prediction.valid)
                {
                    const auto o = noteMachine.triggerPredicted (prediction.midiNote, attack.velocity);
                    if (o.count > 0)
                    {
                        emit (o, blockEnd, true, false, out);
                        ++diagnostics.predictions;
                        diagnostics.lastTriggerLatencySamples = blockEnd - attack.detectedAtSample;
                    }
                }
                validating = true;
                validationAttackSample = attack.detectedAtSample;
                validationIndex = prediction.index;
                validationCandidate = -1;
                validationRun = 0;
                validationPredicted = prediction.valid;
                validationVoiced = false;
            }
        }

        if (barActive())
        {
            BarPlayer::Output bo;
            barPlayer.advance (seconds (blockEnd), bo);
            emitBar (bo, blockEnd, out);
        }

        // 2. Pitch path.
        framer.push (samples, numSamples, [&] (const float* frame, std::int64_t frameEnd, int)
        {
            handleFrame (frame, frameEnd, blockEnd, out);
        });
        samplePosition = blockEnd;

        diagnostics.songActive = songActive();
        diagnostics.position = follower.getPosition();
        diagnostics.timelineCount = follower.getCount();
        diagnostics.confidence = follower.getConfidence();
        diagnostics.tempoRatio = follower.getTempoRatio();
        diagnostics.corrections = noteMachine.getPredictionCorrections();
        diagnostics.paletteDelayed = noteMachine.getPaletteDelayedDecisions();
        diagnostics.matches = follower.getMatches();
        diagnostics.mismatches = follower.getMismatches();
        diagnostics.resyncs = follower.getResyncs();
        diagnostics.extraAttacks = follower.getExtraAttacks();
        diagnostics.barActive = barActive();
        diagnostics.barOutputting = barActive() && barPlayer.isOutputting();
        diagnostics.barTrusted = barPlayer.isTrusted();
        diagnostics.barState = static_cast<int> (barPlayer.getState());
        diagnostics.barPlayed = barPlayer.getBar();
        diagnostics.barWritten = barPlayer.getWrittenBar();
        diagnostics.barCount = timelineBarCount;
        diagnostics.barTempoRatio = barPlayer.getTempoRatio();
        diagnostics.nextAnchorSeconds = barPlayer.getNextAnchorSeconds();
        diagnostics.barsStarted = barPlayer.getBarsStarted();
        diagnostics.barStops = barPlayer.getStops();
        diagnostics.rootMatches = barPlayer.getRootMatches();
        diagnostics.rootMismatches = barPlayer.getRootMismatches();
        diagnostics.barFallbacks = barPlayer.getFallbacks();
    }

    void BassToMidiProcessor::handleFrame (const float* frame, std::int64_t frameEnd, std::int64_t blockEnd, Output& out) noexcept
    {
        // frameEnd counts from prepare(), like samplePosition and the attack detector.
        const auto estimate = tracker.estimate (frame);
        const auto& pitch = estimate.pitch;
        diagnostics.pitch = pitch;
        diagnostics.rungWindowSamples = estimate.windowSamples;
        diagnostics.paletteRelieved = estimate.paletteRelieved;

        const int window = framer.getWindowSamples();
        const int hop = framer.getHopSamples();
        float hopPeak = 0.0f;
        for (int i = window - hop; i < window; ++i)
            hopPeak = std::max (hopPeak, std::abs (frame[i]));

        const int correctionsBefore = noteMachine.getPredictionCorrections();
        const int soundingBefore = noteMachine.getSoundingNote();
        bool correction = false;
        const bool barOutputs = barActive() && barPlayer.isOutputting();
        if (barOutputs && ! noteMachineSilent)
        {
            if (noteMachine.getSoundingNote() >= 0) // trust regained mid-block: end the sent Free note
                emit (noteMachine.allNotesOff(), blockEnd, false, false, out);
            noteMachineSilent = true;
        }
        if (! barOutputs && noteMachineSilent)
        {
            noteMachine.allNotesOff(); // its silent notes were never sent: start Free output clean
            noteMachineSilent = false;
        }
        {
            FrameFeatures features { pitch, static_cast<double> (hopPeak) };
            // While a prediction is pending, a window that still holds mostly the previous note would
            // "correct" the new note back to the old one: such frames carry no pitch for the state machine.
            if (noteMachine.isPredictionPending() && frameEnd - acceptedAttackSample < (3 * estimate.windowSamples) / 4)
                features.pitch.valid = false;
            const auto o = noteMachine.processFrame (features);
            correction = noteMachine.getPredictionCorrections() != correctionsBefore;
            if (! noteMachineSilent)
                emit (o, blockEnd, false, correction, out);
        }

        if (settings.attacks.usePeriodicity || (barActive() && settings.barModePeriodicity))
        {
            // Period of what the player's string sounds: the Free-path note (it also runs silently
            // during bar playback).
            const int sounding = noteMachine.getSoundingNote();
            attackDetector.setPeriodSamples (sounding > 0 ? sampleRateHz / midi::frequencyHzFromNote (sounding) : 0.0);
        }

        if (! songActive() && ! barActive())
            return;

        // A note the pitch path started on its own (no attack being validated): tell the follower.
        if (songActive() && ! validating && ! correction && noteMachine.getSoundingNote() >= 0 && noteMachine.getSoundingNote() != soundingBefore)
            follower.onPlayedNote (noteMachine.getSoundingNote(), seconds (frameEnd - estimate.windowSamples), -1);

        if (! validating)
            return;

        if (frameEnd - validationAttackSample > validationWindowSamples)
        {
            validating = false; // no clear pitch in time: no verdict
            return;
        }
        // Only frames whose window holds mostly the new attack count.
        if (frameEnd - validationAttackSample < (3 * estimate.windowSamples) / 4)
            return;
        if (pitch.valid && pitch.clarity >= settings.unpitchedClarity)
            validationVoiced = true;
        if (songActive() && ! validationVoiced && frameEnd - validationAttackSample >= unpitchedDecisionSamples)
        {
            // No pitch at all after the attack: a dead-note click, not the expected note.
            if (validationPredicted)
                emit (noteMachine.cancelPrediction(), blockEnd, false, false, out);
            follower.undoLastAttack();
            ++diagnostics.unpitchedAttacks;
            validating = false;
            return;
        }
        if (! pitch.valid || pitch.clarity < settings.validationMinClarity || ! (pitch.frequencyHz > 0.0))
        {
            validationRun = 0;
            return;
        }
        const double exact = midi::noteFromFrequencyHz (pitch.frequencyHz);
        const int note = static_cast<int> (std::lround (exact));
        if (std::abs (exact - note) * 100.0 > settings.toleranceCents)
        {
            validationRun = 0;
            return;
        }
        if (note == validationCandidate)
            ++validationRun;
        else
        {
            validationCandidate = note;
            validationRun = 1;
        }
        if (validationRun >= validationFramesNeeded && validationIndex == barAnchorVerdict)
        {
            barPlayer.onAnchorPitch (note);
            diagnostics.lastValidatedNote = note;
            validating = false;
            return;
        }
        if (validationRun >= validationFramesNeeded)
        {
            const auto* expected = follower.noteAt (validationIndex);
            diagnostics.lastValidatedNote = note;
            diagnostics.lastValidationMatch = expected != nullptr ? (expected->midiNote == note ? 1 : 0) : -1;
            follower.onPlayedNote (note, seconds (validationAttackSample), validationIndex);
            validating = false;
        }
    }
}
