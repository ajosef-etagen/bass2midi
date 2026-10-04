#include <doctest/doctest.h>

#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/NoteStateMachine.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace bass2midi;

namespace
{
    using Type = NoteEvent::Type;

    FrameFeatures frame (double note, double peakDbfs, double clarity = 0.95)
    {
        FrameFeatures f;
        f.hopPeakLinear = std::pow (10.0, peakDbfs / 20.0);
        if (note > 0.0)
        {
            f.pitch.valid = true;
            f.pitch.frequencyHz = midi::frequencyHzFromNote (note);
            f.pitch.clarity = clarity;
        }
        return f;
    }

    FrameFeatures silence() { return frame (0.0, -90.0); }

    // Feeds frames and records every event; checks pairing invariants on the way.
    struct Harness
    {
        NoteStateMachine machine;
        std::vector<NoteEvent> events;
        int sounding = -1, soundingChannel = 0;

        Harness()
        {
            NoteStateMachine::Settings s;
            REQUIRE (machine.setSettings (s));
        }

        int feed (const FrameFeatures& f, int times = 1)
        {
            int emitted = 0;
            for (int i = 0; i < times; ++i)
            {
                const auto out = machine.processFrame (f);
                for (int e = 0; e < out.count; ++e)
                    record (out.events[static_cast<size_t> (e)]);
                emitted += out.count;
            }
            return emitted;
        }

        void record (const NoteEvent& e)
        {
            if (e.type == Type::noteOn)
            {
                REQUIRE (sounding < 0); // never two Note Ons without a Note Off in between
                CHECK (e.velocity >= 1);
                CHECK (e.velocity <= 127);
                sounding = e.note;
                soundingChannel = e.channel;
            }
            else
            {
                REQUIRE (sounding >= 0);
                CHECK (e.note == sounding);
                CHECK (e.channel == soundingChannel);
                sounding = -1;
            }
            events.push_back (e);
        }

        int count (Type t) const
        {
            int n = 0;
            for (const auto& e : events)
                n += e.type == t ? 1 : 0;
            return n;
        }
    };
}

TEST_CASE ("Note state machine: settings validation")
{
    NoteStateMachine machine;
    NoteStateMachine::Settings s;
    CHECK (s.isValid());

    auto bad = s;
    bad.midiChannel = 17;
    CHECK_FALSE (machine.setSettings (bad));
    bad = s;
    bad.lowestNote = 70;
    CHECK_FALSE (machine.setSettings (bad));
    bad = s;
    bad.toleranceCents = 50.0;
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.frameIntervalSeconds = 0.0;
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.velocityCeilDbfs = bad.velocityFloorDbfs;
    CHECK_FALSE (bad.isValid());
    bad = s;
    bad.gateOpenDbfs = NAN;
    CHECK_FALSE (bad.isValid());

    // Rejected settings leave the previous ones in place.
    s.midiChannel = 5;
    REQUIRE (machine.setSettings (s));
    bad = s;
    bad.midiChannel = 0;
    CHECK_FALSE (machine.setSettings (bad));
    CHECK (machine.getSettings().midiChannel == 5);
}

TEST_CASE ("Note state machine: silence and noise produce nothing")
{
    Harness h;
    CHECK (h.feed (silence(), 400) == 0);
    CHECK (h.feed (frame (0.0, -20.0), 400) == 0); // loud but unpitched
    h.feed (silence(), 20); // let the 25 ms envelope hold forget the loud noise
    CHECK (h.feed (frame (45.0, -60.0), 400) == 0); // pitched but below the gate
    CHECK (h.feed (frame (45.0, -20.0, 0.5), 400) == 0); // unclear pitch
}

TEST_CASE ("Note state machine: Note On after attackFrames with attack-peak velocity")
{
    Harness h;
    h.feed (silence(), 20);
    CHECK (h.feed (frame (33.0, -12.0)) == 0);
    REQUIRE (h.feed (frame (33.0, -12.0)) == 1);

    const auto& on = h.events.back();
    CHECK (on.type == Type::noteOn);
    CHECK (on.note == 33);
    CHECK (on.channel == 1);
    CHECK (on.velocity == NoteStateMachine::velocityFromPeak (std::pow (10.0, -12.0 / 20.0), h.machine.getSettings()));

    // Sustained pluck: no duplicate Note On.
    CHECK (h.feed (frame (33.0, -14.0), 400) == 0);
    CHECK (h.count (Type::noteOn) == 1);
}

