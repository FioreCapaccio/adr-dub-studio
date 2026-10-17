#include "BoothTools.h"
#include "Settings.h"

namespace fc
{
using namespace theme;

//==============================================================================
namespace script
{
std::vector<AdrCue> sorted (const std::vector<AdrCue>& cues)
{
    auto v = cues;
    std::stable_sort (v.begin(), v.end(), [] (const AdrCue& a, const AdrCue& b) { return a.inPos < b.inPos; });
    return v;
}

State stateAt (const std::vector<AdrCue>& cues, int64_t pos, double rate, double leadSec)
{
    State s;
    for (int i = 0; i < (int) cues.size(); ++i)
    {
        const auto& c = cues[(size_t) i];
        if (c.outPos <= pos) continue;   // gia' finito
        s.valid = true;
        s.current = i;
        s.secondsToStart = (double) (c.inPos - pos) / rate;
        s.secondsToEnd = (double) (c.outPos - pos) / rate;
        s.active = s.secondsToStart <= leadSec;
        const double len = juce::jmax (1.0, (double) (c.outPos - c.inPos));
        s.progress = juce::jlimit (0.0, 1.0, (double) (pos - c.inPos) / len);
        return s;
    }
    if (! cues.empty()) { s.valid = true; s.current = (int) cues.size() - 1; s.progress = 1.0; }   // dopo l'ultimo: resta l'ultimo, concluso
    return s;
}
} // namespace script

//==============================================================================
ScriptComponent::ScriptComponent (Project& p, AudioEngine& e) : project (p), engine (e)
{
    fontSize = juce::jlimit (20, 200, settings().getIntValue ("script.size", 56));
    for (auto* b : { &smaller, &bigger }) addAndMakeVisible (*b);
    smaller.setTooltip (TRANS ("Testo piu' piccolo (o rotellina del mouse con Cmd/Ctrl)"));
    bigger.setTooltip (TRANS ("Testo piu' grande"));
    smaller.onClick = [this] { setFontSize (fontSize - 6); };
    bigger.onClick = [this] { setFontSize (fontSize + 6); };
    startTimerHz (25);
}

void ScriptComponent::setFontSize (int px)
{
    fontSize = juce::jlimit (20, 200, px);
    settings().setValue ("script.size", fontSize);
    settings().saveIfNeeded();
    repaint();
}

void ScriptComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) setFontSize (fontSize + (w.deltaY > 0 ? 4 : -4));
}

void ScriptComponent::resized()
{
    auto r = getLocalBounds().reduced (12, 8);
    bigger.setBounds (r.removeFromRight (44).removeFromTop (28));
    r.removeFromRight (6);
    smaller.setBounds (r.removeFromRight (44).removeFromTop (28));
}

void ScriptComponent::timerCallback()
{
    const auto cues = script::sorted (project.getSession().cues);
    const auto st = script::stateAt (cues, engine.getPosition(), project.getSampleRate(), 3.0);
    const int64_t pos = engine.getPosition();
    if (pos != lastPos || st.current != lastCue || st.active != lastActive)
    {
        lastPos = pos; lastCue = st.current; lastActive = st.active;
        repaint();
    }
}

