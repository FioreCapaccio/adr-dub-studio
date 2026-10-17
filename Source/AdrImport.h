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
    int maxFrameField = -1;              // il piu' alto numero di fotogrammi scritto nei timecode del file (indica la frequenza vera del file)
    bool dropFrameMarks = false;         // i timecode usano ';' prima dei fotogrammi (drop-frame)
};

/** Formato di timecode con cui interpretare il file: quello dichiarato dal file, altrimenti il piu' piccolo che contiene tutti i fotogrammi
    scritti (un file con fotogrammi fino a 29 non puo' essere a 24 fps). Ritorna l'indice in tcFormats(); 'current' se quello attuale va bene. */
int suggestedTcFormat (const ImportResult&, int current);

/** File che l'importazione dei cue sa leggere (per il trascinamento sulla timeline e l'apertura da Finder). */
inline bool isCueFile (const juce::File& f) { return f.hasFileExtension ("csv;tsv;tab;edl;srt;vtt;txt"); }

/** Riconosce il formato e legge i cue. result.cues e' vuoto se il file non e' riconosciuto. */
ImportResult parse (const juce::String& text);

/** Lettura di una riga di commenti "CHARACTER: Marco | SCENE: 12 | OUT: 01:00:14:00 | DIALOGUE: ..." (usata anche per riesportare). */
void parseCommentFields (const juce::String& comment, ImportedCue& cue);

/** Converte i cue letti in cue di sessione, usando formato e inizio del timecode del progetto.
    Le durate mancanti diventano defaultDurationSec. Gli avvisi dicono cosa e' stato scartato. */
std::vector<AdrCue> toCues (const ImportResult&, const Project&, double sampleRate, double defaultDurationSec, juce::StringArray& warnings);

} // namespace fc::adr
