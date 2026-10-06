#include "Readers.h"

#include "MiniXml.h"
#include "ZipReader.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

// Guitar Pro 7/8 (.gp): a ZIP archive whose Content/score.gpif holds the score as XML. The score is
// normalised: master bars list one bar id per track; bars list voice ids; voices list beat ids;
// beats reference a rhythm and list note ids. Implemented from the observable file structure.
namespace bass2midi::song::detail
{
    namespace
    {
        std::vector<int> parseIds (const std::string& text)
        {
            std::vector<int> ids;
            std::istringstream in (text);
            int id = 0;
            while (in >> id)
                ids.push_back (id);
            return ids;
        }

        std::map<int, const XmlNode*> indexById (const XmlNode* parent, const char* childName)
        {
            std::map<int, const XmlNode*> out;
            if (parent == nullptr)
                return out;
            for (const auto* node : parent->childrenNamed (childName))
                out[std::atoi (node->attribute ("id").c_str())] = node;
            return out;
        }

        const XmlNode* findProperty (const XmlNode* properties, const std::string& name)
        {
            if (properties == nullptr)
                return nullptr;
            for (const auto* p : properties->childrenNamed ("Property"))
                if (p->attribute ("name") == name)
                    return p;
            return nullptr;
        }

        double noteValueQuarters (const std::string& value)
        {
            static const std::map<std::string, double> table {
                { "Long", 16.0 }, { "DoubleWhole", 8.0 }, { "Whole", 4.0 }, { "Half", 2.0 },
                { "Quarter", 1.0 }, { "Eighth", 0.5 }, { "16th", 0.25 }, { "32nd", 0.125 },
                { "64th", 0.0625 }, { "128th", 0.03125 }, { "256th", 0.015625 }
            };
            const auto it = table.find (value);
            return it != table.end() ? it->second : 1.0;
        }

        double rhythmQuarters (const XmlNode* rhythm)
        {
            if (rhythm == nullptr)
                return 0.0;
            double q = noteValueQuarters (rhythm->childText ("NoteValue"));
            if (const auto* dot = rhythm->child ("AugmentationDot"))
            {
                const int dots = std::max (0, std::atoi (dot->attribute ("count").c_str()));
                double add = q;
                for (int i = 0; i < dots; ++i)
                {
                    add *= 0.5;
                    q += add;
                }
            }
            if (const auto* tuplet = rhythm->child ("PrimaryTuplet"))
            {
                const int num = std::atoi (tuplet->attribute ("num").c_str());
                const int den = std::atoi (tuplet->attribute ("den").c_str());
                if (num > 0 && den > 0)
                    q = q * den / num;
            }
            return q;
        }

        void parseTimeSignature (const std::string& time, int& numerator, int& denominator)
        {
            numerator = denominator = 4;
            const auto slash = time.find ('/');
            if (slash == std::string::npos)
                return;
            const int num = std::atoi (time.substr (0, slash).c_str());
            const int den = std::atoi (time.substr (slash + 1).c_str());
            if (num > 0 && den > 0)
            {
                numerator = num;
                denominator = den;
            }
        }

        bool containsNoCase (std::string haystack, std::string needle)
        {
            for (auto& c : haystack) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
            for (auto& c : needle) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
            return haystack.find (needle) != std::string::npos;
        }
    }

