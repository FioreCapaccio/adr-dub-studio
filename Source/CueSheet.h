#pragma once

#include "Model.h"

namespace fc::cuesheet
{
/** Foglio cue in HTML pronto per la stampa (o per "Salva come PDF" dal browser): riepilogo con tutti i cue e, per ogni personaggio, il suo copione. */
juce::String buildHtml (const Project&, const juce::String& title);
} // namespace fc::cuesheet
