#pragma once

#include "Model.h"

namespace fc::dub
{
struct TemplateTrack { const char* name; int type; };   // type = ruolo (TrackType)
struct SessionTemplate { const char* id; const char* name; const char* description; std::vector<TemplateTrack> tracks; };

/** Modelli di sessione per il doppiaggio. Ognuno crea tracce con ruolo, preset della catena, colore e bus di uscita del ruolo. */
const std::vector<SessionTemplate>& templates();

/** Nuova sessione dal modello (sostituisce quella corrente: va chiamata dopo aver confermato lo scarto delle modifiche). */
void applyTemplate (Project&, const SessionTemplate&);

/** Nome di un take secondo lo schema ({personaggio} {scena} {bobina} {cue} {take} {data} {progetto}). Il segnaposto {take} resta
    cosi' com'e' (lo sostituisce Project::addRecordings con il numero del take); i campi vuoti non lasciano trattini doppi. */
juce::String expandNaming (const juce::String& scheme, const AdrCue&, const juce::String& projectName);

/** Crea una traccia Dialoghi per ogni personaggio dei cue che non ne ha gia' una (stesso nome). Ritorna quante ne ha create. */
int createCharacterTracks (Project&, const std::vector<AdrCue>&);

/** Traccia su cui registrare il cue: quella col nome del personaggio, altrimenti -1. */
int trackForCue (const Project&, const AdrCue&);
} // namespace fc::dub
