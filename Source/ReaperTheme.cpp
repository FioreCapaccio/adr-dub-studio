#include "ReaperTheme.h"

namespace fc::reaper
{
namespace
{
float luminance (juce::Colour c)
{
    auto lin = [] (float v) { return v <= 0.03928f ? v / 12.92f : std::pow ((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin (c.getFloatRed()) + 0.7152f * lin (c.getFloatGreen()) + 0.0722f * lin (c.getFloatBlue());
}

bool isLight (juce::Colour c) { return luminance (c) > 0.30f; }
juce::Colour mix (juce::Colour a, juce::Colour b, float t) { return a.interpolatedWith (b, t); }
} // namespace

float contrastRatio (juce::Colour a, juce::Colour b)
{
    const float la = luminance (a), lb = luminance (b);
    return (juce::jmax (la, lb) + 0.05f) / (juce::jmin (la, lb) + 0.05f);
}

juce::Colour fromColorRef (int v)
{
    const auto u = (juce::uint32) v;
    return juce::Colour ((juce::uint8) (u & 0xff), (juce::uint8) ((u >> 8) & 0xff), (juce::uint8) ((u >> 16) & 0xff));
}

ColorMap parseColorTheme (const juce::String& ini)
{
    ColorMap map;
    bool inSection = false;
    for (auto line : juce::StringArray::fromLines (ini))
    {
        line = line.trim();
        if (line.startsWithChar ('['))
        {
            inSection = line.equalsIgnoreCase ("[color theme]");
            continue;
        }
        if (! inSection) continue;
        const int eq = line.indexOfChar ('=');
        if (eq <= 0) continue;
        const auto value = line.substring (eq + 1).trim();
        if (value.isEmpty() || ! (value.containsOnly ("-0123456789"))) continue;
        map[line.substring (0, eq).trim().toLowerCase()] = (int) value.getLargeIntValue();
    }
    return map;
}

theme::Palette paletteFromColors (const ColorMap& m, const juce::String& name)
{
    auto get = [&m] (const char* key, juce::Colour& out)
    {
        auto it = m.find (key);
        if (it == m.end()) return false;
        out = fromColorRef (it->second);
        return true;
    };

    theme::Palette p;   // base: tema scuro dell'app, usato per tutto cio' che il tema non definisce
    p.name = name;

    // 1) i pannelli: il fondo del mixer (o dell'elenco tracce, o della finestra principale) decide se il tema e' chiaro o scuro
    juce::Colour panel;
    const bool havePanel = get ("col_mixerbg", panel) || get ("col_tracklistbg", panel) || get ("col_main_bg2", panel);
    if (! havePanel) panel = p.panel;
    const bool light = isLight (panel);
    const auto black = juce::Colours::black;
    const auto ink = light ? juce::Colour (0xff15171b) : juce::Colour (0xffe9ebef);   // testo di riserva

    // 2) testo: il primo colore del tema con un buon contrasto sul pannello, altrimenti quello di riserva
    juce::Colour text = ink;
    {
        float best = 0.0f;
        for (const char* key : { "col_main_text", "col_main_text2", "col_toolbar_text", "col_mi_label", "genlist_fg", "col_tcp_text" })
        {
            juce::Colour c;
            if (! get (key, c)) continue;
            const float r = contrastRatio (c, panel);
            if (r >= 7.0f) { text = c; best = r; break; }    // contrasto forte: e' il testo del tema
            if (r >= 4.5f && r > best) { text = c; best = r; }
        }
    }
    const auto dim = mix (text, panel, 0.45f);

    // 3) accento (stati attivi, selezioni): colore saturo del tema, altrimenti quello predefinito
    juce::Colour accent = p.blue;
    for (const char* key : { "genlist_selbg", "col_seltrack", "col_main_text2", "col_toolbar_text_on" })
    {
        juce::Colour c;
        if (get (key, c) && c.getSaturation() > 0.30f && c.getBrightness() > 0.25f) { accent = c; break; }
    }
    // l'accento deve staccare dal pannello ma anche reggere il testo bianco dei tasti attivi: si corregge a piccoli passi
    for (int i = 0; i < 8 && contrastRatio (accent, panel) < 1.6f; ++i) accent = light ? accent.darker (0.15f) : accent.brighter (0.15f);
    for (int i = 0; i < 8 && contrastRatio (accent, juce::Colours::white) < 3.0f; ++i) accent = accent.darker (0.15f);

    // 4) tracce e griglia: quelle del tema solo se hanno la stessa polarita' dei pannelli (altrimenti testo e meter sarebbero illeggibili)
    auto sameKind = [light] (juce::Colour c) { return isLight (c) == light; };
    const auto laneDefault = mix (panel, black, light ? 0.06f : 0.30f);
    juce::Colour lane = laneDefault, laneAlt;
    juce::Colour c1, c2;
    if (get ("col_tr1_bg", c1) && sameKind (c1)) lane = c1;
    else if (get ("col_arrangebg", c1) && sameKind (c1)) lane = c1;
    if (get ("col_tr2_bg", c2) && sameKind (c2) && c2 != lane) laneAlt = c2;
    else laneAlt = light ? lane.darker (0.04f) : lane.brighter (0.06f);

    juce::Colour grid = mix (lane, text, 0.09f);
    if (get ("arrange_vgrid", c1) && sameKind (c1) && c1 != lane && contrastRatio (c1, lane) > 1.08f) grid = c1;

    // 5) tutto il resto deriva dal pannello e dal testo
    p.panel = panel;
    p.bg = mix (panel, black, light ? 0.14f : 0.45f);
    p.panelHi = mix (panel, text, 0.07f);
    p.panelTop = mix (panel, text, 0.035f);
    p.border = mix (panel, text, 0.12f);
    p.lane = lane;
    p.laneAlt = laneAlt;
    p.grid = grid;
    p.text = text;
    p.dim = dim;
    p.blue = accent;
    p.muteCol = accent;
    // accent (rosso rec), playhead, green, yellow, solo restano quelli dell'app: i temi non li definiscono come colori
    if (light) { p.green = juce::Colour (0xff1f9d55); p.yellow = juce::Colour (0xffc98a00); p.soloCol = juce::Colour (0xffd9a100); p.accent = juce::Colour (0xffe03a28); }
    return p;
}

bool loadThemeFile (const juce::File& file, theme::Palette& out, juce::String& error)
{
    juce::String ini, name = file.getFileNameWithoutExtension();
    if (! file.existsAsFile()) { error = TRANS ("File non trovato."); return false; }

    juce::ZipFile zip (file);
    if (zip.getNumEntries() > 0)
    {
        int found = -1;
        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto fn = zip.getEntry (i)->filename;
            if (fn.endsWithIgnoreCase (".reapertheme") && ! fn.contains ("/") && ! fn.contains ("\\")) { found = i; break; }   // nella radice
            if (fn.endsWithIgnoreCase (".reapertheme") && found < 0) found = i;
        }
        if (found < 0) { error = TRANS ("Lo zip non contiene un file .ReaperTheme."); return false; }
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (found));
        if (in == nullptr) { error = "Impossibile leggere il tema."; return false; }
        ini = in->readEntireStreamAsString();
        name = juce::File (zip.getEntry (found)->filename).getFileNameWithoutExtension().replace ("_unpacked", "").replace (" unpacked", "");
    }
    else
        ini = file.loadFileAsString();

    const auto colors = parseColorTheme (ini);
    if (colors.empty()) { error = TRANS ("Il file non e' un tema di Reaper (nessun colore trovato)."); return false; }
    out = paletteFromColors (colors, name);
    return true;
}

juce::Array<juce::File> findInstalledThemes()
{
    juce::Array<juce::File> result;
#if JUCE_WINDOWS
    const auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("REAPER/ColorThemes");
#else
    const auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Application Support/REAPER/ColorThemes");
#endif
    for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
    {
        const auto f = e.getFile();
        if (f.getFileName().startsWithChar ('.')) continue;
        if (f.hasFileExtension ("ReaperThemeZip") || f.hasFileExtension ("ReaperTheme")) result.add (f);
    }
    struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareIgnoreCase (b.getFileName()); } } sorter;
    result.sort (sorter);
    return result;
}
} // namespace fc::reaper
