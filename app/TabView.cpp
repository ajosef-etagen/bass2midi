#include "TabView.h"

#include "bass2midi/MidiNoteUtils.h"

namespace
{
    constexpr const char* stringNames[4] = { "E", "A", "D", "G" };
    constexpr float labelWidth = 22.0f;
    constexpr float barNumberHeight = 14.0f;
    constexpr float stringSpacing = 15.0f;
}

void TabView::setSong (const std::vector<bass2midi::ExpectedNote>* newNotes, const std::vector<bass2midi::TimelineBar>* newBars)
{
    notes = newNotes;
    bars = newBars;
    repaint();
}

void TabView::setPosition (const Position& p)
{
    position = p;
    repaint();
}

void TabView::paint (juce::Graphics& g)
{
    const auto background = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    g.fillAll (background.darker (0.25f));
    auto area = getLocalBounds().toFloat().reduced (6.0f);

    if (notes == nullptr || bars == nullptr || bars->empty())
    {
        g.setColour (juce::Colours::grey);
        g.drawText ("No song selected: choose a song to see its bass tablature.", area, juce::Justification::centred);
        return;
    }

    // Header: where we are and what comes next.
    const auto header = area.removeFromTop (20.0f);
    juce::String text;
    if (position.playedBar >= 0)
    {
        text << "Bar " << position.writtenBar + 1 << "  beat " << juce::String (position.beat, 1) << "   "
             << juce::roundToInt (position.bpm) << " BPM";
        if (position.running)
            text << "   confidence " << juce::roundToInt (100.0 * position.confidence) << " %";
        else
            text << (position.armed ? "   (armed: starts with your first note)" : "   (stopped)");
        if (position.nextNote >= 0)
        {
            text << "   next:";
            for (int i = position.nextNote; i < juce::jmin ((int) notes->size(), position.nextNote + 4); ++i)
            {
                const auto& n = (*notes)[(size_t) i];
                text << "  " << bass2midi::midi::noteName (n.midiNote);
                if (n.string >= 0)
                    text << " (" << stringNames[n.string] << n.fret << ")";
            }
        }
    }
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (text, header, juce::Justification::centredLeft);

    // Two rows: the one with the current bar, then the next.
    const int barCount = (int) bars->size();
    const int current = juce::jlimit (0, barCount - 1, juce::jmax (0, position.playedBar));
    const int firstBar = (current / barsPerRow) * barsPerRow;
    const float rowHeight = area.getHeight() / 2.0f;
    paintRow (g, area.removeFromTop (rowHeight), firstBar);
    if (firstBar + barsPerRow < barCount)
        paintRow (g, area, firstBar + barsPerRow);
}

void TabView::paintRow (juce::Graphics& g, juce::Rectangle<float> area, int firstBar)
{
    const int barCount = (int) bars->size();
    const float top = area.getY() + barNumberHeight + 4.0f;
    const auto lineY = [&] (int string) { return top + (float) (3 - string) * stringSpacing; }; // G on top, E at the bottom

    g.setFont (juce::FontOptions (12.0f));
    g.setColour (juce::Colours::lightgrey);
    for (int s = 0; s < 4; ++s)
        g.drawText (stringNames[s], juce::Rectangle<float> (area.getX(), lineY (s) - 7.0f, labelWidth - 4.0f, 14.0f), juce::Justification::centredRight);

    const float barWidth = (area.getWidth() - labelWidth) / (float) barsPerRow;
    for (int k = 0; k < barsPerRow && firstBar + k < barCount; ++k)
    {
        const int b = firstBar + k;
        const auto& bar = (*bars)[(size_t) b];
        const float x0 = area.getX() + labelWidth + (float) k * barWidth;
        const float x1 = x0 + barWidth;
        const auto xAt = [&] (double scoreSeconds)
        {
            const double fraction = (scoreSeconds - bar.startSeconds) / bar.lengthSeconds;
            return x0 + 8.0f + (float) juce::jlimit (0.0, 1.0, fraction) * (barWidth - 16.0f);
        };

        // Bar number, lines, bar lines; the current bar slightly highlighted.
        if (b == position.playedBar)
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> (x0, top - 6.0f, barWidth, 3.0f * stringSpacing + 12.0f));
        }
        g.setColour (juce::Colours::grey);
        g.drawText (juce::String (bar.writtenBar + 1), juce::Rectangle<float> (x0 + 2.0f, area.getY(), 40.0f, barNumberHeight), juce::Justification::centredLeft);
        for (int s = 0; s < 4; ++s)
            g.drawHorizontalLine ((int) lineY (s), x0, x1);
        g.drawVerticalLine ((int) x1, lineY (3), lineY (0));
        if (k == 0)
            g.drawVerticalLine ((int) x0, lineY (3), lineY (0));

        // Notes: fret numbers on their strings.
        for (int i = bar.firstNote; i < bar.firstNote + bar.noteCount; ++i)
        {
            const auto& n = (*notes)[(size_t) i];
            const float x = xAt (n.startSeconds);
            const juce::String label = n.string >= 0 ? juce::String (n.fret) : juce::String ("?");
            const int string = n.string >= 0 ? n.string : 0;
            const auto box = juce::Rectangle<float> (x - 8.0f, lineY (string) - 7.0f, 16.0f, 14.0f);
            const bool passed = position.playedBar >= 0 && n.startSeconds < position.scoreSeconds - 1.0e-6;
            const bool next = i == position.nextNote;
            g.setColour (next ? juce::Colours::orange : getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId).darker (0.25f));
            g.fillRect (box);
            g.setColour (next ? juce::Colours::black : (passed ? juce::Colours::grey : juce::Colours::white));
            g.setFont (juce::FontOptions (12.0f + 1.5f * (float) n.anchorWeight, next ? juce::Font::bold : juce::Font::plain));
            g.drawText (label, box, juce::Justification::centred);
        }

        // The position line.
        if (b == position.playedBar)
        {
            g.setColour (juce::Colours::red);
            const float x = xAt (position.scoreSeconds);
            g.drawLine (x, top - 8.0f, x, lineY (0) + 8.0f, 2.0f);
        }
    }
}
