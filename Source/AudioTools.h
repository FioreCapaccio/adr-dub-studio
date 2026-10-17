#pragma once

#include "Model.h"

namespace fc::tools
{
/** Zone con suono dentro una porzione di audio (campioni relativi alla porzione): le pause piu' corte di 'minSilenceSec' si ignorano,
    ogni zona si allarga di 'padSec' per lato (come Detect Silence / Strip Silence). */
std::vector<std::pair<int64_t, int64_t>> soundRegions (const juce::AudioBuffer<float>& audio, int64_t offset, int64_t length, double rate,
                                                       float thresholdDb, double minSilenceSec, double padSec);

/** Loudness integrato (LUFS, BS.1770) di una porzione di audio; -100 se e' silenzio. 'dialogueOnly' misura solo i blocchi in cui c'e' parlato
    (energia sopra la soglia di silenzio), come la misura "dialogue-gated". */
double measureLufs (const juce::AudioBuffer<float>& audio, int64_t offset, int64_t length, double rate, bool dialogueOnly = false);

/** Percentuale (0..100) della porzione in cui c'e' parlato (blocchi di 400 ms sopra la soglia). */
double speechPercent (const juce::AudioBuffer<float>& audio, int64_t offset, int64_t length, double rate);

enum class Op { Reverse, RemoveDc, InvertPolarity, NormalizeLufs, NormalizeDialogueLufs };
} // namespace fc::tools
