#pragma once

#include "Model.h"

namespace fc::duck
{
struct Settings
{
    double thresholdDb = -45.0;   // sotto questo livello (RMS) il dialogo e' considerato assente
    double depthDb = 12.0;        // di quanto si abbassa il fondo
    double attackSec = 0.15;      // durata della discesa (che finisce prima che il parlato cominci)
    double releaseSec = 0.5;      // durata della risalita
    double marginSec = 0.10;      // margine prima e dopo il parlato
    double holdSec = 0.30;        // pause piu' corte di questa non fanno risalire il fondo
};

/** Intervalli (campioni) in cui parla almeno una traccia Dialoghi non in mute. */
std::vector<std::pair<int64_t, int64_t>> speechIntervals (const Session&, double rate, const Settings&);

/** Punti della curva: posizione (campioni) e attenuazione in dB (<= 0). Parte e finisce a 0 dB. */
std::vector<std::pair<int64_t, float>> duckCurve (const std::vector<std::pair<int64_t, int64_t>>& intervals, double rate, const Settings&);

/** Scrive la curva sul volume delle tracce M&E ed Effetti (se non ce ne sono, sulla traccia indicata), attorno al livello del loro fader.
    Un solo passo di undo; l'automazione dei punti gia' presenti nella zona viene sostituita e la traccia passa in lettura. Ritorna le tracce toccate. */
int apply (Project&, const Settings&, int fallbackTrackId);
} // namespace fc::duck
