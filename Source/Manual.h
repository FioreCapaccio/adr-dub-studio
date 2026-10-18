#pragma once

#include <JuceHeader.h>

namespace fc::manual
{
/** Ripiego: apre nel browser il manuale incorporato nella lingua indicata ("it" o "en"): lo estrae una volta in una cartella temporanea. */
bool open (const juce::String& lang, juce::String& error);

/** Finestra con il manuale dentro l'app (pagine servite dalla memoria, senza file temporanei ne' browser). nullptr se il motore web del sistema non e' disponibile. */
std::unique_ptr<juce::DocumentWindow> createWindow (const juce::String& lang);
} // namespace fc::manual
