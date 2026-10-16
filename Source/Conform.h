#pragma once

#include "Model.h"

namespace fc::conform
{
/** Un evento video di un EDL CMX3600 (tempi in secondi di timecode assoluto). */
struct Event
{
    int number = 0;
    juce::String reel, clipName, transition;
    double srcIn = 0.0, srcOut = 0.0, recIn = 0.0, recOut = 0.0;
    bool retimed = false;   // seguito da una riga M2 (cambio di velocita'): non supportato
};

struct Parsed
{
    std::vector<Event> events;
    juce::StringArray warnings;
    int formatIndex = -1;   // indice in tcFormats() dichiarato dall'EDL (FCM), -1 = non dichiarato
};

/** Legge gli eventi video dell'EDL con il formato di timecode indicato (se l'EDL ne dichiara uno drop-frame si usa quello).
    Gli eventi con timecode non validi per il formato vengono saltati e segnalati. */
Parsed parseEdl (const juce::String& text, int tcFormatIndex);

struct Map
{
    struct Seg { double oldStart, oldEnd, newStart; };   // secondi di timecode assoluto
    std::vector<Seg> segments;
    std::vector<std::pair<double, double>> coverage;     // zone del vecchio montaggio
    std::vector<std::pair<double, double>> removed;      // materiale del vecchio montaggio che nel nuovo non c'e'
    std::vector<std::pair<double, double>> inserted;     // materiale del nuovo montaggio che nel vecchio non c'era
    juce::StringArray warnings;
};

/** Confronta i due montaggi per sorgente (bobina e nome del clip) e intervallo di sorgente. */
Map buildMap (const std::vector<Event>& oldEvents, const std::vector<Event>& newEvents);

/** Piano in campioni di timeline per Project::conform (tcStartSec = timecode della posizione 0). */
ConformPlan makePlan (const Map&, double tcStartSec, double sampleRate, bool markInserted);

/** Testo del rapporto (anteprima o esito). */
juce::String describe (const Map&, const ConformStats&, double sampleRate);
} // namespace fc::conform
