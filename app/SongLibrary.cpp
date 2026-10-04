#include "SongLibrary.h"

#include "bass2midi/MidiNoteUtils.h"

juce::String SongLibrary::Entry::displayName() const
{
    juce::String name;
    if (! song.title.empty())
        name << juce::String::fromUTF8 (song.title.c_str());
    if (! song.artist.empty())
        name << (name.isEmpty() ? "" : " - ") << juce::String::fromUTF8 (song.artist.c_str());
    if (name.isEmpty())
        name = file.getFileNameWithoutExtension();
    if (error.isNotEmpty())
        name << "  (cannot load)";
    return name;
}

bass2midi::NotePalette SongLibrary::Entry::palette() const
{
    return usable() ? bass2midi::song::paletteFromTrack (song.tracks[(size_t) track]) : bass2midi::NotePalette {};
}

int SongLibrary::add (const juce::File& file)
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].file == file)
            return (int) i;

    Entry entry;
    entry.file = file;
    std::string error;
    if (bass2midi::song::loadSongFile (file.getFullPathName().toStdString(), entry.song, error))
    {
        entry.track = bass2midi::song::findBassTrack (entry.song);
        if (entry.track < 0)
            entry.error = "no pitched track with notes";
    }
    else
    {
        entry.error = juce::String::fromUTF8 (error.c_str());
    }
    entries.push_back (std::move (entry));
    return (int) entries.size() - 1;
}

void SongLibrary::remove (int index)
{
    if (index >= 0 && index < size())
        entries.erase (entries.begin() + index);
}

void SongLibrary::setTrack (int index, int track)
{
    if (index < 0 || index >= size())
        return;
    auto& entry = entries[(size_t) index];
    if (track >= 0 && track < (int) entry.song.tracks.size() && ! entry.song.tracks[(size_t) track].percussion)
        entry.track = track;
}

juce::String SongLibrary::toState() const
{
    juce::StringArray lines;
    for (const auto& e : entries)
        lines.add (juce::String (e.track) + "\t" + e.file.getFullPathName());
    return lines.joinIntoString ("\n");
}

void SongLibrary::restoreState (const juce::String& state)
{
    entries.clear();
    for (const auto& line : juce::StringArray::fromLines (state))
    {
        const auto tab = line.indexOfChar ('\t');
        if (tab <= 0)
            continue;
        const auto index = add (juce::File (line.substring (tab + 1)));
        setTrack (index, line.substring (0, tab).getIntValue());
    }
}

juce::String SongLibrary::describe (const bass2midi::NotePalette& palette)
{
    if (palette.empty())
        return "-";
    juce::StringArray names;
    for (int n = 0; n < 128; ++n)
        if (palette.contains (n))
            names.add (bass2midi::midi::noteName (n));
    return names.joinIntoString (" ") + "  (" + juce::String (palette.size()) + " notes)";
}
