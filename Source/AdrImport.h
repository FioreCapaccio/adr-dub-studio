#pragma once

#include "Model.h"
#include <vector>

namespace fc::adr
{
/** Un cue come letto da un file esterno (i tempi sono ancora testo: dipendono dal formato di timecode della sessione). */
struct ImportedCue
{
    juce::String number, character, scene, reel, dialogue, notes, name;
    juce::String tcIn, tcOut;     // "hh:mm:ss:ff" (assoluto), oppure vuoto
    double samplesIn = -1.0, samplesOut = -1.0;   // se il file riporta i campioni (Pro Tools) e il timecode manca
};

struct ImportResult
{
    juce::String format;                 // "Pro Tools (testo Session Info)", "EDL", "Tabella"
    std::vector<ImportedCue> cues;
    juce::String sessionStartTc;         // se il file lo dichiara (Pro Tools)
    int tcFormatIndex = -1;              // indice in tcFormats() se il file dichiara frequenza e drop
    double sampleRate = 0.0;             // se dichiarata
    juce::StringArray warnings;
};

/** Riconosce il formato e legge i cue. result.cues e' vuoto se il file non e' riconosciuto. */
ImportResult parse (const juce::String& text);

/** Lettura di una riga di commenti "CHARACTER: Marco | SCENE: 12 | OUT: 01:00:14:00 | DIALOGUE: ..." (usata anche per riesportare). */
void parseCommentFields (const juce::String& comment, ImportedCue& cue);

/** Converte i cue letti in cue di sessione, usando formato e inizio del timecode del progetto.
    Le durate mancanti diventano defaultDurationSec. Gli avvisi dicono cosa e' stato scartato. */
std::vector<AdrCue> toCues (const ImportResult&, const Project&, double sampleRate, double defaultDurationSec, juce::StringArray& warnings);

} // namespace fc::adr
