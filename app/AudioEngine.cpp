#include "AudioEngine.h"
#include "MidiOutputSender.h"

#include <algorithm>
#include <cmath>

AudioEngine::AudioEngine (MidiOutputSender& sender) : midiSender (sender) {}

AudioEngine::~AudioEngine() = default;

void AudioEngine::setMidiSettings (bool enabled, int midiChannel, double gateOpenDbfs, int transposeSemitones) noexcept
{
    pendingTransposeSemitones.store (juce::jlimit (-48, 48, transposeSemitones));
    pendingOutputEnabled.store (enabled);
    pendingMidiChannel.store (juce::jlimit (1, 16, midiChannel));
    pendingGateOpenDbfs.store (juce::jlimit (-90.0, 0.0, gateOpenDbfs));
    settingsVersion.fetch_add (1);
}

void AudioEngine::setPalette (const bass2midi::NotePalette& palette) noexcept
{
    // Single writer (message thread): odd sequence while the words change.
    paletteSequence.fetch_add (1);
    pendingPaletteLow.store (palette.bits[0]);
    pendingPaletteHigh.store (palette.bits[1]);
    paletteSequence.fetch_add (1);
}

void AudioEngine::setSongTimeline (std::vector<bass2midi::ExpectedNote> notes, std::vector<bass2midi::TimelineBar> bars)
{
    auto timeline = std::make_unique<Timeline>();
    timeline->notes = std::move (notes);
    timeline->bars = std::move (bars);
    pendingTimeline.store (timeline.get());
    timelines.push_back (std::move (timeline));
    releaseRetiredTimelines();
}

void AudioEngine::releaseRetiredTimelines()
{
    // `timelines` is in publication order. The audio thread may still load anything published at or
    // after the timeline it last acknowledged (appliedTimeline), but never anything before it - so
    // only those older entries are freed. (Freeing by "neither pending nor applied" would race with
    // an audio thread that has just loaded a pointer that is being replaced.)
    const auto* applied = appliedTimeline.load();
    const auto it = std::find_if (timelines.begin(), timelines.end(),
                                  [applied] (const std::unique_ptr<Timeline>& t) { return t.get() == applied; });
    if (it != timelines.end())
        timelines.erase (timelines.begin(), it);
}

void AudioEngine::applyPendingPalette() noexcept
{
    const auto before = paletteSequence.load();
    if ((before == appliedPaletteSequence && ! forcePaletteApply) || (before & 1u) != 0)
        return; // unchanged, or a write is in progress: try again next block

    bass2midi::NotePalette palette;
    palette.bits[0] = pendingPaletteLow.load();
    palette.bits[1] = pendingPaletteHigh.load();
    if (paletteSequence.load() != before)
        return; // torn read

    appliedPaletteSequence = before;
    forcePaletteApply = false;
    processor.setPalette (palette);
    paletteSize.store (palette.size(), std::memory_order_relaxed);
}

void AudioEngine::applyPendingSong() noexcept
{
    const auto* pending = pendingTimeline.load();
    if (pending != currentTimeline || forceTimelineApply)
    {
        currentTimeline = pending;
        forceTimelineApply = false;
        if (pending != nullptr)
            processor.setTimeline (pending->notes.data(), (int) pending->notes.size(), pending->bars.data(), (int) pending->bars.size());
        else
            processor.setTimeline (nullptr, 0, nullptr, 0);
        appliedTimeline.store (pending);
    }

    const int mode = pendingMode.load (std::memory_order_relaxed);
    processor.setMode (mode == 1 ? bass2midi::BassToMidiProcessor::Mode::song
                     : mode == 2 ? bass2midi::BassToMidiProcessor::Mode::bar
                                 : bass2midi::BassToMidiProcessor::Mode::free);
    const auto barStart = pendingBarStart.exchange (-1);
    if (barStart >= 0)
        processor.setBarStart (barStart);

    const auto counter = scoreCommandCounter.load();
    if (counter != appliedScoreCommandCounter)
    {
        appliedScoreCommandCounter = counter;
        switch (pendingScoreCommand.load())
        {
            case 1: processor.armScoreFollower (pendingScoreBar.load(), pendingScoreBpm.load()); break;
            case 2: processor.startScoreClock(); break;
            case 3: processor.stopScoreFollower(); break;
            default: break;
        }
    }

    processor.setPeriodicityEnabled (pendingRepluck.load (std::memory_order_relaxed));

    const auto position = pendingSongPosition.exchange (-1);
    if (position >= 0)
        processor.setSongPosition (position);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Called before callbacks start (not concurrently with them), so allocation is allowed here.
    const auto rate = device->getCurrentSampleRate();
    analysisReady = processor.prepare (rate, {});

    forcePaletteApply = true;   // the processor was re-prepared: hand it palette, timeline and
    forceTimelineApply = true;  // settings again
    appliedSettingsVersion = -1;
    soundingNote.store (-1);

    sampleRateHz.store (rate);
    blockSize.store (device->getCurrentBufferSizeSamples());
    windowSamples.store (processor.getFrameSamples());
    hopSamples.store (processor.getHopSamples());
    lastValid.store (false);
    resetStatsRequested.store (true);
}

