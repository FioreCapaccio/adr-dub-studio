#pragma once

#include "Model.h"

namespace fc::logic
{
enum class Field { Name, Track, Character, Length, Gain, Muted, Rating, Start };
enum class Op { Contains, Equals, Greater, Less };
struct Cond { Field field = Field::Name; Op op = Op::Contains; juce::String value; };
enum class Action { Select, Mute, Unmute, Delete, SetGain, AddGain, SetFadeMs, Rate, Reject };

/** Id dei clip che soddisfano tutte le condizioni (Muted: valore 1/0, Length e Start in secondi, Gain in dB). Una lista vuota seleziona tutti i clip. */
std::vector<int64_t> matching (const Project&, const std::vector<Cond>&);

/** Applica l'azione ai clip dati (tracce bloccate escluse) in un solo passo di undo; ritorna quanti clip sono cambiati. */
int apply (Project&, const std::vector<int64_t>& ids, Action, double param);

juce::StringArray fieldNames();
juce::StringArray opNames();
juce::StringArray actionNames();
} // namespace fc::logic
