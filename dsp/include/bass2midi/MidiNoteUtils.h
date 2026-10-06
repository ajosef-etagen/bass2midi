#pragma once

#include <array>
#include <cmath>
#include <string>

// Frequency <-> MIDI note conversion (12-TET, A4 = 440 Hz). Header-only, no dependencies.
namespace bass2midi::midi
{
    inline constexpr double a4FrequencyHz = 440.0;
    inline constexpr int a4MidiNote = 69;

    // Fractional MIDI note number, e.g. 41.2034 Hz -> ~28.0 (E1).
    inline double noteFromFrequencyHz (double frequencyHz)
    {
        return static_cast<double> (a4MidiNote) + 12.0 * std::log2 (frequencyHz / a4FrequencyHz);
    }

    inline double frequencyHzFromNote (double midiNote)
    {
        return a4FrequencyHz * std::pow (2.0, (midiNote - static_cast<double> (a4MidiNote)) / 12.0);
    }

    // Signed distance in cents between two frequencies (positive when frequencyHz is higher).
    inline double centsBetween (double frequencyHz, double referenceHz)
    {
        return 1200.0 * std::log2 (frequencyHz / referenceHz);
    }

    // Scientific pitch name, e.g. 28 -> "E1", 67 -> "G4".
    inline std::string noteName (int midiNote)
    {
        static constexpr std::array<const char*, 12> names { "C", "C#", "D", "D#", "E", "F",
                                                             "F#", "G", "G#", "A", "A#", "B" };
        const int pitchClass = ((midiNote % 12) + 12) % 12;
        const int octave = (midiNote - pitchClass) / 12 - 1;
        return std::string (names[static_cast<size_t> (pitchClass)]) + std::to_string (octave);
    }

    // Four-string bass, EADG standard tuning (open strings, MIDI note numbers).
    inline constexpr int lowEOpenNote = 28; // E1, 41.20 Hz
    inline constexpr int aOpenNote = 33;    // A1, 55.00 Hz
    inline constexpr int dOpenNote = 38;    // D2, 73.42 Hz
    inline constexpr int gOpenNote = 43;    // G2, 98.00 Hz
    inline constexpr int v1HighestNote = 67; // G4, 392.00 Hz - top of the V1 operating range
}