TEST_CASE ("Note state machine: release needs sustained low level, not one bad frame")
{
    Harness h;
    h.feed (silence(), 20);
    h.feed (frame (40.0, -10.0), 10);
    REQUIRE (h.machine.getSoundingNote() == 40);

    // Isolated uncertain frames while loud do not end the note.
    for (int i = 0; i < 20; ++i)
    {
        h.feed (frame (0.0, -12.0));
        h.feed (frame (40.0, -12.0), 3);
    }
    CHECK (h.count (Type::noteOff) == 0);

    // Low level: the envelope (25 ms hold = 10 frames) falls at the 10th silent frame, then
    // releaseHoldMs (30 ms = 12 frames) must elapse: Note Off at the 21st silent frame.
    CHECK (h.feed (silence(), 20) == 0);
    CHECK (h.feed (silence()) == 1);
    CHECK (h.events.back().type == Type::noteOff);
    CHECK (h.machine.getState() == NoteStateMachine::State::idle);
}

TEST_CASE ("Note state machine: loud but unpitched for unvoicedReleaseMs ends the note")
{
    Harness h;
    h.feed (frame (40.0, -10.0), 5);
    REQUIRE (h.count (Type::noteOn) == 1);
    CHECK (h.feed (frame (0.0, -10.0), 79) == 0);
    CHECK (h.feed (frame (0.0, -10.0)) == 1);
    CHECK (h.count (Type::noteOff) == 1);
}

TEST_CASE ("Note state machine: semitone change vs. octave jump without onset")
{
    Harness h;
    h.feed (frame (40.0, -10.0), 10);

    // Semitone change: noteChangeConfirmMs = 10 ms = 4 frames.
    CHECK (h.feed (frame (41.0, -10.0), 3) == 0);
    CHECK (h.feed (frame (41.0, -10.0)) == 2);
    CHECK (h.events[1].type == Type::noteOff);
    CHECK (h.events[1].note == 40);
    CHECK (h.events[2].type == Type::noteOn);
    CHECK (h.events[2].note == 41);

    // A 10-frame octave blip (typical octave error) is suppressed: octave needs 40 ms = 16 frames.
    h.feed (frame (53.0, -10.0), 10);
    h.feed (frame (41.0, -10.0), 10);
    CHECK (h.machine.getSoundingNote() == 41);
    CHECK (h.count (Type::noteOn) == 2);

    // A sustained octave move is accepted after 16 frames.
    CHECK (h.feed (frame (53.0, -10.0), 15) == 0);
    CHECK (h.feed (frame (53.0, -10.0)) == 2);
    CHECK (h.machine.getSoundingNote() == 53);
}

TEST_CASE ("Note state machine: an onset lets an octave change through quickly")
{
    Harness h;
    h.feed (frame (40.0, -30.0), 20); // decayed note
    REQUIRE (h.machine.getSoundingNote() == 40);

    h.feed (frame (40.0, -10.0)); // new attack: +20 dB rise
    REQUIRE (h.machine.isOnsetPending());
    CHECK (h.feed (frame (52.0, -10.0)) == 0);
    CHECK (h.feed (frame (52.0, -10.0)) == 2);
    CHECK (h.machine.getSoundingNote() == 52);
}

TEST_CASE ("Note state machine: same-note re-attack after onset, once")
{
    Harness h;
    h.feed (silence(), 10);
    h.feed (frame (45.0, -10.0), 40);
    h.feed (frame (45.0, -35.0), 40); // decayed, still above the release threshold (-51 dBFS)
    REQUIRE (h.count (Type::noteOn) == 1);

    // Re-pluck: level rises, pitch stays. Re-attack waits for onsetSettleMs (40 ms = 16 frames).
    CHECK (h.feed (frame (45.0, -8.0), 15) == 0);
    CHECK (h.feed (frame (45.0, -8.0)) == 2);
    CHECK (h.count (Type::noteOn) == 2);
    CHECK (h.events.back().velocity > h.events.front().velocity);

    // No further retriggers while it rings.
    CHECK (h.feed (frame (45.0, -9.0), 200) == 0);
}

