#include <doctest/doctest.h>

#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/NoteStateMachine.h"
#include "bass2midi/SongFollower.h"

#include <cmath>
#include <vector>

using namespace bass2midi;

namespace
{
    // A riff of eighth notes at 120 bpm (0.25 s apart): repeated notes, steps, octave jumps.
    std::vector<ExpectedNote> riff (int bars = 4)
    {
        const int pattern[] = { 28, 28, 40, 28, 31, 33, 35, 33 };
        std::vector<ExpectedNote> notes;
        for (int b = 0; b < bars; ++b)
            for (int i = 0; i < 8; ++i)
            {
                ExpectedNote n;
                n.midiNote = pattern[i] + (b % 2 == 1 ? 5 : 0); // second bar a fourth up
                n.startSeconds = (b * 8 + i) * 0.25;
                n.durationSeconds = 0.25;
                n.bar = b;
                n.beat = 1.0 + i * 0.5;
                notes.push_back (n);
            }
        return notes;
    }

    // Plays the timeline: each attack at its (scaled) time, then the pitch verdict.
    struct Player
    {
        SongFollower f;
        std::vector<ExpectedNote> notes;
        int predicted = 0, correct = 0, wrong = 0;

        explicit Player (std::vector<ExpectedNote> n) : notes (std::move (n)) { f.setTimeline (notes.data(), (int) notes.size()); }

        // Returns the prediction; `played` is what the player really plays.
        SongFollower::Prediction play (double t, int played, bool verdict = true)
        {
            const auto p = f.onAttack (t);
            if (p.valid)
            {
                ++predicted;
                (p.midiNote == played ? correct : wrong)++;
            }
            if (verdict)
                f.onPlayedNote (played, t, p.index);
            return p;
        }
    };
}

TEST_CASE ("Song follower: settings validation")
{
    SongFollower f;
    SongFollower::Settings s;
    CHECK (s.isValid());
    s.resyncMatches = 0;
    CHECK_FALSE (f.setSettings (s));
    s = {};
    s.minTempoRatio = 2.0; // > max
    CHECK_FALSE (s.isValid());
    // No timeline: no predictions, nothing breaks.
    CHECK_FALSE (f.onAttack (1.0).valid);
    f.onPlayedNote (40, 1.0, -1);
}

TEST_CASE ("Song follower: as written, with timing jitter, every attack is predicted correctly")
{
    Player p (riff());
    const double jitter[] = { 0.0, 0.018, -0.02, 0.01, -0.012, 0.02, -0.008, 0.015 };
    for (std::size_t i = 0; i < p.notes.size(); ++i)
        p.play (p.notes[i].startSeconds + jitter[i % 8], p.notes[i].midiNote);
    CHECK (p.predicted == (int) p.notes.size());
    CHECK (p.wrong == 0);
    CHECK (p.f.getResyncs() == 0);
    CHECK (p.f.getConfidence() > 0.95);
}

TEST_CASE ("Song follower: a missed attack is skipped by timing")
{
    Player p (riff());
    for (std::size_t i = 0; i < p.notes.size(); ++i)
    {
        if (i == 5 || i == 17)
            continue; // the attack detector missed these plucks; the pitch path gives no verdict either
        p.play (p.notes[i].startSeconds, p.notes[i].midiNote);
    }
    CHECK (p.wrong == 0);
    CHECK (p.predicted == (int) p.notes.size() - 2);
}

TEST_CASE ("Song follower: an extra early attack (ghost note) predicts nothing and keeps the place")
{
    Player p (riff());
    for (std::size_t i = 0; i < p.notes.size(); ++i)
    {
        p.play (p.notes[i].startSeconds, p.notes[i].midiNote);
        if (i == 3 || i == 20)
        {
            const auto ghost = p.f.onAttack (p.notes[i].startSeconds + 0.06); // 60 ms after: a dead-note click
            CHECK_FALSE (ghost.valid);
        }
    }
    CHECK (p.wrong == 0);
    CHECK (p.f.getExtraAttacks() == 2);
}

