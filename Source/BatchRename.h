#pragma once

#include <JuceHeader.h>

namespace fc::rename
{
struct Context
{
    int index = 1;                  // numero progressivo (parte dal valore scelto)
    int digits = 2;                 // cifre del numero ({n} = 01, 02...)
    juce::String original, track, character;
    int cue = 0;
};

/** Nuovo nome da uno schema: {n} numero progressivo, {nome} nome attuale, {traccia}, {personaggio}, {cue}. Prima si applica trova/sostituisci al nome attuale. */
inline juce::String expand (const juce::String& pattern, const Context& c, const juce::String& find = {}, const juce::String& replaceWith = {})
{
    juce::String orig = c.original;
    if (find.isNotEmpty()) orig = orig.replace (find, replaceWith, true);
    return pattern.replace ("{nome}", orig).replace ("{traccia}", c.track).replace ("{personaggio}", c.character)
                  .replace ("{cue}", juce::String (c.cue)).replace ("{n}", juce::String (c.index).paddedLeft ('0', c.digits)).trim();
}
} // namespace fc::rename
