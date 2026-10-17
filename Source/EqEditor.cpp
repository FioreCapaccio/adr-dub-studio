#include "EqEditor.h"

namespace fc
{
using namespace theme;

juce::Colour EqEditor::bandColour (int i)
{
    static const juce::Colour c[] = { juce::Colour (0xffff6b6b), juce::Colour (0xffffc94d), juce::Colour (0xff4cd27a), juce::Colour (0xff4aa8ff) };
    return c[juce::jlimit (0, 3, i)];
}

EqEditor::EqEditor (Project& p, std::shared_ptr<TrackLive> l, std::function<double()> sr) : project (p), live (std::move (l)), rate (std::move (sr))
{
    enableBtn.onClick = [this] { if (! loading) { live->eq.enabled = enableBtn.getToggleState(); changed(); } };
    addAndMakeVisible (enableBtn);
    resetBtn.setTooltip (TRANS ("Riporta tutte le bande a 0 dB"));
    resetBtn.onClick = [this]
    {
        ParamEq fresh;
        const bool en = live->eq.enabled.load();
        live->eq.copyFrom (fresh);
        live->eq.enabled = en;
        refresh();
        changed();
    };
    addAndMakeVisible (resetBtn);

    typeLbl.setText (TRANS ("TIPO DI TRACCIA"), juce::dontSendNotification);
    typeLbl.setFont (font (10.0f, true));
    typeLbl.setColour (juce::Label::textColourId, dim);
    addAndMakeVisible (typeLbl);
    for (int t = 0; t < kNumTrackTypes; ++t) typeBox.addItem (trackTypeName (t), t + 1);
    typeBox.setTooltip (TRANS ("Applica una catena consigliata: EQ, compressore, limiter e livello. Si puo' ritoccare tutto dopo"));
    typeBox.onChange = [this]
    {
        if (loading) return;
        applyTrackPreset (*live, typeBox.getSelectedId() - 1);
        project.liveChanged();
        refresh();
    };
    addAndMakeVisible (typeBox);
    typeInfo.setFont (font (10.5f));
    typeInfo.setColour (juce::Label::textColourId, dim);
    typeInfo.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (typeInfo);

    compOn.onClick = [this] { if (! loading) { live->comp.enabled = compOn.getToggleState(); project.liveChanged(); repaint(); } };
    addAndMakeVisible (compOn);
    struct CS { juce::Slider* s; juce::Label* l; const char* name; double lo, hi, step, mid; const char* suffix; std::atomic<float> TrackComp::* p; };
    const CS comps[] = {
        { &compThr, &compThrLbl, TRX ("SOGLIA"), -60.0, 0.0, 0.5, -18.0, " dB", &TrackComp::thresholdDb },
        { &compRatio, &compRatioLbl, TRX ("RAPPORTO"), 1.0, 20.0, 0.1, 4.0, " :1", &TrackComp::ratio },
        { &compAtk, &compAtkLbl, TRX ("ATTACCO"), 0.1, 200.0, 0.1, 20.0, " ms", &TrackComp::attackMs },
        { &compRel, &compRelLbl, TRX ("RILASCIO"), 5.0, 2000.0, 1.0, 200.0, " ms", &TrackComp::releaseMs },
        { &compMakeup, &compMakeupLbl, TRX ("GUADAGNO"), -12.0, 24.0, 0.1, 0.0, " dB", &TrackComp::makeupDb } };
    for (auto& c : comps)
    {
        c.s->setRange (c.lo, c.hi, c.step);
        if (c.mid > c.lo && c.mid < c.hi) c.s->setSkewFactorFromMidPoint (c.mid);
        c.s->setTextValueSuffix (c.suffix);
        c.s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
        c.s->setColour (juce::Slider::thumbColourId, hilite);
        c.s->onValueChange = [this, c] { if (! loading) { (live->comp.*(c.p)).store ((float) c.s->getValue()); project.liveChanged(); repaint(); } };
        c.l->setText (juce::translate (c.name), juce::dontSendNotification);
        c.l->setFont (font (10.0f, true));
        c.l->setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (*c.s);
        addAndMakeVisible (*c.l);
    }

    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        auto& u = ui[i];
        for (int t = 0; t < ParamEq::kNumTypes; ++t) u.type.addItem (ParamEq::typeName (t), t + 1);
        u.type.onChange = [this, i] { if (! loading) { live->eq.band[i].type = ui[i].type.getSelectedId() - 1; changed(); } };
        u.on.setButtonText (TRANS ("Banda ") + juce::String (i + 1));
        u.on.setColour (juce::ToggleButton::textColourId, bandColour (i));
        u.on.setColour (juce::ToggleButton::tickColourId, bandColour (i));
        u.on.onClick = [this, i] { if (! loading) { live->eq.band[i].on = ui[i].on.getToggleState(); changed(); } };

        u.freq.setRange (20.0, 20000.0, 1.0);
        u.freq.setSkewFactorFromMidPoint (1000.0);
        u.freq.setTextValueSuffix (" Hz");
        u.freq.setDoubleClickReturnValue (true, i == 0 ? 100.0 : i == 1 ? 500.0 : i == 2 ? 2500.0 : 8000.0);
        u.gain.setRange (-kRange, kRange, 0.1);
        u.gain.setTextValueSuffix (" dB");
        u.gain.setDoubleClickReturnValue (true, 0.0);
        u.q.setRange (0.1, 10.0, 0.01);
        u.q.setSkewFactorFromMidPoint (1.0);
        u.q.setDoubleClickReturnValue (true, i == 0 || i == 3 ? 0.7 : 1.0);
        for (auto* s : { &u.freq, &u.gain, &u.q })
        {
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
            s->setColour (juce::Slider::thumbColourId, bandColour (i));
            s->setColour (juce::Slider::trackColourId, bandColour (i).withAlpha (0.5f));
        }
        u.freq.onValueChange = [this, i] { if (! loading) { live->eq.band[i].freq = (float) ui[i].freq.getValue(); changed(); } };
        u.gain.onValueChange = [this, i] { if (! loading) { live->eq.band[i].gainDb = (float) ui[i].gain.getValue(); changed(); } };
        u.q.onValueChange = [this, i] { if (! loading) { live->eq.band[i].q = (float) ui[i].q.getValue(); changed(); } };
        for (auto* l : { &u.freqLbl, &u.gainLbl, &u.qLbl })
        {
            l->setFont (font (10.0f, true));
            l->setColour (juce::Label::textColourId, dim);
        }
        u.freqLbl.setText (TRANS ("FREQ"), juce::dontSendNotification);
        u.gainLbl.setText (TRANS ("GUADAGNO"), juce::dontSendNotification);
        u.qLbl.setText (TRANS ("Q (LARGHEZZA)"), juce::dontSendNotification);
        for (auto* c : std::initializer_list<juce::Component*> { &u.type, &u.on, &u.freq, &u.gain, &u.q, &u.freqLbl, &u.gainLbl, &u.qLbl }) addAndMakeVisible (*c);
    }
    refresh();
    setSize (840, 650);
}