    bool readGpif (const std::vector<unsigned char>& archive, Song& song, std::string& error)
    {
        std::vector<unsigned char> xmlBytes;
        if (! extractZipEntry (archive, "Content/score.gpif", xmlBytes, error))
            return false;

        const auto root = parseXml (std::string (xmlBytes.begin(), xmlBytes.end()), error);
        if (root == nullptr)
            return false;
        if (root->name != "GPIF")
        {
            error = "score.gpif has no <GPIF> root";
            return false;
        }

        song = {};
        song.format = "gp";
        if (const auto* score = root->child ("Score"))
        {
            song.title = score->childText ("Title");
            song.artist = score->childText ("Artist");
        }

        // Tracks, in document order (master bars list bar ids in the same order).
        if (const auto* tracks = root->child ("Tracks"))
            for (const auto* t : tracks->childrenNamed ("Track"))
            {
                Track track;
                track.name = t->childText ("Name");

                const XmlNode* tuningProperty = nullptr;
                if (const auto* staves = t->child ("Staves"))
                    if (const auto* staff = staves->child ("Staff"))
                        tuningProperty = findProperty (staff->child ("Properties"), "Tuning");
                if (tuningProperty == nullptr) // older GP7 layout keeps properties on the track
                    tuningProperty = findProperty (t->child ("Properties"), "Tuning");
                if (tuningProperty != nullptr)
                    track.tuning = parseIds (tuningProperty->childText ("Pitches"));
                std::sort (track.tuning.begin(), track.tuning.end());

                std::string instrumentType;
                if (const auto* set = t->child ("InstrumentSet"))
                    instrumentType = set->childText ("Type");
                track.percussion = containsNoCase (instrumentType, "drum") || containsNoCase (instrumentType, "percussion");
                track.bassLike = ! track.percussion
                                 && (containsNoCase (instrumentType, "bass")
                                     || (! track.tuning.empty() && track.tuning.front() < 36));
                song.tracks.push_back (std::move (track));
            }

        const auto bars = indexById (root->child ("Bars"), "Bar");
        const auto voices = indexById (root->child ("Voices"), "Voice");
        const auto beats = indexById (root->child ("Beats"), "Beat");
        const auto notes = indexById (root->child ("Notes"), "Note");
        const auto rhythms = indexById (root->child ("Rhythms"), "Rhythm");

        std::vector<double> barStartQuarters;
        double barStart = 0.0;
        int barIndex = 0;
        if (const auto* masterBars = root->child ("MasterBars"))
            for (const auto* masterBar : masterBars->childrenNamed ("MasterBar"))
            {
                barStartQuarters.push_back (barStart);
                MasterBar info;
                info.startQuarters = barStart;
                parseTimeSignature (masterBar->childText ("Time"), info.numerator, info.denominator);
                info.lengthQuarters = 4.0 * info.numerator / info.denominator;
                if (const auto* repeat = masterBar->child ("Repeat"))
                {
                    info.repeatStart = repeat->attribute ("start") == "true";
                    if (repeat->attribute ("end") == "true")
                        info.repeatPlays = std::max (2, std::atoi (repeat->attribute ("count").c_str()));
                }
                for (const int ending : parseIds (masterBar->childText ("AlternateEndings")))
                    if (ending >= 1 && ending <= 32)
                        info.alternateEndings |= 1u << (ending - 1);
                song.masterBars.push_back (info);
                const auto barIds = parseIds (masterBar->childText ("Bars"));
                for (std::size_t trackIndex = 0; trackIndex < barIds.size() && trackIndex < song.tracks.size(); ++trackIndex)
                {
                    const auto barIt = bars.find (barIds[trackIndex]);
                    if (barIt == bars.end())
                        continue;
                    auto& track = song.tracks[trackIndex];
                    for (const int voiceId : parseIds (barIt->second->childText ("Voices")))
                    {
                        const auto voiceIt = voices.find (voiceId);
                        if (voiceId < 0 || voiceIt == voices.end())
                            continue;
                        double position = barStart;
                        for (const int beatId : parseIds (voiceIt->second->childText ("Beats")))
                        {
                            const auto beatIt = beats.find (beatId);
                            if (beatIt == beats.end())
                                continue;
                            const auto* beat = beatIt->second;
                            double duration = 0.0;
                            if (const auto* rhythm = beat->child ("Rhythm"))
                            {
                                const auto rhythmIt = rhythms.find (std::atoi (rhythm->attribute ("ref").c_str()));
                                duration = rhythmQuarters (rhythmIt != rhythms.end() ? rhythmIt->second : nullptr);
                            }
                            // Grace notes are written before the beat and take no metric time.
                            const bool grace = beat->child ("GraceNotes") != nullptr;

                            for (const int noteId : parseIds (beat->childText ("Notes")))
                            {
                                const auto noteIt = notes.find (noteId);
                                if (noteIt == notes.end())
                                    continue;
                                const auto* note = noteIt->second;
                                const auto* properties = note->child ("Properties");

                                int pitch = -1, writtenString = -1, writtenFret = -1;
                                const auto* stringProperty = findProperty (properties, "String");
                                const auto* fretProperty = findProperty (properties, "Fret");
                                if (stringProperty != nullptr && fretProperty != nullptr)
                                {
                                    const int stringIndex = std::atoi (stringProperty->childText ("String").c_str());
                                    const int fret = std::atoi (fretProperty->childText ("Fret").c_str());
                                    if (stringIndex >= 0 && stringIndex < static_cast<int> (track.tuning.size()))
                                    {
                                        pitch = track.tuning[static_cast<std::size_t> (stringIndex)] + fret;
                                        writtenString = stringIndex; // GPIF numbers strings from the lowest
                                        writtenFret = fret;
                                    }
                                }
                                if (pitch < 0)
                                    if (const auto* midi = findProperty (properties, "Midi"))
                                        pitch = std::atoi (midi->childText ("Number").c_str());
                                if (pitch < 0 || pitch > 127)
                                    continue;

                                Note out;
                                out.midiNote = pitch;
                                out.startQuarters = position;
                                out.durationQuarters = grace ? 0.0 : duration;
                                out.bar = barIndex;
                                out.string = writtenString;
                                out.fret = writtenFret;
                                if (const auto* tie = note->child ("Tie"))
                                    out.tieContinuation = tie->attribute ("destination") == "true";
                                track.notes.push_back (out);
                            }
                            if (! grace)
                                position += duration;
                        }
                    }
                }
                barStart += info.lengthQuarters;
                ++barIndex;
            }
        song.barCount = barIndex;

        if (const auto* masterTrack = root->child ("MasterTrack"))
            if (const auto* automations = masterTrack->child ("Automations"))
                for (const auto* automation : automations->childrenNamed ("Automation"))
                {
                    if (automation->childText ("Type") != "Tempo")
                        continue;
                    const int bar = std::atoi (automation->childText ("Bar").c_str());
                    const double bpm = std::atof (automation->childText ("Value").c_str()); // "120 2": bpm, unit
                    if (bpm <= 0.0)
                        continue;
                    const double at = bar >= 0 && bar < static_cast<int> (barStartQuarters.size())
                                          ? barStartQuarters[static_cast<std::size_t> (bar)]
                                          : barStart;
                    song.tempos.push_back ({ at, bpm });
                }
        std::stable_sort (song.tempos.begin(), song.tempos.end(),
                          [] (const TempoChange& a, const TempoChange& b) { return a.atQuarters < b.atQuarters; });

        for (auto& track : song.tracks)
            std::stable_sort (track.notes.begin(), track.notes.end(),
                              [] (const Note& a, const Note& b) { return a.startQuarters < b.startQuarters; });
        return true;
    }
}
