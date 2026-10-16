#include "Relink.h"

namespace fc::relink
{
std::vector<AssetPtr> offlineAssets (const Session& s)
{
    std::vector<AssetPtr> out;
    std::set<const Asset*> seen;
    for (auto& t : s.tracks)
        for (auto& c : t.clips)
            if (c.asset != nullptr && c.asset->offline && seen.insert (c.asset.get()).second) out.push_back (c.asset);
    return out;
}

Result search (juce::AudioFormatManager& fm, const Session& s, const juce::File& folder, double rate, const juce::String& videoName,
               const std::function<bool (double)>& progress)
{
    Result r;
    const auto missing = offlineAssets (s);

    // indice dei file della cartella per nome (minuscolo): un nome puo' comparire piu' volte, si prova il primo che ha la lunghezza giusta
    std::multimap<juce::String, juce::File> index;
    for (const auto& e : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
    {
        const auto f = e.getFile();
        if (! f.getFileName().startsWithChar ('.')) index.emplace (f.getFileName().toLowerCase(), f);
    }

    int done = 0;
    for (auto& a : missing)
    {
        if (progress && ! progress ((double) done / (double) juce::jmax<size_t> (1, missing.size()))) break;
        ++done;
        auto range = index.equal_range (a->file.getFileName().toLowerCase());
        bool ok = false, shortFile = false;
        for (auto it = range.first; it != range.second && ! ok; ++it)
        {
            juce::String err;
            auto loaded = loadAsset (fm, it->second, rate, err);
            if (loaded == nullptr) continue;
            if (loaded->length() < a->offlineLength) { shortFile = true; continue; }
            loaded->name = a->name;
            r.replacements[a.get()] = loaded;
            ++r.found;
            ok = true;
        }
        if (! ok) (shortFile ? r.tooShort : r.notFound).add (a->file.getFileName());
    }

    if (videoName.isNotEmpty())
    {
        auto range = index.equal_range (videoName.toLowerCase());
        if (range.first != range.second) r.video = range.first->second;
        else r.notFound.add (videoName);
    }
    if (progress) progress (1.0);
    return r;
}
} // namespace fc::relink
