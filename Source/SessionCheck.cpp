#include "SessionCheck.h"
#include "Relink.h"
#include "TrackPresets.h"

namespace fc::check
{
using namespace theme;

std::vector<Issue> checkSession (const Project& p)
{
    std::vector<Issue> out;
    const auto& s = p.getSession();
    auto add = [&out] (Issue::Level l, const juce::String& t, int cue = 0, int track = 0) { out.push_back ({ l, t, cue, track }); };

    int clips = 0;
    for (auto& t : s.tracks) clips += (int) t.clips.size();
    if (clips == 0 && s.cues.empty()) add (Issue::Warning, "La sessione e' vuota: nessun clip e nessun cue.");

    // media mancanti
    for (auto& a : relink::offlineAssets (s)) add (Issue::Error, TRANS ("File mancante: ") + a->file.getFullPathName() + TRANS ("  (File > Ricollega i media)"));
    if (s.video == nullptr && p.missingVideoPath.isNotEmpty()) add (Issue::Error, "Video mancante: " + p.missingVideoPath + "  (File > Ricollega i media)");

    // ruoli delle tracce
    for (auto& t : s.tracks)
    {
        if (t.clips.empty()) continue;
        if (t.live->trackType.load() == TrackNone)
            add (Issue::Warning, TRANS ("La traccia \"") + t.name + TRANS ("\" ha dei clip ma nessun ruolo (Dialoghi, M&E, Effetti o Guida): non finisce negli stem."), 0, t.id);
        else if (t.live->outBus.load() != roleOutBus (t.live->trackType.load()))
            add (Issue::Info, TRANS ("La traccia \"") + t.name + TRANS ("\" non e' sul bus del suo ruolo (") + trackTypeName (t.live->trackType.load()) + TRANS ("): negli stem non compare."), 0, t.id);
        if (t.live->mute.load()) add (Issue::Info, TRANS ("La traccia \"") + t.name + TRANS ("\" e' in mute."), 0, t.id);
    }

    // cue
    int toRecord = 0, redo = 0, noTake = 0, recorded = 0;
    const AdrCue* firstToRecord = nullptr;
    for (auto& c : s.cues)
    {
        if (c.status == 0) { ++toRecord; if (firstToRecord == nullptr) firstToRecord = &c; }
        else if (c.status == 3) { ++redo; add (Issue::Warning, "Cue " + juce::String (c.number) + " (" + c.character + ") da rifare.", c.id); }
        else if (p.takeCount (c.id) == 0) { ++noTake; add (Issue::Warning, "Cue " + juce::String (c.number) + " segnato come registrato ma senza take.", c.id); }
        if (c.status == 1) ++recorded;
    }
    if (toRecord > 0)
        add (Issue::Warning, juce::String (toRecord) + TRANS (" cue ancora da registrare (il primo e' il ") + juce::String (firstToRecord->number) + ").", firstToRecord->id);
    if (recorded > 0) add (Issue::Info, juce::String (recorded) + TRANS (" cue registrati ma non ancora approvati."));

    // video e timecode
    if (s.video != nullptr)
    {
        const double fps = s.video->fps;
        if (std::abs (fps - p.tcFmt().fps) > 0.1)
            add (Issue::Warning, TRANS ("Il video e' a ") + juce::String (fps, 3) + TRANS (" fps ma il timecode della sessione e' ") + juce::String (p.tcFmt().name) + TRANS (": i timecode non corrisponderanno al video."));
        if (std::abs (s.video->offsetSec) > 1.0e-6)
            add (Issue::Info, TRANS ("Il video e' spostato di ") + juce::String (s.video->offsetSec * 1000.0, 0) + TRANS (" ms rispetto all'audio."));
    }
    bool haveGuide = false;
    for (auto& t : s.tracks) haveGuide = haveGuide || t.live->trackType.load() == TrackGuide;
    if (! s.cues.empty() && ! haveGuide) add (Issue::Info, TRANS ("Non c'e' una traccia Guida: Allinea alla guida non e' disponibile."));

    if (out.empty()) add (Issue::Info, TRANS ("Nessun problema trovato."));
    std::stable_sort (out.begin(), out.end(), [] (const Issue& a, const Issue& b) { return (int) a.level > (int) b.level; });
    return out;
}

//==============================================================================
Panel::Panel (std::vector<Issue> list_, std::function<void (const Issue&)> go, std::function<void()> relinkFn)
    : issues (std::move (list_)), goTo (std::move (go))
{
    int errors = 0, warnings = 0;
    for (auto& i : issues) { errors += i.level == Issue::Error; warnings += i.level == Issue::Warning; }
    summary.setText (errors == 0 && warnings == 0 ? TRANS ("Tutto a posto.") : juce::String (errors) + (errors == 1 ? " errore, " : " errori, ") + juce::String (warnings) + (warnings == 1 ? " avviso" : " avvisi"),
                     juce::dontSendNotification);
    summary.setFont (font (14.0f, true));
    summary.setColour (juce::Label::textColourId, errors > 0 ? accent : (warnings > 0 ? yellow : green));
    addAndMakeVisible (summary);
    list.setRowHeight (30);
    list.setColour (juce::ListBox::backgroundColourId, bg);
    list.setColour (juce::ListBox::outlineColourId, border);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);
    goBtn.onClick = [this] { const int r = list.getSelectedRow(); if (r >= 0 && r < (int) issues.size() && goTo) goTo (issues[(size_t) r]); };
    relinkBtn.onClick = std::move (relinkFn);
    addAndMakeVisible (goBtn);
    addAndMakeVisible (relinkBtn);
    relinkBtn.setVisible (errors > 0);
}

void Panel::resized()
{
    auto r = getLocalBounds().reduced (14);
    summary.setBounds (r.removeFromTop (26));
    r.removeFromTop (6);
    auto bottom = r.removeFromBottom (32);
    r.removeFromBottom (8);
    list.setBounds (r);
    goBtn.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (8);
    relinkBtn.setBounds (bottom.removeFromRight (170));
}

void Panel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) issues.size()) return;
    const auto& i = issues[(size_t) row];
    if (selected) { g.setColour (blue.withAlpha (0.3f)); g.fillRect (0, 0, w, h); }
    const auto col = i.level == Issue::Error ? accent : (i.level == Issue::Warning ? yellow : dim);
    g.setColour (col);
    g.fillEllipse (10.0f, (float) h * 0.5f - 5.0f, 10.0f, 10.0f);
    g.setColour (text);
    g.setFont (font (13.0f));
    g.drawText (i.text, 30, 0, w - 40, h, juce::Justification::centredLeft, true);
}
} // namespace fc::check
