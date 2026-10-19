#pragma once

#include <JuceHeader.h>
#include <vector>

/** Divisione del parlato in voce maschile e femminile dal tono (F0, metodo YIN) frase per frase.
    Non separa due persone che parlano insieme (la frase va alla voce dominante) e puo' sbagliare su bambini, voci molto gravi o acute,
    sussurri e grida: i segmenti incerti sono segnalati. I tagli cadono a meta' delle pause, quindi i segmenti coprono tutto l'audio. */
namespace fc::gender
{
struct VoiceSeg
{
    int64_t start = 0, length = 0;   // campioni dell'audio analizzato
    bool female = false;
    bool uncertain = false;
    float f0 = 0.0f;                 // tono mediano (Hz), 0 se non sonoro
};

struct Settings
{
    double pauseSec = 0.12;          // pause piu' lunghe di cosi' possono separare due voci
    double fixedThresholdHz = 165.0; // soglia di partenza tra voce maschile e femminile
    double minThresholdHz = 140.0, maxThresholdHz = 190.0;   // limiti della soglia adattiva
    double ambiguityRatio = 1.08;    // entro questo rapporto dalla soglia la frase e' ambigua
};

/** Segmenti contigui (senza buchi) con il genere di ciascuno; vuoto se non c'e' parlato. Segmenti vicini dello stesso genere sono uniti. */
std::vector<VoiceSeg> analyse (const float* mono, int64_t numSamples, double sampleRate, const Settings& = {});

/** Come analyse() su piu' canali (si fa la media). */
std::vector<VoiceSeg> analyse (const juce::AudioBuffer<float>&, int64_t offset, int64_t length, double sampleRate, const Settings& = {});

/** Ci sono entrambi i generi? Se no, non ha senso dividere. */
bool hasBoth (const std::vector<VoiceSeg>&);
} // namespace fc::gender