void ScriptComponent::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    const double rate = project.getSampleRate();
    const auto cues = script::sorted (project.getSession().cues);
    const int64_t pos = engine.getPosition();
    const auto st = script::stateAt (cues, pos, rate, 3.0);

    auto area = getLocalBounds().reduced (22, 14);
    auto top = area.removeFromTop (34);
    g.setColour (dim);
    g.setFont (mono (20.0f));
    g.drawText (project.formatTc ((double) pos / rate), top.removeFromLeft (200), juce::Justification::centredLeft);
    g.setFont (font (14.0f, true));
    g.drawText (st.valid ? TRANS ("CUE ") + juce::String (st.current + 1) + " / " + juce::String ((int) cues.size()) : juce::String (TRANS ("NESSUN CUE")), top.withTrimmedRight (110),
                juce::Justification::centredRight);

    if (! st.valid)
    {
        g.setColour (dim);
        g.setFont (font (24.0f));
        g.drawFittedText (TRANS ("Importa un copione (ADR > Importa cue...) o aggiungi dei cue per vederli qui."), getLocalBounds().reduced (60), juce::Justification::centred, 3);
        return;
    }

    const auto& c = cues[(size_t) st.current];

    // barra di avanzamento: prima dell'ingresso scende il conto alla rovescia (3 s), poi sale l'avanzamento del cue
    auto bar = area.removeFromBottom (14).toFloat();
    g.setColour (panelHi);
    g.fillRoundedRectangle (bar, 7.0f);
    if (st.secondsToStart > 0.0)
    {
        const double lead = juce::jlimit (0.0, 3.0, st.secondsToStart) / 3.0;
        if (st.active) { g.setColour (yellow); g.fillRoundedRectangle (bar.withWidth ((float) (bar.getWidth() * lead)), 7.0f); }
    }
    else
    {
        g.setColour (st.progress >= 1.0 ? green : blue);
        g.fillRoundedRectangle (bar.withWidth ((float) (bar.getWidth() * st.progress)), 7.0f);
    }
    area.removeFromBottom (10);

    // le prossime due battute
    auto next = area.removeFromBottom (juce::jmin (area.getHeight() / 3, 120));
    g.setColour (border);
    g.drawHorizontalLine (next.getY(), (float) next.getX(), (float) next.getRight());
    g.setFont (font ((float) juce::jmax (14, fontSize / 3)));
    int y = next.getY() + 8;
    for (int k = 1; k <= 2 && st.current + k < (int) cues.size(); ++k)
    {
        const auto& n = cues[(size_t) (st.current + k)];
        g.setColour (dim);
        g.drawText (juce::String ("#") + juce::String (n.number) + "  " + n.character + ":  " + n.dialogue, next.getX(), y, next.getWidth(), fontSize / 3 + 8, juce::Justification::centredLeft, true);
        y += fontSize / 3 + 10;
    }

    // personaggio e scena
    const int colourIndex = juce::jmax (0, (int) (std::hash<std::string>() (c.character.toStdString()) % (size_t) numPaletteColours));
    auto head = area.removeFromTop (juce::jmax (40, fontSize));
    g.setColour (trackPalette[colourIndex]);
    g.setFont (font ((float) fontSize * 0.8f, true));
    g.drawText (c.character.isEmpty() ? juce::String (TRANS ("(senza personaggio)")) : c.character.toUpperCase(), head.removeFromLeft (head.getWidth() * 2 / 3), juce::Justification::centredLeft, true);
    g.setColour (dim);
    g.setFont (font (juce::jmax (16.0f, (float) fontSize / 2.5f)));
    g.drawText ((c.scene.isNotEmpty() ? TRANS ("Scena ") + c.scene : juce::String()) + (c.reel.isNotEmpty() ? TRANS ("   Bobina ") + c.reel : juce::String()) + "   #" + juce::String (c.number),
                head, juce::Justification::centredRight, true);

    // stato (conto alla rovescia / in corso)
    auto status = area.removeFromTop (juce::jmax (26, fontSize / 2));
    if (st.secondsToStart > 0.05)
    {
        g.setColour (st.active ? yellow : dim);
        g.setFont (font ((float) fontSize * 0.45f, true));
        g.drawText (st.active ? TRANS ("PRONTO... ") + juce::String (st.secondsToStart, 1) + " s" : TRANS ("tra ") + juce::String (st.secondsToStart, 1) + " s", status, juce::Justification::centredLeft);
    }
    else if (st.progress < 1.0)
    {
        g.setColour (green);
        g.setFont (font ((float) fontSize * 0.45f, true));
        g.drawText (TRANS ("ORA"), status, juce::Justification::centredLeft);
    }
    area.removeFromTop (8);

    // la battuta
    juce::AttributedString text;
    text.setJustification (juce::Justification::centredLeft);
    text.append (c.dialogue.isEmpty() ? juce::String ("...") : c.dialogue, font ((float) fontSize, true), st.progress >= 1.0 ? dim : juce::Colour (theme::text));
    text.draw (g, area.toFloat());
}

