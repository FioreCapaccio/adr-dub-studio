#pragma once

#include <JuceHeader.h>

/** Segna una stringa da tradurre dentro tabelle di const char*: la traduzione si fa dove viene mostrata (juce::translate).
    Nel resto del codice le stringhe visibili usano TRANS("..."). Il testo italiano e' la chiave del catalogo. */
#define TRX(s) s

namespace fc::lang
{
struct Language { const char* code; const char* name; };

/** Le lingue disponibili (la prima e' l'italiano, che non ha catalogo). */
const std::vector<Language>& languages();

/** Codice della lingua scelta nelle preferenze (predefinita: italiano). */
juce::String savedCode();
void saveCode (const juce::String& code);

/** Carica il catalogo della lingua (stringa vuota o "it" = italiano) e lo rende attivo per tutte le chiamate TRANS/translate. Da chiamare all'avvio. */
void apply (const juce::String& code);

/** Testo grezzo del catalogo incorporato per la lingua (vuoto se non esiste). Serve ai controlli. */
juce::String catalogText (const juce::String& code);

/** Traduce un testo con il catalogo di una lingua precisa (anche se non e' quella attiva): per avvisare nella lingua appena scelta. */
juce::String translateTo (const juce::String& code, const juce::String& italian);
} // namespace fc::lang
