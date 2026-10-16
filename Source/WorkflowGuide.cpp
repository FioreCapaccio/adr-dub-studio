#include "WorkflowGuide.h"
#include "TrackPresets.h"

namespace fc::workflow
{
using namespace theme;

std::vector<Step> steps (const Project& p, bool checkedClean, bool delivered)
{
    const auto& s = p.getSession();
    std::vector<Step> v;
    const int cues = (int) s.cues.size();
    int recorded = 0, approved = 0;
    for (auto& c : s.cues) { if (c.status == 1 || c.status == 2) ++recorded; if (c.status == 2) ++approved; }

    int roleTracks = 0;
    for (auto& t : s.tracks) if (t.live->trackType.load() == TrackDialogue && ! t.name.startsWithIgnoreCase ("Audio")) ++roleTracks;

    v.push_back ({ "1. Video", s.video != nullptr ? s.video->file.getFileName() : juce::String ("nessun video importato"), s.video != nullptr, ActImportVideo, "Importa video..." });
    v.push_back ({ TRANS ("2. Copione"), cues > 0 ? juce::String (cues) + TRANS (" cue importati") : juce::String (TRANS ("nessun cue")), cues > 0, ActImportScript, TRANS ("Importa cue...") });
    v.push_back ({ TRANS ("3. Tracce"), roleTracks > 0 ? juce::String (roleTracks) + TRANS (" tracce Dialoghi") : juce::String (TRANS ("ancora nessuna traccia per i personaggi")), roleTracks > 0, ActTracks, TRANS ("Tracce per personaggio") });
    v.push_back ({ "4. Registrazione", cues > 0 ? juce::String (recorded) + " di " + juce::String (cues) + " cue registrati" : juce::String ("serve il copione"), cues > 0 && recorded == cues, ActRecord, "Finestra ADR" });
    v.push_back ({ "5. Approvazione", cues > 0 ? juce::String (approved) + " di " + juce::String (cues) + " cue approvati" : juce::String ("serve il copione"), cues > 0 && approved == cues, ActApprove, "Finestra ADR" });
    v.push_back ({ TRANS ("6. Controllo"), checkedClean ? juce::String (TRANS ("la sessione e' a posto")) : juce::String (TRANS ("controlla la sessione prima di consegnare")), checkedClean, ActCheck, "Controlla..." });
    v.push_back ({ TRANS ("7. Consegna"), delivered ? juce::String (TRANS ("consegna esportata in questa sessione")) : juce::String (TRANS ("MIX, stem e rapporto")), delivered, ActDeliver, "Esporta..." });
    return v;
}

Panel::Panel (std::vector<Step> s, std::function<void (int)> act) : items (std::move (s)), action (std::move (act))
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto* b = buttons.add (new juce::TextButton (items[i].button));
        const int a = items[i].action;
        b->onClick = [this, a] { if (action) action (a); };
        addAndMakeVisible (b);
    }
}

void Panel::paint (juce::Graphics& g)
{
    g.fillAll (panel);
    const int rowH = 52;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const int y = 14 + (int) i * rowH;
        const auto& it = items[i];
        g.setColour (it.done ? green : border);
        g.fillEllipse (18.0f, (float) y + 12.0f, 22.0f, 22.0f);
        if (it.done)
        {
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            juce::Path tick;
            tick.startNewSubPath (23.0f, (float) y + 23.0f); tick.lineTo (28.0f, (float) y + 28.0f); tick.lineTo (36.0f, (float) y + 17.0f);
            g.strokePath (tick, juce::PathStrokeType (2.4f));
        }
        g.setColour (text);
        g.setFont (font (15.0f, true));
        g.drawText (it.title, 52, y + 4, getWidth() - 220, 22, juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (font (12.5f));
        g.drawText (it.detail, 52, y + 25, getWidth() - 220, 18, juce::Justification::centredLeft, true);
    }
}

void Panel::resized()
{
    const int rowH = 52;
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setBounds (getWidth() - 160, 14 + i * rowH + 9, 142, 28);
}
} // namespace fc::workflow
