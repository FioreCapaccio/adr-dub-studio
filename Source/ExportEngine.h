#pragma once

#include "AudioEngine.h"

namespace fc
{
struct ExportOptions
{
    juce::String format = "wav";      // "wav", "aiff", "flac"
    int bitDepth = 24;                // 16, 24, 32 (32 = float nei WAV, intero negli AIFF; i FLAC arrivano a 24)
    double outRate = 0.0;             // 0 = come la sessione
    bool loopRange = false;           // esporta solo la regione di loop
    int64_t rangeStart = 0, rangeEnd = 0;   // campioni alla frequenza della sessione

    enum class Norm { None, Peak, Lufs };
    Norm norm = Norm::None;
    double normTarget = -1.0;         // dBFS (Peak) oppure LUFS (Lufs)
    double ceilingDb = -1.0;          // tetto di true-peak: se la normalizzazione lo supera interviene un limiter
    bool dither = true;               // dither TPDF a 16 bit
    bool stems = false;               // un file per traccia (senza bus master)
    bool excludeGuide = true;         // le tracce con ruolo Guida (dialogo originale) non entrano in mix e stem
    bool delivery = false;            // pacchetto di consegna: MIX + stem per ruolo (Dialoghi, M&E, Effetti) + rapporto; target = cartella
    juce::String presetName;          // nome del preset (solo per il rapporto)
};

/** Un file del pacchetto di consegna con le sue misure (dopo il guadagno di normalizzazione). */
struct ExportItem { juce::String file; double peakDb = -120.0, truePeakDb = -120.0, lufs = -100.0, lra = 0.0; };

struct ExportResult
{
    bool ok = false;
    juce::String message;
    juce::StringArray files;
    double peakDb = -120.0, truePeakDb = -120.0, lufs = -100.0, lra = 0.0;   // misurati sul file finale (non per gli stem)
    double gainDb = 0.0, limiterGrDb = 0.0;
    std::vector<ExportItem> items;    // pacchetto di consegna
    juce::String reportFile;          // rapporto di consegna (HTML)
};

/** Esporta la sessione. target: il file (mixdown) oppure la cartella (stem). baseName: prefisso dei nomi degli stem. */
ExportResult exportSession (const Session& session, double projectRate, const ExportOptions&, const juce::File& target,
                            const juce::String& baseName, const std::function<bool (double)>& progress);

/** Freeze: rende l'audio di una traccia con guadagno in ingresso, EQ, compressore e plug-in (prima del fader), come file WAV a 32 bit float. */
bool renderTrackProcessed (const Session& source, int trackId, double rate, int64_t totalSamples, const juce::File& dest,
                           const std::function<bool (double)>& progress, juce::String& error);

} // namespace fc
