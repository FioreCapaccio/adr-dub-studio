#pragma once

#include "Model.h"
#include <map>

namespace fc::relink
{
/** Asset offline (file non trovato) dei clip della sessione, senza duplicati. */
std::vector<AssetPtr> offlineAssets (const Session&);

struct Result
{
    std::map<const Asset*, AssetPtr> replacements;   // asset offline -> asset ricaricato
    juce::File video;                                // video mancante trovato (vuoto se no)
    juce::StringArray notFound, tooShort;
    int found = 0;
};

/** Cerca per nome di file (anche nelle sottocartelle) i media mancanti della sessione e li ricarica. Un file piu' corto di quanto
    richiedono i clip viene rifiutato. videoName: nome del video mancante (vuoto = non cercarlo). Va chiamata fuori dal thread messaggi. */
Result search (juce::AudioFormatManager&, const Session&, const juce::File& folder, double rate, const juce::String& videoName,
               const std::function<bool (double)>& progress);
} // namespace fc::relink