TEST_CASE ("Note state machine: frames between semitones and out-of-range notes do not count")
{
    Harness h;
    CHECK (h.feed (frame (40.45, -10.0), 50) == 0); // 45 cents off: beyond the 40-cent tolerance
    CHECK (h.feed (frame (20.0, -10.0), 50) == 0);  // below lowestNote
    CHECK (h.feed (frame (80.0, -10.0), 50) == 0);  // above highestNote
    CHECK (h.feed (frame (40.3, -10.0), 2) == 1);   // 30 cents off is fine
}

TEST_CASE ("Note state machine: channel change and allNotesOff never orphan a note")
{
    Harness h;
    h.feed (frame (40.0, -10.0), 5);
    REQUIRE (h.machine.getSoundingNote() == 40);

    auto s = h.machine.getSettings();
    s.midiChannel = 7;
    REQUIRE (h.machine.setSettings (s));

    const auto out = h.machine.allNotesOff();
    REQUIRE (out.count == 1);
    CHECK (out.events[0].channel == 1);
    CHECK (out.events[0].note == 40);
    CHECK (h.machine.allNotesOff().count == 0);

    // Next note uses the new channel.
    h.sounding = -1;
    h.feed (silence(), 10);
    h.feed (frame (42.0, -10.0), 3);
    CHECK (h.events.back().channel == 7);
}

TEST_CASE ("Note state machine: velocity mapping is monotonic and bounded")
{
    NoteStateMachine::Settings s;
    int previous = 0;
    for (double db = -70.0; db <= 6.0; db += 1.0)
    {
        const int v = NoteStateMachine::velocityFromPeak (std::pow (10.0, db / 20.0), s);
        CHECK (v >= previous);
        CHECK (v >= s.minVelocity);
        CHECK (v <= 127);
        previous = v;
    }
    CHECK (NoteStateMachine::velocityFromPeak (0.0, s) == s.minVelocity);
    CHECK (NoteStateMachine::velocityFromPeak (1.0, s) == 127);
}

TEST_CASE ("Note state machine: pairing invariants hold on a pseudo-random frame stream")
{
    Harness h;
    std::uint32_t state = 12345;
    const auto next = [&]
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    for (int i = 0; i < 20000; ++i)
    {
        const auto r = next();
        const double note = (r % 5 == 0) ? 0.0 : 28.0 + static_cast<double> ((r >> 3) % 40) + static_cast<double> ((r >> 9) % 100) / 100.0 - 0.5;
        const double level = -90.0 + static_cast<double> ((r >> 16) % 90);
        const double clarity = static_cast<double> ((r >> 24) % 100) / 100.0;
        h.feed (frame (note, level, clarity), 1 + static_cast<int> ((r >> 28) % 6));
    }

    const auto out = h.machine.allNotesOff();
    for (int e = 0; e < out.count; ++e)
        h.record (out.events[static_cast<size_t> (e)]);

    CHECK (h.sounding < 0);
    CHECK (h.count (Type::noteOn) == h.count (Type::noteOff));
    CHECK (h.count (Type::noteOn) > 0);
}

TEST_CASE ("Note state machine: transposition applies to output and never orphans a note")
{
    Harness h;
    auto s = h.machine.getSettings();
    s.transposeSemitones = 12;
    REQUIRE (h.machine.setSettings (s));

    h.feed (frame (28.0, -10.0), 3);
    REQUIRE (h.count (Type::noteOn) == 1);
    CHECK (h.events.back().note == 40);           // played E1 (28) sent as E2 (40)
    CHECK (h.machine.getSoundingNote() == 28);
    CHECK (h.machine.getSoundingOutputNote() == 40);

    // Changing the transposition mid-note: the Note Off still uses the note that was sent.
    s.transposeSemitones = -12;
    REQUIRE (h.machine.setSettings (s));
    const auto out = h.machine.allNotesOff();
    REQUIRE (out.count == 1);
    CHECK (out.events[0].note == 40);

    s.transposeSemitones = 49;
    CHECK_FALSE (s.isValid());
    s.transposeSemitones = -48;
    CHECK (s.isValid());

    // Notes that would leave 0..127 are not played at all.
    Harness low;
    auto ls = low.machine.getSettings();
    ls.transposeSemitones = -30;
    REQUIRE (low.machine.setSettings (ls));
    CHECK (low.feed (frame (28.0, -10.0), 50) == 0);
    CHECK (low.feed (frame (40.0, -10.0), 2) == 1);
    CHECK (low.events.back().note == 10);
}
