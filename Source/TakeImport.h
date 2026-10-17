#pragma once

#include "Model.h"

namespace fc::takes
{
/** Un file audio di una take registrata, riconosciuto dal nome ("Cue012_Marco_Take2_BEST.wav", "12_Marco_sc3_T2.wav"). */
struct Found
{
    juce::File file;
    int cueNumber = 0;
    juce::String character;
    int take = 1;
    bool best = false;            // nel nome c'e' BEST: e' la take scelta
    double refSeconds = -1.0;     // posizione scritta nel file (BWF TimeReference) in secondi dalla mezzanotte; -1 se manca
};

/** Legge numero del cue, personaggio, numero di take e BEST dal nome del file. Ritorna false se il nome non indica un cue. */
bool parseName (const juce::String& fileNameWithoutExtension, Found& out);

/** Cerca i file audio nella cartella e in quelle sotto (fino a 'maxDepth'). Se lo stesso cue/take c'e' piu' volte (esportazioni per programmi
    diversi) tiene un solo file: prima quelli con la posizione incorporata, poi il percorso piu' corto. 'duplicates' conta gli scartati. */
std::vector<Found> scan (juce::AudioFormatManager&, const juce::File& folder, int& duplicates, int maxDepth = 4);

struct Plan
{
    std::vector<Project::TakeFile> items;
    juce::StringArray notes;     // avvisi: cue non trovati, personaggio diverso...
    int cues = 0;                // cue distinti che ricevono take
};

/** Quanti file hanno un cue con quel numero nella sessione (per proporre l'importazione, senza modificare nulla). */
int countMatches (const Project&, const std::vector<Found>&);

/** Abbina i file ai cue della sessione per numero (e personaggio, se il file lo dice), crea le tracce dei personaggi che mancano e decide dove
    cade ogni take: alla posizione scritta nel file, altrimenti all'ingresso del cue. */
Plan plan (Project&, const std::vector<Found>&, double fileRateFallback = 48000.0);
} // namespace fc::takes
