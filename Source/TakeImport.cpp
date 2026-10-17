#include "TakeImport.h"
#include "DubWorkflow.h"
#include <regex>

namespace fc::takes
{
namespace
{
juce::String alnum (const juce::String& s) { return s.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789"); }
}

bool parseName (const juce::String& name, Found& out)
{
    const std::string s = name.toStdString();
    std::smatch m;
    static const std::regex cueRx ("(?:^|[^A-Za-z])cue[ _-]*0*([0-9]+)", std::regex::icase);
    static const std::regex leadRx ("^0*([0-9]+)[ _-]+");
    size_t after = 0;   // dove finisce il numero del cue: da li' parte il personaggio
    if (std::regex_search (s, m, cueRx)) { out.cueNumber = std::stoi (m[1].str()); after = (size_t) (m.position (0) + m.length (0)); }
    else if (std::regex_search (s, m, leadRx)) { out.cueNumber = std::stoi (m[1].str()); after = (size_t) m.length (0); }
    else return false;
    if (out.cueNumber <= 0) return false;

    static const std::regex takeRx ("(?:^|[ _-])(?:take|t)[ _-]*0*([0-9]+)(?=$|[ _.-])", std::regex::icase);
    const std::string rest = s.substr (juce::jmin (after, s.size()));
    size_t charEnd = rest.size();
    out.take = 1;
    if (std::regex_search (rest, m, takeRx)) { out.take = juce::jmax (1, std::stoi (m[1].str())); charEnd = (size_t) m.position (0); }
    static const std::regex bestRx ("(?:^|[ _-])best(?=$|[ _.-])", std::regex::icase);
    out.best = std::regex_search (s, bestRx);
    if (charEnd == rest.size() && out.best) { std::smatch b; if (std::regex_search (rest, b, bestRx)) charEnd = (size_t) b.position (0); }

    // personaggio: "Prof_steeve" -> "Prof steeve"; si ignorano "sc3" (scena) e le cifre sole in coda
    auto chr = juce::String (rest.substr (0, charEnd)).replaceCharacter ('_', ' ').trim();
    chr = chr.trimCharactersAtStart ("-: ").trimCharactersAtEnd ("-: ");
    auto words = juce::StringArray::fromTokens (chr, " ", "");
    while (words.size() > 0 && words[words.size() - 1].matchesWildcard ("sc*", true) && words[words.size() - 1].substring (2).containsOnly ("0123456789") && words[words.size() - 1].length() > 2)
        words.remove (words.size() - 1);
    out.character = words.joinIntoString (" ").trim();
    return true;
}

std::vector<Found> scan (juce::AudioFormatManager& fm, const juce::File& folder, int& duplicates, int maxDepth)
{
    duplicates = 0;
    std::vector<Found> all;
    int examined = 0;
    const int rootDepth = juce::StringArray::fromTokens (folder.getFullPathName(), "/\\", "").size();
    for (const auto& e : juce::RangedDirectoryIterator (folder, true, "*.wav;*.aif;*.aiff;*.flac", juce::File::findFiles))
    {
        if (++examined > 20000) break;
        const auto f = e.getFile();
        if (f.getFileName().startsWith ("._")) continue;
        if (juce::StringArray::fromTokens (f.getFullPathName(), "/\\", "").size() - rootDepth - 1 > maxDepth) continue;   // sotto 'maxDepth' cartelle
        Found found;
        found.file = f;
        if (! parseName (f.getFileNameWithoutExtension(), found)) continue;
        if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (f) })
        {
            const auto ref = rd->metadataValues.getValue (juce::WavAudioFormat::bwavTimeReference, "");
            if (ref.isNotEmpty() && rd->sampleRate > 0.0) found.refSeconds = (double) ref.getLargeIntValue() / rd->sampleRate;
        }
        else continue;
        all.push_back (found);
    }