//==============================================================================
CueMixPanel::CueMixPanel (AudioEngine& e) : engine (e)
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (outLbl, TRANS ("USCITE CUFFIE")); label (mixLbl, TRANS ("MIX (dB)")); label (beepLbl, TRANS ("BIP (dB)")); label (talkLbl, TRANS ("TALKBACK (dB)")); label (inLbl, TRANS ("MICROFONO DEL DIRETTORE"));
    for (auto* s : { &mix, &beep, &talk })
    {
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
        addAndMakeVisible (*s);
    }
    mix.setRange (-40.0, 6.0, 0.5); beep.setRange (-40.0, 6.0, 0.5); talk.setRange (-30.0, 12.0, 0.5);
    for (auto* c : { (juce::Component*) &enable, (juce::Component*) &duckMix, (juce::Component*) &outputs, (juce::Component*) &talkInput, (juce::Component*) &talkButton, (juce::Component*) &status })
        addAndMakeVisible (*c);

    status.setFont (font (12.0f));
    status.setColour (juce::Label::textColourId, dim);
    status.setJustificationType (juce::Justification::topLeft);

    auto& s = settings();
    loading = true;
    enable.setToggleState (false, juce::dontSendNotification);   // si accende sempre a mano: la seconda uscita puo' non esistere
    duckMix.setToggleState (s.getBoolValue ("cue.duck", true), juce::dontSendNotification);
    mix.setValue (s.getDoubleValue ("cue.mixDb", 0.0), juce::dontSendNotification);
    beep.setValue (s.getDoubleValue ("cue.beepDb", 0.0), juce::dontSendNotification);
    talk.setValue (s.getDoubleValue ("cue.talkDb", 0.0), juce::dontSendNotification);
    refreshOutputs();
    const int pairWanted = s.getIntValue ("cue.pair", 1), inWanted = s.getIntValue ("cue.talkInput", 0);
    outputs.setSelectedId (pairWanted + 1, juce::dontSendNotification);
    if (talkInput.getNumItems() > 0) talkInput.setSelectedId (juce::jlimit (0, talkInput.getNumItems() - 1, inWanted) + 1, juce::dontSendNotification);
    loading = false;

    auto changed = [this] { if (! loading) apply(); };
    enable.onClick = changed; duckMix.onClick = changed;
    outputs.onChange = changed; talkInput.onChange = changed;
    mix.onValueChange = changed; beep.onValueChange = changed; talk.onValueChange = changed;
    talkButton.onStateChange = [this] { engine.cue.talkOn = talkButton.isDown() && enable.getToggleState(); };
    talkButton.setTooltip (TRANS ("Tieni premuto per parlare nelle cuffie dell'attore"));
    startTimerHz (20);
}

void CueMixPanel::refreshOutputs()
{
    outputs.clear (juce::dontSendNotification);
    talkInput.clear (juce::dontSendNotification);
    int avail = 2;
    if (auto* dev = engine.deviceManager.getCurrentAudioDevice()) avail = dev->getOutputChannelNames().size();
    for (int pair = 1; 2 * (pair + 1) <= avail; ++pair)
        outputs.addItem (TRANS ("Uscite ") + juce::String (2 * pair + 1) + " - " + juce::String (2 * pair + 2), pair + 1);
    int inputs = 0;
    if (auto* dev = engine.deviceManager.getCurrentAudioDevice()) inputs = dev->getInputChannelNames().size();
    for (int i = 0; i < juce::jmax (1, inputs); ++i) talkInput.addItem (TRANS ("Ingresso ") + juce::String (i + 1), i + 1);
    const bool ok = outputs.getNumItems() > 0;
    enable.setEnabled (ok);
    outputs.setEnabled (ok);
    status.setText (ok ? TRANS ("Le cuffie ricevono il mix (con il suo livello), i bip e il microfono del direttore. La prima coppia di uscite resta il master.")
                       : TRANS ("La scheda audio attuale ha solo ") + juce::String (avail) + TRANS (" uscite: per le cuffie dell'attore serve un'interfaccia con almeno 4 uscite (si sceglie in File > Impostazioni audio)."),
                    juce::dontSendNotification);
    if (ok && outputs.getSelectedId() == 0) outputs.setSelectedId (outputs.getItemId (0), juce::dontSendNotification);
}

