// Runs a recording through the same chain as the app (MultiResolutionPitchTracker + NoteStateMachine
// with default settings, 2.5 ms hop) and writes the emitted MIDI notes and a per-frame trace.
//
// Usage: bass2midi_analyze <file.wav> [--out <dir>] [--channel N] [--gate dBFS]

#include "WavFile.h"

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/MidiNoteUtils.h"
#include "bass2midi/MultiResolutionPitchTracker.h"
#include "bass2midi/NoteStateMachine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace bass2midi;

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <file.wav> [--out <dir>] [--channel N] [--gate dBFS]\n";
        return 2;
    }

    const std::string input = argv[1];
    std::filesystem::path outDir = "analysis";
    int channel = 0;
    double gateDbfs = NoteStateMachine::Settings {}.gateOpenDbfs;

    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string arg = argv[i];
        if (arg == "--out")
            outDir = argv[i + 1];
        else if (arg == "--channel")
            channel = std::stoi (argv[i + 1]);
        else if (arg == "--gate")
            gateDbfs = std::stod (argv[i + 1]);
        else
        {
            std::cerr << "unknown option " << arg << "\n";
            return 2;
        }
    }

    eval::WavData wav;
    std::string error;
    if (! eval::readWav (input, wav, error))
    {
        std::cerr << error << "\n";
        return 1;
    }
    if (channel < 0 || channel >= wav.channels)
    {
        std::cerr << "channel " << channel << " not in file (" << wav.channels << " channels)\n";
        return 1;
    }

    const double rate = wav.sampleRateHz;
    const auto& signal = wav.channelData[static_cast<size_t> (channel)];
    const int hop = std::max (1, static_cast<int> (std::lround (rate * 0.0025)));

    MultiResolutionPitchTracker tracker;
    AnalysisFramer framer;
    NoteStateMachine machine;
    NoteStateMachine::Settings settings;
    settings.frameIntervalSeconds = hop / rate;
    settings.gateOpenDbfs = gateDbfs;

    if (! tracker.prepare (rate, {}) || ! framer.prepare (tracker.getFrameSamples(), hop))
    {
        std::cerr << "cannot prepare analysis at " << rate << " Hz\n";
        return 1;
    }
    settings.onsetSettleMs = 0.75 * 1000.0 * tracker.getFrameSamples() / rate;
    if (! machine.setSettings (settings))
    {
        std::cerr << "invalid state machine settings\n";
        return 1;
    }

    std::filesystem::create_directories (outDir);
    const auto stem = std::filesystem::path (input).stem().string();
    std::ofstream events (outDir / (stem + "_events.csv"));
    std::ofstream frames (outDir / (stem + "_frames.csv"));
    events << "time_ms,type,note,name,velocity\n";
    frames << "time_ms,hop_peak_dbfs,valid,frequency_hz,note,cents,clarity,rung,window_ms,sub_octave_switches,straddles_silence,sounding\n";

    struct Note { double onMs; double offMs; int note; int velocity; };
    std::vector<Note> notes;
    int openIndex = -1;

    const auto emit = [&] (const NoteStateMachine::Output& out, double timeMs)
    {
        for (int i = 0; i < out.count; ++i)
        {
            const auto& e = out.events[static_cast<size_t> (i)];
            const bool on = e.type == NoteEvent::Type::noteOn;
            events << timeMs << ',' << (on ? "on" : "off") << ',' << e.note << ',' << midi::noteName (e.note) << ',' << e.velocity << '\n';
            if (on)
            {
                notes.push_back ({ timeMs, -1.0, e.note, e.velocity });
                openIndex = static_cast<int> (notes.size()) - 1;
            }
            else if (openIndex >= 0)
            {
                notes[static_cast<size_t> (openIndex)].offMs = timeMs;
                openIndex = -1;
            }
        }
    };

    const int frameSamples = tracker.getFrameSamples();
    framer.push (signal.data(), static_cast<int> (signal.size()), [&] (const float* frame, std::int64_t end, int)
    {
        float hopPeak = 0.0f;
        for (int i = frameSamples - hop; i < frameSamples; ++i)
            hopPeak = std::max (hopPeak, std::abs (frame[i]));

        const auto r = tracker.estimate (frame);
        const double timeMs = 1000.0 * static_cast<double> (end) / rate;
        emit (machine.processFrame ({ r.pitch, static_cast<double> (hopPeak) }), timeMs);

        double exact = 0.0;
        int note = -1;
        if (r.pitch.valid)
        {
            exact = midi::noteFromFrequencyHz (r.pitch.frequencyHz);
            note = static_cast<int> (std::lround (exact));
        }
        char line[256];
        std::snprintf (line, sizeof (line), "%.2f,%.1f,%d,%.2f,%d,%.0f,%.3f,%d,%.1f,%d,%d,%d\n",
                       timeMs, NoteStateMachine::toDbfs (hopPeak), r.pitch.valid ? 1 : 0, r.pitch.frequencyHz, note,
                       note >= 0 ? (exact - note) * 100.0 : 0.0, r.pitch.clarity, r.rung, 1000.0 * r.windowSamples / rate,
                       r.pitch.subOctaveSwitches, r.pitch.straddlesSilence ? 1 : 0, machine.getSoundingNote());
        frames << line;
    });
    emit (machine.allNotesOff(), 1000.0 * static_cast<double> (signal.size()) / rate); // NOLINT

    // Human-readable note list.
    std::printf ("%s: %.1f s, %.0f Hz, %d bit, channel %d, gate %.0f dBFS -> %zu notes\n",
                 input.c_str(), static_cast<double> (signal.size()) / rate, rate, wav.bitsPerSample, channel, gateDbfs, notes.size());
    for (const auto& n : notes)
        std::printf ("  %8.1f ms  %-4s (MIDI %2d)  vel %3d  dur %6.0f ms\n", n.onMs, midi::noteName (n.note).c_str(), n.note,
                     n.velocity, n.offMs - n.onMs);
    return 0;
}