    auto pathDepth = [] (const Found& f) { return juce::StringArray::fromTokens (f.file.getFullPathName(), "/\\", "").size(); };
    // stesso nome (senza BEST) = copia dello stesso file in un'altra cartella; nomi diversi = registrazioni diverse dello stesso take
    auto key = [] (const Found& f) { return f.file.getFileNameWithoutExtension().toLowerCase().replace ("_best", "").replace ("-best", "").replace (" best", ""); };
    std::sort (all.begin(), all.end(), [&] (const Found& a, const Found& b)
    {
        if (a.cueNumber != b.cueNumber) return a.cueNumber < b.cueNumber;
        if (a.take != b.take) return a.take < b.take;
        if (key (a) != key (b)) return key (a) < key (b);
        if ((a.refSeconds >= 0.0) != (b.refSeconds >= 0.0)) return a.refSeconds >= 0.0;
        if (pathDepth (a) != pathDepth (b)) return pathDepth (a) < pathDepth (b);
        return a.file.getFullPathName() < b.file.getFullPathName();
    });
    std::vector<Found> out;
    for (auto& f : all)
    {
        if (! out.empty() && out.back().cueNumber == f.cueNumber && out.back().take == f.take && key (out.back()) == key (f)) { ++duplicates; out.back().best = out.back().best || f.best; continue; }
        out.push_back (f);
    }
    return out;
}

int countMatches (const Project& p, const std::vector<Found>& found)
{
    int n = 0;
    for (auto& f : found)
        for (auto& c : p.getSession().cues)
            if (c.number == f.cueNumber) { ++n; break; }
    return n;
}

Plan plan (Project& p, const std::vector<Found>& found, double fallbackRate)
{
    Plan pl;
    std::set<int> cueIds;
    int notFound = 0, otherCharacter = 0, alreadyThere = 0;
    juce::String firstMissing;
    for (auto& f : found)
    {
        const AdrCue* cue = nullptr;
        for (auto& c : p.getSession().cues) if (c.number == f.cueNumber) { cue = &c; break; }
        if (cue == nullptr) { if (notFound++ == 0) firstMissing = f.file.getFileName(); continue; }
        if (f.character.isNotEmpty() && cue->character.trim().isNotEmpty() && alnum (f.character) != alnum (cue->character)) { ++otherCharacter; continue; }

        bool dup = false;   // lo stesso file e' gia' una take di questo cue
        for (auto& t : p.getSession().tracks) for (auto& c : t.clips) dup = dup || (c.cueId == cue->id && c.asset != nullptr && c.asset->file == f.file);
        if (dup) { ++alreadyThere; continue; }

        int trackId = dub::trackForCue (p, *cue);
        if (trackId < 0)
        {
            // la traccia del personaggio non c'e': si crea (nel gruppo Dialoghi), altrimenti la traccia ADR / selezionata
            std::vector<AdrCue> one { *cue };
            if (cue->character.trim().isNotEmpty() && dub::createCharacterTracks (p, one) > 0) trackId = dub::trackForCue (p, *cue);
            if (trackId < 0) trackId = p.findTrack (p.adrTrackId) != nullptr ? p.adrTrackId : (p.findTrack (p.selectedTrackId) != nullptr ? p.selectedTrackId : p.addTrack());
        }

        Project::TakeFile item;
        item.trackId = trackId;
        item.file = f.file;
        item.cueId = cue->id;
        item.takeNo = f.take;
        item.best = f.best;
        const double rate = p.getSampleRate() > 0.0 ? p.getSampleRate() : fallbackRate;
        item.startPos = f.refSeconds >= 0.0 ? (int64_t) std::llround ((f.refSeconds - p.tcStartSec) * rate) : cue->inPos;
        pl.items.push_back (item);
        cueIds.insert (cue->id);
    }
    pl.cues = (int) cueIds.size();
    if (notFound > 0) pl.notes.add (juce::String (notFound) + " file non hanno un cue con quel numero nella sessione (primo: " + firstMissing + ").");
    if (otherCharacter > 0) pl.notes.add (juce::String (otherCharacter) + " file hanno un personaggio diverso da quello del cue e sono stati saltati.");
    if (alreadyThere > 0) pl.notes.add (juce::String (alreadyThere) + " take erano gia' nella sessione.");
    return pl;
}
} // namespace fc::takes