void AudioEngine::audioDeviceStopped()
{
    // Callbacks have stopped, so this thread is the only producer for the audio FIFO now:
    // end a sounding note so MainStage never keeps a stuck note.
    bass2midi::BassToMidiProcessor::Output out;
    processor.allNotesOff (out);
    if (outputEnabled)
        send (out);
    analysisReady = false;
    lastValid.store (false);
}

void AudioEngine::applyPendingSettings() noexcept
{
    const auto version = settingsVersion.load();
    if (version == appliedSettingsVersion)
        return;

    appliedSettingsVersion = version;

    bass2midi::BassToMidiProcessor::Output out;
    const bool enabled = pendingOutputEnabled.load();
    const int channel = pendingMidiChannel.load();
    if (enabled != outputEnabled || channel != appliedMidiChannel)
    {
        // Disabling ends the sounding note; enabling starts from silence (whatever the processor
        // tracked while disabled was never sent, so its Note Off is dropped too). A channel change
        // ends the note on its old channel instead of letting it hang.
        processor.allNotesOff (out);
        if (outputEnabled)
            send (out);
    }
    outputEnabled = enabled;

    if (processor.setNoteSettings (channel, pendingGateOpenDbfs.load(), pendingTransposeSemitones.load()))
        appliedMidiChannel = channel;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const auto startTicks = juce::Time::getHighResolutionTicks();

    // Bass2MIDI produces MIDI only; never pass audio through.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    applyPendingSettings();
    applyPendingPalette();
    applyPendingSong();

    const int channel = juce::jmin (requestedChannel.load (std::memory_order_relaxed), numInputChannels - 1);
    const float* input = channel >= 0 ? inputChannelData[channel] : nullptr;

    if (input != nullptr)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (input, numSamples);
        const auto blockPeak = juce::jmax (-range.getStart(), range.getEnd());
        if (blockPeak > inputPeakLinear.load (std::memory_order_relaxed))
            inputPeakLinear.store (blockPeak, std::memory_order_relaxed);

        if (analysisReady)
        {
            bass2midi::BassToMidiProcessor::Output out;
            processor.process (input, numSamples, out);
            if (outputEnabled)
                send (out);
            publishDiagnostics();
        }
    }

    const auto elapsed = juce::Time::getHighResolutionTicks() - startTicks;

    if (resetStatsRequested.exchange (false))
    {
        worstCallbackTicks.store (0);
        totalCallbackTicks.store (0);
        callbackCount.store (0);
    }

    if (elapsed > worstCallbackTicks.load (std::memory_order_relaxed))
        worstCallbackTicks.store (elapsed, std::memory_order_relaxed);
    totalCallbackTicks.fetch_add (elapsed, std::memory_order_relaxed);
    callbackCount.fetch_add (1, std::memory_order_relaxed);
}

void AudioEngine::send (const bass2midi::BassToMidiProcessor::Output& output) noexcept
{
    for (int i = 0; i < output.count; ++i)
    {
        const auto& e = output.events[(size_t) i].event;
        if (e.type == bass2midi::NoteEvent::Type::noteOn)
        {
            midiSender.pushFromAudioThread (MidiOutputSender::noteOn (e.channel, e.note, e.velocity));
            noteOnCount.fetch_add (1, std::memory_order_relaxed);
        }
        else
        {
            midiSender.pushFromAudioThread (MidiOutputSender::noteOff (e.channel, e.note));
        }
    }
}