void CueMixPanel::save() const
{
    auto& s = settings();
    s.setValue ("cue.duck", duckMix.getToggleState());
    s.setValue ("cue.mixDb", mix.getValue());
    s.setValue ("cue.beepDb", beep.getValue());
    s.setValue ("cue.talkDb", talk.getValue());
    s.setValue ("cue.pair", outputs.getSelectedId() - 1);
    s.setValue ("cue.talkInput", talkInput.getSelectedId() - 1);
    s.saveIfNeeded();
}

void CueMixPanel::apply()
{
    const int pair = juce::jmax (1, outputs.getSelectedId() - 1);
    if (enable.getToggleState())
    {
        auto err = engine.openCueOutputs (pair);
        if (err.isNotEmpty())
        {
            enable.setToggleState (false, juce::dontSendNotification);
            status.setText (err, juce::dontSendNotification);
            engine.cue.enabled = false;
            return;
        }
        engine.ensureInput();   // il microfono del direttore e' un ingresso: serve aperto
        refreshOutputs();
        outputs.setSelectedId (pair + 1, juce::dontSendNotification);
    }
    engine.cue.pair = pair;
    engine.cue.mixDb = (float) mix.getValue();
    engine.cue.beepDb = (float) beep.getValue();
    engine.cue.talkDb = (float) talk.getValue();
    engine.cue.mixDuckDb = duckMix.getToggleState() ? -12.0f : 0.0f;
    engine.cue.talkInput = juce::jmax (0, talkInput.getSelectedId() - 1);
    engine.cue.enabled = enable.getToggleState();
    if (! enable.getToggleState()) engine.cue.talkOn = false;
    save();
}

void CueMixPanel::timerCallback()
{
    const float l = engine.cue.peakL.exchange (0.0f), r = engine.cue.peakR.exchange (0.0f);
    shownL = juce::jmax (l, shownL - 0.05f);
    shownR = juce::jmax (r, shownR - 0.05f);
    repaint (getLocalBounds().removeFromBottom (40));
}

void CueMixPanel::paint (juce::Graphics& g)
{
    g.fillAll (panel);
    auto m = getLocalBounds().reduced (18).removeFromBottom (26);
    drawCaption (g, TRANS ("LIVELLO CUFFIE"), m.removeFromLeft (110));
    for (int c = 0; c < 2; ++c)
    {
        auto bar = m.removeFromTop (10).toFloat();
        m.removeFromTop (4);
        g.setColour (well);
        g.fillRoundedRectangle (bar, 3.0f);
        const float v = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (c == 0 ? shownL : shownR, -60.0f) + 60.0f) / 60.0f);
        g.setColour (v > 0.97f ? accent : (v > 0.85f ? yellow : green));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * v), 3.0f);
    }
}

void CueMixPanel::resized()
{
    auto r = getLocalBounds().reduced (18);
    r.removeFromBottom (36);   // misuratori
    auto row = [&r] (juce::Label& l, juce::Component& c, int h = 28)
    {
        auto line = r.removeFromTop (h);
        l.setBounds (line.removeFromLeft (190));
        c.setBounds (line);
        r.removeFromTop (8);
    };
    enable.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    row (outLbl, outputs);
    row (mixLbl, mix);
    row (beepLbl, beep);
    duckMix.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);
    row (inLbl, talkInput);
    row (talkLbl, talk);
    talkButton.setBounds (r.removeFromTop (44));
    r.removeFromTop (10);
    status.setBounds (r);
}
} // namespace fc
