#pragma once

#include "Model.h"

namespace fc::versions
{
struct Entry
{
    juce::File file;
    juce::String note;
    juce::Time time;
    int number = 0;
};

/** Cartella delle versioni di una sessione: <cartella>/<nome>_versioni. */
juce::File folderFor (const juce::File& sessionFile);

/** Salva una copia numerata della sessione (<nome>_vNNN_AAAAMMGG-HHMM.arostudio) con la nota in un file .txt accanto. Ritorna il file, o File() con 'error'. */
juce::File save (const Project&, const juce::String& note, juce::String& error);

/** Le versioni della sessione, dalla piu' recente. */
std::vector<Entry> list (const juce::File& sessionFile);

bool remove (const Entry&);
} // namespace fc::versions
