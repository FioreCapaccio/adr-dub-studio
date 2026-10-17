#include "ThemeManager.h"

namespace fc
{
namespace
{
juce::String idFor (const juce::String& name)
{
    juce::String id = "reaper-" + name.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789 -_").replaceCharacters (" _", "--");
    return id;
}
} // namespace

juce::File ThemeManager::themesDir() { return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ArotronStudio/themes"); }

std::vector<ThemeEntry> ThemeManager::builtins()
{
    std::vector<ThemeEntry> v;
    v.push_back ({ "dark", TRANS ("Scuro"), theme::Palette::dark(), true });
    v.push_back ({ "light", TRANS ("Chiaro"), theme::Palette::light(), true });
    v.push_back ({ "contrast", TRANS ("Alto contrasto"), theme::Palette::highContrast(), true });
    // il tema predefinito di Reaper 5.0 (grigio neutro con accenti turchesi)
    static const char* default5 = R"({ "name": "Default 5", "bg": "FF1C1C1C", "panel": "FF333333", "panelHi": "FF3B3B3B", "panelTop": "FF373737", "lane": "FF232323", "laneAlt": "FF2F2F2F",
        "grid": "FF2F2F2F", "border": "FF414141", "text": "FFAAAAAA", "dim": "FF747474", "accent": "FFFF5A47", "blue": "FF466168", "playhead": "FFFF4136",
        "green": "FF4CD27A", "yellow": "FFFFC94D", "muteCol": "FF466168", "soloCol": "FFFFC94D" })";
    v.push_back ({ "default5", "Default 5", theme::Palette::fromVar (juce::JSON::parse (default5)), true });
    return v;
}

std::vector<ThemeEntry> ThemeManager::imported()
{
    std::vector<ThemeEntry> v;
    for (const auto& e : juce::RangedDirectoryIterator (themesDir(), false, "*.json", juce::File::findFiles))
    {
        const auto var = juce::JSON::parse (e.getFile());
        if (! var.isObject()) continue;
        ThemeEntry t;
        t.palette = theme::Palette::fromVar (var);
        t.name = t.palette.name.isNotEmpty() ? t.palette.name : e.getFile().getFileNameWithoutExtension();
        t.id = e.getFile().getFileNameWithoutExtension();
        t.builtin = false;
        v.push_back (t);
    }
    std::sort (v.begin(), v.end(), [] (const ThemeEntry& a, const ThemeEntry& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return v;
}

std::vector<ThemeEntry> ThemeManager::all() { return builtins(); }   // i temi importati da Reaper non si usano piu': restano solo i quattro predefiniti

bool ThemeManager::importReaperTheme (const juce::File& file, ThemeEntry& out, juce::String& error)
{
    theme::Palette p;
    if (! reaper::loadThemeFile (file, p, error)) return false;
    out.name = p.name;
    out.id = idFor (p.name);
    out.palette = p;
    out.builtin = false;
    const auto dest = themesDir().getChildFile (out.id + ".json");
    if (! themesDir().createDirectory() || ! dest.replaceWithText (juce::JSON::toString (p.toVar())))
    {
        error = TRANS ("Impossibile salvare il tema.");
        return false;
    }
    return true;
}

void ThemeManager::remove (const ThemeEntry& t)
{
    if (! t.builtin) themesDir().getChildFile (t.id + ".json").deleteFile();
}

juce::String ThemeManager::currentId()
{
    const auto id = settings().getValue ("theme", "dark");
    return id == "reaper-default-50" ? juce::String ("default5") : id;   // il vecchio tema importato
}

bool ThemeManager::select (const juce::String& id)
{
    for (auto& t : all())
        if (t.id == id)
        {
            theme::apply (t.palette);
            settings().setValue ("theme", id);
            settings().saveIfNeeded();
            return true;
        }
    return false;
}

void ThemeManager::applySaved()
{
    const auto id = currentId();
    if (id != "dark" && ! select (id)) settings().setValue ("theme", "dark");
}
} // namespace fc
