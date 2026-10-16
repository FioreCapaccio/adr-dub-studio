#pragma once

#include "Model.h"

namespace fc::align
{
/** Inviluppo (RMS mono) in finestre di 'hop' campioni. */
std::vector<float> envelope (const juce::AudioBuffer<float>& b, int hop);

struct Shift
{
    bool ok = false;
    double samples = 0.0;      // di quanto spostare il target (positivo = piu' tardi) per sovrapporlo al riferimento
    double confidence = 0.0;   // correlazione normalizzata 0..1 (sotto ~0,3 il risultato non e' affidabile)
};

/** Ritardo da dare a 'target' per sovrapporlo a 'ref' (due finestre mono della stessa lunghezza sulla stessa base dei tempi).
    Cerca fra -maxShiftSec e +maxShiftSec sull'inviluppo (passo 10 ms) e poi affina a 1 ms. */
Shift findShift (const juce::AudioBuffer<float>& ref, const juce::AudioBuffer<float>& target, double rate, double maxShiftSec);

struct Speech { bool ok = false; int64_t start = 0, end = 0; };   // campioni nella finestra
/** Primo e ultimo punto in cui l'inviluppo supera (picco - rangeDb), con un minimo di durata per ignorare i click. */
Speech speechBounds (const juce::AudioBuffer<float>& b, double rate, double rangeDb = 30.0);

/** Finestra mono [t0, t0 + n) con l'audio dei clip dati (posizione, offset e guadagno del clip; fade e fader ignorati). */
juce::AudioBuffer<float> renderMono (const std::vector<const Clip*>& clips, int64_t t0, int64_t n);

struct Item
{
    int64_t clipId = 0;
    double shiftMs = 0.0, confidence = 0.0;
    bool applied = false;
    double stretchRatio = 1.0;   // != 1 se serve adattare la durata (solo con adaptDuration)
    int64_t stretchStart = -1;   // nuova posizione del clip dopo lo stretch
    juce::String note;
};

/** Allinea i clip alla traccia Guida (le tracce con ruolo Guida, non in mute). Sposta i clip in un solo passo di undo; se adaptDuration
    calcola anche il rapporto per far coincidere inizio e fine del parlato (lo stretch lo applica chi chiama: e' asincrono). */
std::vector<Item> alignToGuide (Project&, const std::vector<int64_t>& clipIds, double maxShiftSec, double minConfidence, bool adaptDuration);
} // namespace fc::align
