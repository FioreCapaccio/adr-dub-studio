#pragma once

#include "Model.h"

namespace fc
{
/** Icone delle tracce (disegnate, nel colore della traccia) o immagine scelta dall'utente. Track::icon: 0 = quella del tipo di traccia, -1 = nessuna, 1.. = icona. */
constexpr int kNumTrackIcons = 4;   // 1 voice-over, 2 musica, 3 effetti, 4 guida; dal menu si scelgono solo voice-over ed effetti, gli altri sono le icone dei tipi di traccia
juce::String trackIconName (int icon);
/** L'icona che si vede davvero: quella scelta, altrimenti quella del tipo (dialogo, musica, effetti, guida); 0 = nessuna. */
int effectiveTrackIcon (int icon, int trackType);
void drawTrackIcon (juce::Graphics&, int icon, juce::Rectangle<float>, juce::Colour);
/** Immagine (se c'e') o icona della traccia; non disegna nulla se non ce n'e'. */
void drawTrackBadge (juce::Graphics&, const juce::Image& image, int icon, int trackType, juce::Rectangle<float>, juce::Colour);
bool hasTrackBadge (const juce::Image& image, int icon, int trackType);
/** Miniatura quadrata (ritagliata al centro, 96 px) di un file immagine; non valida se non si legge. */
juce::Image makeTrackImage (const juce::File&);
juce::String trackImageToBase64 (const juce::Image&);
juce::Image trackImageFromBase64 (const juce::String&);
} // namespace fc
