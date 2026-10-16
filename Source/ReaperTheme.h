#pragma once

#include "Theme.h"
#include <map>

namespace fc::reaper
{
/** Colori della sezione [color theme] di un .ReaperTheme (valori COLORREF come interi). */
using ColorMap = std::map<juce::String, int>;

/** Legge la sezione [color theme] di un file .ReaperTheme (testo ini). */
ColorMap parseColorTheme (const juce::String& iniText);

/** Colore da un valore COLORREF di Reaper (0x00BBGGRR; byte alti e flag di fusione ignorati). */
juce::Colour fromColorRef (int v);

/** Costruisce la palette dell'app dai colori di un tema Reaper. Dei colori del tema usa i pannelli, le tracce, la griglia, l'accento e il
    testo; il resto (e le chiavi mancanti) viene derivato in modo coerente, con un controllo di contrasto sul testo. */
theme::Palette paletteFromColors (const ColorMap&, const juce::String& name);

/** Carica un .ReaperThemeZip / .zip (cerca il .ReaperTheme nello zip) oppure un .ReaperTheme semplice. Dei file del tema non copia nulla. */
bool loadThemeFile (const juce::File&, theme::Palette& out, juce::String& error);

/** I temi installati da Reaper (cartella ColorThemes dell'utente). */
juce::Array<juce::File> findInstalledThemes();

/** Rapporto di contrasto (1..21) fra due colori, come nelle linee guida WCAG. */
float contrastRatio (juce::Colour a, juce::Colour b);
} // namespace fc::reaper