void EqEditor::refresh()
{
    loading = true;
    enableBtn.setToggleState (live->eq.enabled.load(), juce::dontSendNotification);
    typeBox.setSelectedId (live->trackType.load() + 1, juce::dontSendNotification);
    typeInfo.setText (trackPresetDescription (live->trackType.load()), juce::dontSendNotification);
    compOn.setToggleState (live->comp.enabled.load(), juce::dontSendNotification);
    compThr.setValue (live->comp.thresholdDb.load(), juce::dontSendNotification);
    compRatio.setValue (live->comp.ratio.load(), juce::dontSendNotification);
    compAtk.setValue (live->comp.attackMs.load(), juce::dontSendNotification);
    compRel.setValue (live->comp.releaseMs.load(), juce::dontSendNotification);
    compMakeup.setValue (live->comp.makeupDb.load(), juce::dontSendNotification);
    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        auto& b = live->eq.band[i];
        auto& u = ui[i];
        u.type.setSelectedId (b.type.load() + 1, juce::dontSendNotification);
        u.on.setToggleState (b.on.load(), juce::dontSendNotification);
        u.freq.setValue (b.freq.load(), juce::dontSendNotification);
        u.gain.setValue (b.gainDb.load(), juce::dontSendNotification);
        u.q.setValue (b.q.load(), juce::dontSendNotification);
        const int t = b.type.load();
        u.gain.setEnabled (t != ParamEq::HighPass && t != ParamEq::LowPass);
    }
    loading = false;
    repaint();
}

void EqEditor::changed()
{
    // modificare un parametro con l'EQ spento lo accende: e' quello che ci si aspetta quando si muove un nodo
    if (! live->eq.enabled.load())
    {
        live->eq.enabled = true;
        loading = true;
        enableBtn.setToggleState (true, juce::dontSendNotification);
        loading = false;
    }
    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        const int t = live->eq.band[i].type.load();
        ui[i].gain.setEnabled (t != ParamEq::HighPass && t != ParamEq::LowPass);
    }
    project.liveChanged();
    repaint();
}

