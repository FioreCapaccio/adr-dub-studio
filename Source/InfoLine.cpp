#include "InfoLine.h"

namespace fc
{
using namespace theme;

InfoLinePanel::InfoLinePanel (Project& p) : project (p)
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (10.5f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (lStart, TRANS ("INIZIO (TC)")); label (lEnd, TRANS ("FINE (TC)")); label (lLen, TRANS ("LUNGHEZZA (S)")); label (lOff, TRANS ("OFFSET NEL FILE (S)"));
    label (lGain, TRANS ("GUADAGNO (DB)")); label (lFi, TRANS ("FADE IN (MS)")); label (lFo, TRANS ("FADE OUT (MS)"));
    title.setFont (font (13.0f, true));
    title.setColour (juce::Label::textColourId, text);
    addAndMakeVisible (title);
    int field = 0;
    for (auto* e : { &eStart, &eEnd, &eLen, &eOff, &eGain, &eFi, &eFo })
    {
        e->setColour (juce::TextEditor::backgroundColourId, bg);
        e->setJustification (juce::Justification::centred);
        e->setFont (mono (13.0f));
        const int f = field++;
        e->onReturnKey = [this, f] { commit (f); };
        e->onFocusLost = [this, f] { commit (f); };
        addAndMakeVisible (*e);
    }
    for (auto* b : { &muteTb, &invertTb })
    {
        b->setWantsKeyboardFocus (false);
        b->onClick = [this, b] { if (! loading) commit (b == &muteTb ? 7 : 8); };
        addAndMakeVisible (*b);
    }
    project.addChangeListener (this);
    refresh();
}

InfoLinePanel::~InfoLinePanel() { project.removeChangeListener (this); }

int64_t InfoLinePanel::clipId() const
{
    return project.selection.size() >= 1 ? *project.selection.begin() : 0;
}

void InfoLinePanel::refresh()
{
    loading = true;
    const auto* c = project.findClip (clipId());
    const double rate = project.getSampleRate();
    for (auto* e : { &eStart, &eEnd, &eLen, &eOff, &eGain, &eFi, &eFo }) e->setEnabled (c != nullptr);
    muteTb.setEnabled (c != nullptr);
    invertTb.setEnabled (c != nullptr);
    if (c != nullptr)
    {
        auto setIf = [] (juce::TextEditor& e, const juce::String& v) { if (e.getText() != v && ! e.hasKeyboardFocus (true)) e.setText (v, false); };
        title.setText ((c->asset != nullptr ? c->asset->name : juce::String()) + (project.selection.size() > 1 ? "   (+" + juce::String ((int) project.selection.size() - 1) + TRANS (" clip selezionati)") : juce::String()), juce::dontSendNotification);
        setIf (eStart, project.formatTc ((double) c->start / rate));
        setIf (eEnd, project.formatTc ((double) c->end() / rate));
        setIf (eLen, juce::String ((double) c->length / rate, 3));
        setIf (eOff, juce::String ((double) c->offset / rate, 3));
        setIf (eGain, juce::String (c->gainDb, 1));
        setIf (eFi, juce::String ((double) c->fadeIn * 1000.0 / rate, 1));
        setIf (eFo, juce::String ((double) c->fadeOut * 1000.0 / rate, 1));
        muteTb.setToggleState (c->muted, juce::dontSendNotification);
        invertTb.setToggleState (c->invert, juce::dontSendNotification);
    }
    else
    {
        title.setText (TRANS ("Nessun clip selezionato"), juce::dontSendNotification);
        for (auto* e : { &eStart, &eEnd, &eLen, &eOff, &eGain, &eFi, &eFo }) e->setText ({}, false);
    }
    loading = false;
}

void InfoLinePanel::commit (int field)
{
    if (loading) return;
    const auto* c = project.findClip (clipId());
    if (c == nullptr) return;
    const double rate = project.getSampleRate();
    auto num = [] (const juce::TextEditor& e) { return e.getText().replaceCharacter (',', '.').getDoubleValue(); };
    Project::ClipEdit ed;
    switch (field)
    {
        case 0: { double s = 0.0; if (! project.parseTc (eStart.getText(), s)) { refresh(); return; } ed.start = (int64_t) std::llround (s * rate); break; }
        case 1: { double s = 0.0; if (! project.parseTc (eEnd.getText(), s)) { refresh(); return; } ed.length = (int64_t) std::llround (s * rate) - c->start; break; }
        case 2: ed.length = (int64_t) std::llround (num (eLen) * rate); break;
        case 3: ed.offset = (int64_t) std::llround (num (eOff) * rate); break;
        case 4: ed.gainDb = (float) num (eGain); break;
        case 5: ed.fadeIn = (int64_t) std::llround (num (eFi) * rate / 1000.0); break;
        case 6: ed.fadeOut = (int64_t) std::llround (num (eFo) * rate / 1000.0); break;
        case 7: ed.muted = muteTb.getToggleState(); break;
        case 8: ed.invert = invertTb.getToggleState(); break;
        default: return;
    }
    // niente passo di undo se il valore non cambia
    if (! project.setClipInfo (c->id, ed)) refresh();
}

void InfoLinePanel::resized()
{
    auto r = getLocalBounds().reduced (12, 8);
    title.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);
    auto row = r.removeFromTop (50);
    const int w = juce::jmax (80, (row.getWidth() - 6 * 8) / 7);
    auto cell = [&] (juce::Label& l, juce::TextEditor& e)
    {
        auto col = row.removeFromLeft (w);
        l.setBounds (col.removeFromTop (16));
        e.setBounds (col.reduced (0, 2));
        row.removeFromLeft (8);
    };
    cell (lStart, eStart); cell (lEnd, eEnd); cell (lLen, eLen); cell (lOff, eOff); cell (lGain, eGain); cell (lFi, eFi); cell (lFo, eFo);
    r.removeFromTop (6);
    auto tog = r.removeFromTop (24);
    muteTb.setBounds (tog.removeFromLeft (100));
    invertTb.setBounds (tog.removeFromLeft (180));
}
} // namespace fc
