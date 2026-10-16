#pragma once

#include "ReaperTheme.h"
#include "Settings.h"

namespace fc
{
/** Un tema selezionabile: integrato oppure importato da Reaper (di questo si conservano solo i colori). */
struct ThemeEntry
{
    juce::String id, name;
    theme::Palette palette;
    bool builtin = true;
};

/** Elenco e applicazione dei temi, e scelta salvata nelle preferenze ("theme"). */
class ThemeManager
{
public:
    static std::vector<ThemeEntry> builtins();
    static std::vector<ThemeEntry> imported();                         // da <cartella dati dell'app>/themes/*.json
    static std::vector<ThemeEntry> all();
    static juce::File themesDir();

    /** Legge un tema Reaper e lo salva fra quelli importati (stesso nome = sostituzione). */
    static bool importReaperTheme (const juce::File&, ThemeEntry& out, juce::String& error);
    static void remove (const ThemeEntry&);

    static juce::String currentId();
    /** Applica il tema e lo ricorda; ritorna false se l'id non esiste. */
    static bool select (const juce::String& id);
    /** All'avvio: applica il tema salvato (o lascia quello scuro). */
    static void applySaved();
};
} // namespace fc
