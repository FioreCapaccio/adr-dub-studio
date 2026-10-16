#include "DubWorkflow.h"
#include "TrackPresets.h"
#include "Theme.h"

namespace fc::dub
{
const std::vector<SessionTemplate>& templates()
{
    static const std::vector<SessionTemplate> t = {
        { "dubbing", TRX ("Doppiaggio standard"), TRX ("Guida (dialogo originale), M&E, Effetti e una traccia Dialoghi: ognuna sul proprio bus, pronta per gli stem."),
          { { TRX ("Guida"), TrackGuide }, { TRX ("M&E"), TrackME }, { TRX ("Effetti"), TrackEffects }, { TRX ("Dialoghi"), TrackDialogue } } },
        { "adr", TRX ("Solo registrazione ADR"), TRX ("Guida e traccia ADR per registrare le battute; M&E come riferimento in cuffia."),
          { { TRX ("Guida"), TrackGuide }, { TRX ("ADR"), TrackDialogue }, { TRX ("M&E"), TrackME } } },
        { "mix", TRX ("Revisione e mix"), TRX ("Dialoghi, M&E ed Effetti per il mix finale, sui rispettivi bus."),
          { { TRX ("Dialoghi"), TrackDialogue }, { TRX ("M&E"), TrackME }, { TRX ("Effetti"), TrackEffects } } },
    };
    return t;
}

void applyTemplate (Project& p, const SessionTemplate& t)
{
    p.newSession (false);
    int i = 0;
    for (auto& tt : t.tracks)
    {
        const auto colour = tt.type == TrackGuide ? trackTypeColour (TrackGuide) : theme::trackPalette[i % theme::numPaletteColours];
        p.addTrackAs (juce::translate (tt.name), tt.type, colour, roleOutBus (tt.type));
        ++i;
    }
    if (! p.getSession().tracks.empty()) p.selectedTrackId = p.getSession().tracks.back().id;
}

juce::String expandNaming (const juce::String& scheme, const AdrCue& cue, const juce::String& projectName)
{
    juce::String s = scheme.isEmpty() ? juce::String ("{personaggio}_{scena}_{cue}_T{take}") : scheme;
    s = s.replace ("{personaggio}", cue.character.isEmpty() ? juce::String ("ADR") : cue.character)
         .replace ("{scena}", cue.scene)
         .replace ("{bobina}", cue.reel)
         .replace ("{cue}", juce::String (cue.number))
         .replace ("{data}", juce::Time::getCurrentTime().formatted ("%Y%m%d"))
         .replace ("{progetto}", projectName);
    s = juce::File::createLegalFileName (s);
    while (s.contains ("__")) s = s.replace ("__", "_");
    return s.trimCharactersAtStart ("_ ").trimCharactersAtEnd ("_ ");
}

int createCharacterTracks (Project& p, const std::vector<AdrCue>& cues)
{
    std::vector<juce::String> names;
    for (auto& c : cues)
    {
        const auto n = c.character.trim();
        if (n.isEmpty()) continue;
        bool known = false;
        for (auto& k : names) known = known || k.equalsIgnoreCase (n);
        if (! known) names.push_back (n);
    }
    int created = 0;
    for (auto& n : names)
    {
        bool exists = false;
        for (auto& t : p.getSession().tracks) exists = exists || t.name.trim().equalsIgnoreCase (n);
        if (exists) continue;
        const int index = (int) p.getSession().tracks.size();
        const int id = p.addTrackAs (n, TrackDialogue, theme::trackPalette[index % theme::numPaletteColours], roleOutBus (TrackDialogue));
        p.setTrackGroup (id, p.addGroup (juce::translate ("Dialoghi")));   // le tracce dei personaggi stanno nel gruppo Dialoghi
        ++created;
    }
    return created;
}

int trackForCue (const Project& p, const AdrCue& cue)
{
    const auto n = cue.character.trim();
    if (n.isEmpty()) return -1;
    for (auto& t : p.getSession().tracks)
        if (t.name.trim().equalsIgnoreCase (n)) return t.id;
    return -1;
}
} // namespace fc::dub