void AudioEngine::publishDiagnostics() noexcept
{
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto& d = processor.getDiagnostics();
    lastValid.store (d.pitch.valid, relaxed);
    lastFrequencyHz.store (d.pitch.frequencyHz, relaxed);
    lastClarity.store (d.pitch.clarity, relaxed);
    lastRmsLinear.store (d.pitch.windowRmsLinear, relaxed);
    lastRungWindowSamples.store (d.rungWindowSamples, relaxed);
    if (d.paletteRelieved)
        paletteRelievedFrames.fetch_add (1, relaxed); // approximate: counted per block, not per frame
    soundingNote.store (outputEnabled ? d.soundingOutputNote : -1, relaxed);
    playedNote.store (outputEnabled ? d.soundingPlayedNote : -1, relaxed);
    lastVelocity.store (d.lastVelocity, relaxed);
    paletteDelayed.store (d.paletteDelayed, relaxed);

    songActive.store (d.songActive, relaxed);
    songPosition.store (d.position, relaxed);
    songNoteCount.store (d.timelineCount, relaxed);
    songConfidence.store (d.confidence, relaxed);
    songTempoRatio.store (d.tempoRatio, relaxed);
    attackCount.store (d.attackCount, relaxed);
    lastAttackVelocity.store (d.lastAttackVelocity, relaxed);
    lastPredictedNote.store (d.lastPredictedNote, relaxed);
    lastValidatedNote.store (d.lastValidatedNote, relaxed);
    lastValidationMatch.store (d.lastValidationMatch, relaxed);
    lastTriggerLatencyMs.store (d.lastTriggerLatencySamples >= 0 ? 1000.0 * (double) d.lastTriggerLatencySamples / processor.getSampleRate() : -1.0, relaxed);
    predictions.store (d.predictions, relaxed);
    corrections.store (d.corrections, relaxed);
    resyncs.store (d.resyncs, relaxed);
    extraAttacks.store (d.extraAttacks, relaxed);
    ghostAttacks.store (d.ghostAttacks, relaxed);
    unpitchedAttacks.store (d.unpitchedAttacks, relaxed);
    barActive.store (d.barActive, relaxed);
    barOutputting.store (d.barOutputting, relaxed);
    barWritten.store (d.barWritten, relaxed);
    barsStarted.store (d.barsStarted, relaxed);
    barStops.store (d.barStops, relaxed);
    barFallbacks.store (d.barFallbacks, relaxed);
    rootMatches.store (d.rootMatches, relaxed);
    rootMismatches.store (d.rootMismatches, relaxed);
    barTempoRatio.store (d.barTempoRatio, relaxed);
    scoreRunning.store (d.score.running, relaxed);
    scoreArmed.store (d.scoreArmed, relaxed);
    scoreSeconds.store (d.score.scoreSeconds, relaxed);
    scoreBeat.store (d.score.beat, relaxed);
    scoreBpm.store (d.score.bpm, relaxed);
    scoreConfidence.store (d.score.confidence, relaxed);
    scorePlayedBar.store (d.score.playedBar, relaxed);
    scoreWrittenBar.store (d.score.writtenBar, relaxed);
    scoreNextNote.store (d.score.nextNote, relaxed);
}

AudioEngine::Snapshot AudioEngine::getSnapshot() const noexcept
{
    Snapshot s;
    s.valid = lastValid.load();
    s.frequencyHz = lastFrequencyHz.load();
    s.clarity = lastClarity.load();
    const auto rms = lastRmsLinear.load();
    s.windowRmsDbfs = rms > 1.0e-6 ? 20.0 * std::log10 (rms) : -120.0;
    s.sampleRateHz = sampleRateHz.load();
    s.blockSize = blockSize.load();
    s.windowSamples = windowSamples.load();
    s.lastRungWindowSamples = lastRungWindowSamples.load();
    s.hopSamples = hopSamples.load();
    s.soundingNote = soundingNote.load();
    s.playedNote = playedNote.load();
    s.lastVelocity = lastVelocity.load();
    s.noteOnCount = noteOnCount.load();
    s.paletteSize = paletteSize.load();
    s.paletteRelievedFrames = paletteRelievedFrames.load();
    s.paletteDelayed = paletteDelayed.load();

    s.songActive = songActive.load();
    s.songPosition = songPosition.load();
    s.songNoteCount = songNoteCount.load();
    s.songConfidence = songConfidence.load();
    s.songTempoRatio = songTempoRatio.load();
    s.attackCount = attackCount.load();
    s.lastAttackVelocity = lastAttackVelocity.load();
    s.lastPredictedNote = lastPredictedNote.load();
    s.lastValidatedNote = lastValidatedNote.load();
    s.lastValidationMatch = lastValidationMatch.load();
    s.lastTriggerLatencyMs = lastTriggerLatencyMs.load();
    s.predictions = predictions.load();
    s.corrections = corrections.load();
    s.resyncs = resyncs.load();
    s.extraAttacks = extraAttacks.load();
    s.ghostAttacks = ghostAttacks.load();
    s.unpitchedAttacks = unpitchedAttacks.load();
    s.barActive = barActive.load();
    s.barOutputting = barOutputting.load();
    s.barWritten = barWritten.load();
    s.barsStarted = barsStarted.load();
    s.barStops = barStops.load();
    s.barFallbacks = barFallbacks.load();
    s.rootMatches = rootMatches.load();
    s.rootMismatches = rootMismatches.load();
    s.barTempoRatio = barTempoRatio.load();
    s.scoreRunning = scoreRunning.load();
    s.scoreArmed = scoreArmed.load();
    s.scoreSeconds = scoreSeconds.load();
    s.scoreBeat = scoreBeat.load();
    s.scoreBpm = scoreBpm.load();
    s.scoreConfidence = scoreConfidence.load();
    s.scorePlayedBar = scorePlayedBar.load();
    s.scoreWrittenBar = scoreWrittenBar.load();
    s.scoreNextNote = scoreNextNote.load();

    const auto ticksPerMs = (double) juce::Time::getHighResolutionTicksPerSecond() / 1000.0;
    const auto count = callbackCount.load();
    s.worstCallbackMs = (double) worstCallbackTicks.load() / ticksPerMs;
    s.averageCallbackMs = count > 0 ? (double) totalCallbackTicks.load() / (double) count / ticksPerMs : 0.0;
    return s;
}