//==============================================================================
juce::Rectangle<float> EqEditor::plot() const
{
    return juce::Rectangle<float> (46.0f, 96.0f, (float) getWidth() - 46.0f - 20.0f, 230.0f);
}

float EqEditor::freqToX (float hz) const
{
    const auto p = plot();
    return p.getX() + p.getWidth() * std::log (juce::jlimit (20.0f, 20000.0f, hz) / 20.0f) / std::log (1000.0f);
}

float EqEditor::xToFreq (float x) const
{
    const auto p = plot();
    return 20.0f * std::exp (juce::jlimit (0.0f, 1.0f, (x - p.getX()) / p.getWidth()) * std::log (1000.0f));
}

float EqEditor::dbToY (float db) const { const auto p = plot(); return p.getCentreY() - juce::jlimit (-kRange, kRange, db) / kRange * p.getHeight() * 0.5f; }
float EqEditor::yToDb (float y) const { const auto p = plot(); return juce::jlimit (-kRange, kRange, (p.getCentreY() - y) / (p.getHeight() * 0.5f) * kRange); }

void EqEditor::resized()
{
    auto top = getLocalBounds().removeFromTop (46).reduced (16, 9);
    resetBtn.setBounds (top.removeFromRight (90));
    top.removeFromRight (8);
    enableBtn.setBounds (top.removeFromRight (120));
    top.removeFromLeft (250);
    typeLbl.setBounds (top.removeFromLeft (110).withTrimmedTop (4));
    typeBox.setBounds (top.removeFromLeft (170));
    typeInfo.setBounds (16, 46, getWidth() - 32, 38);

    const int y0 = (int) plot().getBottom() + 28;
    const int w = (getWidth() - 32) / ParamEq::kBands;
    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        auto& u = ui[i];
        auto col = juce::Rectangle<int> (16 + i * w, y0, w - 8, 150);
        u.box = col;
        auto r = col.reduced (8, 6);
        u.on.setBounds (r.removeFromTop (22));
        u.type.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        for (auto pr : { std::make_pair (&u.freqLbl, &u.freq), std::make_pair (&u.gainLbl, &u.gain), std::make_pair (&u.qLbl, &u.q) })
        {
            pr.first->setBounds (r.removeFromTop (12));
            pr.second->setBounds (r.removeFromTop (20));
        }
    }

    // compressore: una riga con i cinque controlli
    compBox = juce::Rectangle<int> (16, y0 + 160, getWidth() - 32, 90);
    auto cr = compBox.reduced (10, 6);
    compOn.setBounds (cr.removeFromTop (22).removeFromLeft (170));
    const int cw = cr.getWidth() / 5;
    juce::Slider* sl[] = { &compThr, &compRatio, &compAtk, &compRel, &compMakeup };
    juce::Label* lb[] = { &compThrLbl, &compRatioLbl, &compAtkLbl, &compRelLbl, &compMakeupLbl };
    for (int i = 0; i < 5; ++i)
    {
        auto c = cr.removeFromLeft (cw).reduced (4, 0);
        lb[i]->setBounds (c.removeFromTop (14));
        sl[i]->setBounds (c.removeFromTop (22));
    }
}

int EqEditor::bandAt (juce::Point<int> pt) const
{
    int best = -1;
    float bestD = 16.0f;
    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        const auto& b = live->eq.band[i];
        const int t = b.type.load();
        const float y = (t == ParamEq::HighPass || t == ParamEq::LowPass) ? dbToY (0.0f) : dbToY (b.gainDb.load());
        const float d = std::hypot ((float) pt.x - freqToX (b.freq.load()), (float) pt.y - y);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void EqEditor::mouseDown (const juce::MouseEvent& e) { dragBand = bandAt (e.getPosition()); if (dragBand >= 0) mouseDrag (e); }

void EqEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0) return;
    auto& b = live->eq.band[dragBand];
    b.freq = juce::jlimit (20.0f, 20000.0f, xToFreq ((float) e.x));
    const int t = b.type.load();
    if (t != ParamEq::HighPass && t != ParamEq::LowPass) b.gainDb = std::round (yToDb ((float) e.y) * 10.0f) / 10.0f;
    b.on = true;
    refresh();
    changed();
}

void EqEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = bandAt (e.getPosition());
    if (i < 0) return;
    live->eq.band[i].gainDb = 0.0f;   // doppio clic su un nodo: guadagno a zero
    refresh();
    changed();
}

void EqEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int i = bandAt (e.getPosition());
    if (i < 0) return;
    auto& b = live->eq.band[i];
    b.q = juce::jlimit (0.1f, 10.0f, b.q.load() * std::exp (w.deltaY * (w.isReversed ? -1.0f : 1.0f) * 1.5f));
    refresh();
    changed();
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);
    g.setColour (text);
    g.setFont (font (13.0f, true));
    g.drawText (TRANS ("EQ E COMPRESSORE"), 16, 0, 230, 46, juce::Justification::centredLeft);

    const auto p = plot();
    g.setColour (well);
    g.fillRoundedRectangle (p, 6.0f);

    // griglia
    g.setFont (mono (9.5f));
    for (float hz : { 30.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
    {
        const bool major = hz == 100.f || hz == 1000.f || hz == 10000.f;
        g.setColour (grid.withAlpha (major ? 0.95f : 0.55f));
        g.drawVerticalLine ((int) freqToX (hz), p.getY(), p.getBottom());
        if (major || hz == 50.f || hz == 500.f || hz == 5000.f)
        {
            g.setColour (dim);
            g.drawText (hz >= 1000.f ? juce::String ((int) (hz / 1000.f)) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (freqToX (hz) - 18.0f, p.getBottom() + 2.0f, 36.0f, 12.0f).toNearestInt(), juce::Justification::centred);
        }
    }
    for (int db = -18; db <= 18; db += 6)
    {
        const float y = dbToY ((float) db);
        g.setColour (db == 0 ? grid.brighter (0.5f) : grid.withAlpha (0.8f));
        g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
        g.setColour (dim);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<int> (8, (int) y - 6, 34, 12), juce::Justification::centredRight);
    }

    // curva totale
    const double sr = rate();
    const bool en = live->eq.enabled.load();
    juce::Path curve, area;
    const int steps = (int) p.getWidth();
    for (int i = 0; i <= steps; ++i)
    {
        const float x = p.getX() + (float) i;
        const float y = dbToY ((float) live->eq.responseDb ((double) xToFreq (x), sr));
        if (i == 0) { curve.startNewSubPath (x, y); area.startNewSubPath (x, dbToY (0.0f)); area.lineTo (x, y); }
        else { curve.lineTo (x, y); area.lineTo (x, y); }
    }
    area.lineTo (p.getRight(), dbToY (0.0f));
    area.closeSubPath();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (p.toNearestInt());
        g.setColour ((en ? blue : dim).withAlpha (0.22f));
        g.fillPath (area);
        g.setColour (en ? hilite : dim);
        g.strokePath (curve, juce::PathStrokeType (2.0f));
    }

    // nodi
    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        const auto& b = live->eq.band[i];
        const int t = b.type.load();
        const float x = freqToX (b.freq.load()), y = (t == ParamEq::HighPass || t == ParamEq::LowPass) ? dbToY (0.0f) : dbToY (b.gainDb.load());
        const auto c = bandColour (i).withAlpha (b.on.load() ? 1.0f : 0.35f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillEllipse (x - 9.0f, y - 8.0f, 18.0f, 18.0f);
        g.setColour (c);
        g.fillEllipse (x - 8.0f, y - 9.0f, 16.0f, 16.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (font (10.0f, true));
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (x - 8.0f, y - 9.0f, 16.0f, 16.0f).toNearestInt(), juce::Justification::centred);
    }
    g.setColour (dim);
    g.setFont (font (10.5f));
    g.drawText (TRANS ("Trascina i nodi: frequenza e guadagno. Rotella sul nodo: larghezza (Q). Doppio clic: guadagno a zero."),
                juce::Rectangle<int> (46, (int) plot().getBottom() + 14, getWidth() - 66, 14), juce::Justification::centredLeft);

    // riquadro del compressore
    {
        auto f = compBox.toFloat();
        g.setColour (panelTop);
        g.fillRoundedRectangle (f, 8.0f);
        g.setColour ((live->comp.enabled.load() ? hilite : dim).withAlpha (0.8f));
        g.fillRect (f.getX() + 8.0f, f.getY(), f.getWidth() - 16.0f, 2.0f);
        g.setColour (border);
        g.drawRoundedRectangle (f, 8.0f, 1.0f);
    }

    // riquadri delle bande
    for (int i = 0; i < ParamEq::kBands; ++i)
    {
        auto f = ui[i].box.toFloat();
        g.setColour (panelTop);
        g.fillRoundedRectangle (f, 8.0f);
        g.setColour (bandColour (i).withAlpha (0.8f));
        g.fillRect (f.getX() + 8.0f, f.getY(), f.getWidth() - 16.0f, 2.0f);
        g.setColour (border);
        g.drawRoundedRectangle (f, 8.0f, 1.0f);
    }
}

} // namespace fc
