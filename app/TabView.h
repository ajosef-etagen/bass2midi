#pragma once

#include "bass2midi/ExpectedNote.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

// Live bass tablature (4-string EADG): the song's bass part, two rows of bars - the row with the
// current position and the next one - with the position as a vertical line, passed notes dimmed and
// the next expected note highlighted. Message thread only; fed by MainComponent's timer.
class TabView final : public juce::Component
{
public:
    static constexpr int barsPerRow = 4;

    struct Position
    {
        bool running = false, armed = false;
        double scoreSeconds = 0.0;
        int playedBar = -1, writtenBar = -1;
        double beat = 1.0, bpm = 0.0, confidence = 0.0;
        int nextNote = -1;
    };

    // The vectors must outlive the view's use of them (MainComponent owns both).
    void setSong (const std::vector<bass2midi::ExpectedNote>* notes, const std::vector<bass2midi::TimelineBar>* bars);
    void setPosition (const Position& p);

    void paint (juce::Graphics&) override;

private:
    void paintRow (juce::Graphics&, juce::Rectangle<float> area, int firstBar);

    const std::vector<bass2midi::ExpectedNote>* notes = nullptr;
    const std::vector<bass2midi::TimelineBar>* bars = nullptr;
    Position position;
};