TEST_CASE ("Song follower: a faster live tempo is learned")
{
    Player p (riff (8));
    const double ratio = 1.2; // 20 % faster than the score
    for (std::size_t i = 0; i < p.notes.size(); ++i)
        p.play (p.notes[i].startSeconds / ratio, p.notes[i].midiNote);
    CHECK (p.wrong == 0);
    CHECK (p.f.getTempoRatio() == doctest::Approx (ratio).epsilon (0.05));
}

TEST_CASE ("Song follower: variations lower the confidence and fall back; matching notes restore it")
{
    Player p (riff (8));
    int predictionsDuringFill = 0;
    for (std::size_t i = 0; i < p.notes.size(); ++i)
    {
        const bool fill = i >= 16 && i < 24; // the player improvises a bar (chromatic, not the score)
        const int played = fill ? 45 + (int) (i % 3) : p.notes[i].midiNote;
        const auto pred = p.play (p.notes[i].startSeconds, played);
        if (fill && pred.valid)
            ++predictionsDuringFill;
    }
    CHECK (predictionsDuringFill <= 3);                 // Free fallback after a few contradicted notes
    CHECK (p.f.getConfidence() > 0.9);                  // recovered on the written part
    CHECK (p.wrong <= 3);
    CHECK (p.correct >= (int) p.notes.size() - 8 - 4);  // nearly all written notes predicted again
}

TEST_CASE ("Song follower: a wrong start position is re-aligned from the played notes")
{
    Player p (riff (4));
    p.f.setPosition (3); // the player actually starts at note 0
    for (std::size_t i = 0; i < p.notes.size(); ++i)
        p.play (p.notes[i].startSeconds, p.notes[i].midiNote);
    CHECK (p.f.getResyncs() >= 1);
    CHECK (p.wrong <= 3);
    // The second half is predicted correctly.
    CHECK (p.correct >= 14);
}

TEST_CASE ("Note state machine: a predicted note starts at once, is not duplicated, and can be corrected")
{
    NoteStateMachine m;
    NoteStateMachine::Settings s;
    REQUIRE (m.setSettings (s));
    const auto frame = [] (int note, double peakDbfs, double clarity = 0.97)
    {
        FrameFeatures f;
        f.hopPeakLinear = std::pow (10.0, peakDbfs / 20.0);
        f.pitch.windowRmsLinear = f.hopPeakLinear * 0.5;
        if (note > 0)
        {
            f.pitch.valid = true;
            f.pitch.frequencyHz = midi::frequencyHzFromNote (note);
            f.pitch.clarity = clarity;
        }
        return f;
    };

    for (int i = 0; i < 20; ++i)
        m.processFrame (frame (0, -90.0));

    // Attack: predicted A1 right away with the attack's velocity.
    auto o = m.triggerPredicted (33, 90);
    REQUIRE (o.count == 1);
    CHECK (o.events[0].type == NoteEvent::Type::noteOn);
    CHECK (o.events[0].note == 33);
    CHECK (o.events[0].velocity == 90);

    // The same pluck seen by the pitch path (loud rise = its own onset, pitch agrees): no events.
    int events = 0;
    for (int i = 0; i < 40; ++i)
        events += m.processFrame (frame (33, -12.0)).count;
    CHECK (events == 0);

    // Next attack predicted D2, but the player plays C2: corrected after predictionCorrectionMs (8 frames).
    o = m.triggerPredicted (38, 80);
    REQUIRE (o.count == 2); // Note Off A1, Note On D2
    events = 0;
    for (int i = 0; i < 7; ++i)
        events += m.processFrame (frame (36, -12.0)).count;
    CHECK (events == 0);
    o = m.processFrame (frame (36, -12.0));
    REQUIRE (o.count == 2);
    CHECK (o.events[0].note == 38);
    CHECK (o.events[1].note == 36);
    CHECK (m.getPredictionCorrections() == 1);

    // Out-of-range predictions are ignored; transposition applies.
    CHECK (m.triggerPredicted (10, 90).count == 0);
    s.transposeSemitones = 12;
    REQUIRE (m.setSettings (s));
    o = m.triggerPredicted (40, 70);
    REQUIRE (o.count == 2);
    CHECK (o.events[0].note == 36);      // the sounding note keeps its output note
    CHECK (o.events[1].note == 52);
    o = m.allNotesOff();
    REQUIRE (o.count == 1);
    CHECK (o.events[0].note == 52);
}

