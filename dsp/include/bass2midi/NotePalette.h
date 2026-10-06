#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bass2midi
{
    // A set of MIDI notes (0..127) expected in the current song ("song palette"). Plain value type of
    // two 64-bit words so it can be copied into real-time code and handed across threads as two
    // atomics. An empty palette means Free mode (no guidance).
    struct NotePalette
    {
        std::array<std::uint64_t, 2> bits {};

        bool contains (int note) const noexcept
        {
            return note >= 0 && note < 128 && ((bits[static_cast<std::size_t> (note >> 6)] >> (note & 63)) & 1u) != 0;
        }

        void add (int note) noexcept
        {
            if (note >= 0 && note < 128)
                bits[static_cast<std::size_t> (note >> 6)] |= std::uint64_t { 1 } << (note & 63);
        }

        bool empty() const noexcept { return bits[0] == 0 && bits[1] == 0; }

        int size() const noexcept
        {
            int n = 0;
            for (const auto word : bits)
                for (auto w = word; w != 0; w &= w - 1)
                    ++n;
            return n;
        }

        // Palette shifted by `semitones` (notes leaving 0..127 are dropped).
        NotePalette transposed (int semitones) const noexcept
        {
            NotePalette out;
            for (int n = 0; n < 128; ++n)
                if (contains (n))
                    out.add (n + semitones);
            return out;
        }

        bool operator== (const NotePalette& other) const noexcept { return bits == other.bits; }
    };
}
