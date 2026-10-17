#pragma once

#include <JuceHeader.h>

namespace fc::manual
{
/** Apre nel browser il manuale incorporato nella lingua indicata ("it" o "en"): lo estrae una volta in una cartella temporanea. */
bool open (const juce::String& lang, juce::String& error);
} // namespace fc::manual