#include "SyntheticBass.h"

#include "bass2midi/BassToMidiProcessor.h"

TEST_CASE ("Bass-to-MIDI processor: Song Mode triggers expected notes within a few ms; Free mode unchanged")
{
    const double rate = 48000.0;
    // Six plucks, 0.4 s apart, each muted 50 ms before the next (clean attacks).
    const std::vector<int> played { 33, 38, 40, 28, 43, 36 };
    std::vector<ExpectedNote> timeline;
    std::vector<float> signal (static_cast<std::size_t> (3.2 * rate), 0.0f);
    for (std::size_t i = 0; i < played.size(); ++i)
    {
        ExpectedNote n;
        n.midiNote = played[i];
        n.startSeconds = 0.4 * static_cast<double> (i);
        n.durationSeconds = 0.35;
        timeline.push_back (n);

        eval::PluckSpec spec;
        spec.fundamentalHz = midi::frequencyHzFromNote (played[i]);
        spec.sampleRateHz = rate;
        spec.onsetSeconds = 0.0;
        spec.durationSeconds = 0.35;
        spec.peakLinear = 0.3;
        spec.noiseFloorDbfs = -200.0;
        spec.seed = static_cast<std::uint32_t> (i + 5);
        const auto tone = eval::renderPluck (spec);
        const auto onset = static_cast<std::size_t> ((0.3 + 0.4 * static_cast<double> (i)) * rate);
        for (std::size_t k = 0; k < tone.size(); ++k)
            signal[onset + k] += tone[k] * static_cast<float> (std::min (1.0, static_cast<double> (tone.size() - k) / (0.005 * rate)));
    }

    const auto runMode = [&] (BassToMidiProcessor::Mode mode)
    {
        BassToMidiProcessor p;
        REQUIRE (p.prepare (rate, {}));
        p.setTimeline (timeline.data(), static_cast<int> (timeline.size()));
        p.setMode (mode);
        std::vector<BassToMidiProcessor::TimedEvent> events;
        int sounding = -1;
        for (std::size_t pos = 0; pos < signal.size(); pos += 64)
        {
            BassToMidiProcessor::Output out;
            p.process (signal.data() + pos, static_cast<int> (std::min<std::size_t> (64, signal.size() - pos)), out);
            for (int i = 0; i < out.count; ++i)
            {
                const auto& e = out.events[static_cast<std::size_t> (i)];
                if (e.event.type == NoteEvent::Type::noteOn)
                {
                    REQUIRE (sounding < 0);
                    sounding = e.event.note;
                    events.push_back (e);
                }
                else
                {
                    REQUIRE (sounding == e.event.note);
                    sounding = -1;
                }
            }
        }
        return std::make_pair (events, p.getDiagnostics());
    };

    const auto [songEvents, songDiag] = runMode (BassToMidiProcessor::Mode::song);
    REQUIRE (songEvents.size() == played.size());
    for (std::size_t i = 0; i < played.size(); ++i)
    {
        CAPTURE (i);
        CHECK (songEvents[i].event.note == played[i]);
        CHECK (songEvents[i].predicted);
        const double latencyMs = 1000.0 * (static_cast<double> (songEvents[i].sample) / rate - (0.3 + 0.4 * static_cast<double> (i)));
        CHECK (latencyMs >= 0.0);
        CHECK (latencyMs < 6.0);
    }
    CHECK (songDiag.matches == static_cast<int> (played.size()));
    CHECK (songDiag.corrections == 0);

    const auto [freeEvents, freeDiag] = runMode (BassToMidiProcessor::Mode::free);
    REQUIRE (freeEvents.size() == played.size());
    for (std::size_t i = 0; i < played.size(); ++i)
    {
        CHECK (freeEvents[i].event.note == played[i]);
        CHECK_FALSE (freeEvents[i].predicted);
        CHECK (freeEvents[i].sample > songEvents[i].sample + static_cast<std::int64_t> (0.008 * rate)); // > 8 ms later
    }
    CHECK (freeDiag.predictions == 0);
}
