#include "MixerView.h"

namespace fc
{
using namespace theme;

//==============================================================================
float LevelMeter::toPos (float db) { return juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 6.0f, 0.0f, 1.0f)); }

void LevelMeter::setLevels (float l, float r)
{
    const float in[2] = { l, r };
    for (int i = 0; i < 2; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (in[i], -80.0f);
        dispDb[i] = db > dispDb[i] ? db : juce::jmax (db, dispDb[i] - 1.6f);
        peakMaxDb = juce::jmax (peakMaxDb, db);
        if (db >= holdDb[i]) { holdDb[i] = db; holdCount[i] = 45; }
        else if (--holdCount[i] <= 0) holdDb[i] = juce::jmax (-80.0f, holdDb[i] - 1.0f);
    }
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    if (showScale)
    {
        // valore numerico del picco massimo (clic per azzerare); rosso se ha superato 0 dBFS
        auto ro = b.removeFromTop ((float) readoutHeight).reduced (1.0f, 1.0f);
        const bool over = peakMaxDb > 0.0f;
        g.setColour (over ? accent.withAlpha (0.9f) : juce::Colour (0xff08090b));
        g.fillRoundedRectangle (ro, 3.0f);
        g.setColour (over ? accent : border);
        g.drawRoundedRectangle (ro.reduced (0.5f), 3.0f, 1.0f);
        g.setFont (mono (10.0f));
        g.setColour (over ? juce::Colours::white : (peakMaxDb > -6.0f ? yellow : text));
        g.drawText (peakMaxDb <= -79.0f ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) : juce::String (peakMaxDb, 1),
                    ro.toNearestInt(), juce::Justification::centred);
        b.removeFromTop (2.0f);

        auto scale = b.removeFromRight ((float) scaleWidth);

        // etichette in ordine di priorita', saltando quelle troppo vicine
        struct L { float db; const char* t; };
        static const L labels[] = { { 0.0f, "0" }, { -12.0f, "-12" }, { -24.0f, "-24" }, { -48.0f, "-48" }, { 6.0f, "+6" },
                                    { -6.0f, "-6" }, { -36.0f, "-36" }, { -60.0f, "-60" } };
        std::vector<float> placed;
        g.setFont (font (8.5f, true));
        for (auto& l : labels)
        {
            const float y = b.getBottom() - 1.5f - toPos (l.db) * (b.getHeight() - 3.0f);
            bool clash = false;
            for (float p : placed) clash = clash || std::abs (y - p) < 10.0f;
            if (clash) continue;
            placed.push_back (y);
            g.setColour (l.db == 0.0f ? text : (l.db > 0.0f ? accent.withAlpha (0.9f) : dim));
            g.fillRect (scale.getX() + 1.0f, y - 0.5f, 3.0f, 1.0f);
            g.drawText (l.t, juce::Rectangle<float> (scale.getX() + 5.0f, y - 6.0f, scale.getWidth() - 5.0f, 12.0f).toNearestInt(),
                        juce::Justification::centredLeft, false);
        }
    }
    g.setColour (juce::Colour (0xff08090b));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (border);
    g.drawRoundedRectangle (b.reduced (0.5f), 3.0f, 1.0f);

    const float barW = (b.getWidth() - 4.0f) * 0.5f;
    for (int i = 0; i < 2; ++i)
    {
        juce::Rectangle<float> bar (b.getX() + 1.5f + (float) i * (barW + 1.0f), b.getY() + 1.5f, barW, b.getHeight() - 3.0f);
        const float h = toPos (dispDb[i]) * bar.getHeight();
        juce::Rectangle<float> fill (bar.getX(), bar.getBottom() - h, bar.getWidth(), h);

        juce::ColourGradient grad (accent, bar.getX(), bar.getY(), green, bar.getX(), bar.getBottom(), false);
        grad.addColour (0.10, accent);
        grad.addColour (0.20, yellow);
        grad.addColour (0.42, green);
        g.setGradientFill (grad);
        g.fillRect (fill);

        g.setColour (text);
        g.fillRect (bar.getX(), bar.getBottom() - toPos (holdDb[i]) * bar.getHeight() - 1.0f, bar.getWidth(), 1.5f);
    }

    // tacche (-6, -12, -24, -48 dB) e linea dello 0 dB
    for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
    {
        const int y = (int) (b.getBottom() - 1.5f - toPos (db) * (b.getHeight() - 3.0f));
        g.setColour (juce::Colours::black.withAlpha (db == 0.0f ? 0.0f : 0.55f));
        g.drawHorizontalLine (y, b.getX() + 1.0f, b.getRight() - 1.0f);
        if (db == 0.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.drawHorizontalLine (y, b.getX(), b.getRight());
        }
    }
}

//==============================================================================
void InsertButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f, 0.5f);
    if (slot == nullptr)
    {
        g.setColour (juce::Colour (0xff111316));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (border.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (dim.withAlpha (0.7f));
        g.setFont (font (10.5f));
        g.drawText ("+  Insert", getLocalBounds().withTrimmedLeft (8), juce::Justification::centredLeft);
        return;
    }

    const bool err = slot->error.isNotEmpty();
    const bool ready = slot->ready.load();
    const bool byp = slot->bypass.load();

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2e323a), 0.0f, r.getY(), juce::Colour (0xff22252b), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (err ? accent.withAlpha (0.7f) : border);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

    const auto led = juce::Rectangle<float> (r.getX() + 6.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
    g.setColour (err ? accent : (! ready ? yellow : (byp ? juce::Colour (0xff5a5f6a) : green)));
    g.fillEllipse (led);

    g.setColour (byp || err ? dim : text);
    g.setFont (font (10.5f, true));
    g.drawText (slot->name, getLocalBounds().withTrimmedLeft (18).withTrimmedRight (4), juce::Justification::centredLeft, true);
}

//==============================================================================
ChannelStrip::ChannelStrip (Project& p, PluginHost& h, std::shared_ptr<TrackLive> l, int id, bool isMaster, bool isAux)
    : trackId (id), project (p), host (h), live (std::move (l)), master (isMaster), aux (isAux)
{
    for (size_t i = 0; i < inserts.size(); ++i)
    {
        inserts[i].onMouse = [this, i] (const juce::MouseEvent& e) { onInsert ((int) i, e); };
        addAndMakeVisible (inserts[i]);
    }

    if (master)
    {
        limBtn.setClickingTogglesState (true);
        limBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2f8f5b));
        monoBtn.setClickingTogglesState (true);
        monoBtn.setColour (juce::TextButton::buttonOnColourId, yellow);
        monoBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        monoBtn.setTooltip ("Ascolto in mono (controllo di compatibilita'): non influisce su export e misure");
        monoBtn.onClick = [this] { live->monoMon = monoBtn.getToggleState(); };
        addAndMakeVisible (monoBtn);
        dimBtn.setClickingTogglesState (true);
        dimBtn.setColour (juce::TextButton::buttonOnColourId, yellow);
        dimBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        dimBtn.setTooltip ("Abbassa l'ascolto di 20 dB: non influisce su export e misure");
        dimBtn.onClick = [this] { live->dimMon = dimBtn.getToggleState(); };
        addAndMakeVisible (dimBtn);
        limBtn.setTooltip ("Limiter del master: nessun picco supera il tetto. Lookahead 3 ms, rilascio 80 ms");
        limBtn.onClick = [this] { live->limiterOn = limBtn.getToggleState(); project.liveChanged(); };
        addAndMakeVisible (limBtn);

        ceil.getProperties().set ("ticks", 13);
        ceil.getProperties().set ("majorEvery", 3);
        ceil.getProperties().set ("lblMin", "-12");
        ceil.getProperties().set ("lblMax", "0");
        ceil.setRange (-12.0, 0.0, 0.1);
        ceil.setDoubleClickReturnValue (true, -1.0);
        ceil.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        ceil.setTooltip ("Tetto del limiter (dBFS). Doppio clic: -1 dB");
        ceil.onValueChange = [this] { live->ceilingDb = (float) ceil.getValue(); project.liveChanged(); repaint (ceilLabel); };
        addAndMakeVisible (ceil);
    }

    fader.setRange (-80.0, 6.0, 0.1);
    fader.setSkewFactorFromMidPoint (-18.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.onDragEnd = [this] { project.endAutoTouch(); };
    fader.onValueChange = [this]
    {
        live->volumeDb = (float) fader.getValue();
        // modalita' Scrivi: muovere il fader durante la riproduzione registra l'automazione
        if (! master && ! aux && live->autoMode.load() == 2 && fader.isMouseButtonDown() && project.isPlayingFn && project.isPlayingFn())
            project.writeAuto (trackId, AutoVolume, project.positionFn(), (float) fader.getValue());
        dbLabel.setText (fader.getValue() <= -79.5 ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e"))
                                                    : juce::String (fader.getValue(), 1), juce::dontSendNotification);
        project.liveChanged();
    };
    addAndMakeVisible (fader);

    dbLabel.setJustificationType (juce::Justification::centred);
    dbLabel.setFont (mono (12.0f));
    dbLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff0a0b0d));
    addAndMakeVisible (dbLabel);
    meter.setShowScale (true);
    meter.setTooltip ("Picco massimo (dBFS). Clic per azzerare");
    addAndMakeVisible (meter);

    if (! master && ! aux)
    {
        pan.getProperties().set ("ticks", 9);
        pan.getProperties().set ("majorEvery", 4);
        pan.getProperties().set ("centered", true);
        pan.getProperties().set ("lblMin", "L");
        pan.getProperties().set ("lblMax", "R");
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        pan.onDragEnd = [this] { project.endAutoTouch(); };
        pan.onValueChange = [this]
        {
            live->pan = (float) pan.getValue();
            if (live->autoMode.load() == 2 && pan.isMouseButtonDown() && project.isPlayingFn && project.isPlayingFn())
                project.writeAuto (trackId, AutoPan, project.positionFn(), (float) pan.getValue());
            project.liveChanged();
            repaint (panLabel);
        };
        addAndMakeVisible (pan);

        muteBtn.setClickingTogglesState (true);
        muteBtn.setColour (juce::TextButton::buttonOnColourId, muteCol);
        muteBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        muteBtn.onClick = [this] { live->mute = muteBtn.getToggleState(); project.liveChanged(); };
        soloBtn.setClickingTogglesState (true);
        soloBtn.setColour (juce::TextButton::buttonOnColourId, soloCol);
        soloBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        soloBtn.onClick = [this] { live->solo = soloBtn.getToggleState(); project.liveChanged(); };
        addAndMakeVisible (muteBtn);
        addAndMakeVisible (soloBtn);
        invertBtn.setClickingTogglesState (true);
        invertBtn.setColour (juce::TextButton::buttonOnColourId, accent);
        invertBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        invertBtn.setTooltip ("Inverte la polarita' (fase) della traccia");
        invertBtn.onClick = [this] { live->invert = invertBtn.getToggleState(); project.liveChanged(); };
        addAndMakeVisible (invertBtn);

        routeBtn.setTooltip ("Uscita della traccia: master oppure un bus di gruppo");
        routeBtn.onClick = [this]
        {
            juce::PopupMenu m;
            m.addItem (1, "Master", true, live->outBus.load() == 0);
            for (int g = 0; g < Session::kNumGroups; ++g) m.addItem (2 + g, "Gruppo " + juce::String (g + 1), true, live->outBus.load() == g + 1);
            juce::Component::SafePointer<ChannelStrip> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&routeBtn), [safe] (int r)
            {
                if (safe == nullptr || r == 0) return;
                safe->live->outBus = r == 1 ? 0 : r - 1;
                safe->project.liveChanged();
                safe->syncFromLive();
            });
        };
        addAndMakeVisible (routeBtn);
        trackLimBtn.setClickingTogglesState (false);
        trackLimBtn.setColour (juce::TextButton::buttonOnColourId, green.darker (0.15f));
        trackLimBtn.setTooltip ("Limiter della traccia (dopo il fader): nessun picco supera il tetto. Aggiunge 3 ms di latenza, compensata. Si accende di giallo quando interviene");
        trackLimBtn.onClick = [this] { live->limiterOn = ! live->limiterOn.load(); project.liveChanged(); };
        addAndMakeVisible (trackLimBtn);
        trackLimMenuBtn.setTooltip ("Tetto del limiter della traccia");
        trackLimMenuBtn.onClick = [this]
        {
            juce::PopupMenu m;
            const float cur = live->ceilingDb.load();
            static const float values[] = { -0.1f, -1.0f, -3.0f, -6.0f, -9.0f, -12.0f, -18.0f };
            for (int i = 0; i < 7; ++i) m.addItem (i + 1, "Tetto " + juce::String (values[i], 1) + " dB", true, std::abs (cur - values[i]) < 0.05f);
            juce::Component::SafePointer<ChannelStrip> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&trackLimMenuBtn), [safe] (int r)
            {
                if (safe == nullptr || r == 0) return;
                safe->live->ceilingDb = values[r - 1];
                safe->live->limiterOn = true;
                safe->project.liveChanged();
                safe->syncFromLive();
            });
        };
        addAndMakeVisible (trackLimMenuBtn);
        eqBtn.setTooltip ("EQ a 4 bande, compressore e tipo di traccia (preset per dialogo, musica, effetti). Acceso = colorato");
        eqBtn.onClick = [this] { if (onEq) onEq(); };
        addAndMakeVisible (eqBtn);

        // send verso i ritorni FX (-60 = spento)
        juce::Slider* knobs[2] = { &sendA, &sendB };
        for (int k = 0; k < 2; ++k)
        {
            auto* sl = knobs[k];
            sl->getProperties().set ("ticks", 7);
            sl->getProperties().set ("majorEvery", 2);
            sl->getProperties().set ("lblMin", "off");
            sl->getProperties().set ("lblMax", "0");
            sl->setRange (-60.0, 0.0, 0.5);
            sl->setDoubleClickReturnValue (true, -60.0);
            sl->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
            sl->onValueChange = [this, k, sl]
            {
                const float v = (float) sl->getValue();
                live->sendDb[k] = v <= -59.5f ? -80.0f : v;
                if (live->autoMode.load() == 2 && sl->isMouseButtonDown() && project.isPlayingFn && project.isPlayingFn())
                    project.writeAuto (trackId, k == 0 ? AutoSendA : AutoSendB, project.positionFn(), live->sendDb[k].load());
                sl->setTooltip (juce::String (k == 0 ? "Send A" : "Send B") + (v <= -59.5f ? ": spento" : ": " + juce::String (v, 1) + " dB")
                                + "  (post-fader, verso il ritorno FX). Doppio clic: spento");
                project.liveChanged();
            };
            sl->onDragEnd = [this] { project.endAutoTouch(); };
            sl->setTooltip (juce::String (k == 0 ? "Send A" : "Send B") + " verso il ritorno FX (post-fader). Doppio clic: spento");
            addAndMakeVisible (*sl);
        }
    }
    else if (aux)
    {
        muteBtn.setClickingTogglesState (true);
        muteBtn.setColour (juce::TextButton::buttonOnColourId, muteCol);
        muteBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        muteBtn.onClick = [this] { live->mute = muteBtn.getToggleState(); project.liveChanged(); };
        addAndMakeVisible (muteBtn);
    }
    syncFromLive();
}

void ChannelStrip::setInfo (const juce::String& n, juce::Colour c, bool sel)
{
    if (name != n || colour != c || selected != sel)
    {
        name = n; colour = c; selected = sel;
        repaint();
    }
}

void ChannelStrip::syncFromLive()
{
    fader.setValue (live->volumeDb.load(), juce::dontSendNotification);
    dbLabel.setText (live->volumeDb.load() <= -79.5f ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e"))
                                                      : juce::String (live->volumeDb.load(), 1), juce::dontSendNotification);
    if (! master && ! aux)
    {
        pan.setValue (live->pan.load(), juce::dontSendNotification);
        muteBtn.setToggleState (live->mute.load(), juce::dontSendNotification);
        soloBtn.setToggleState (live->solo.load(), juce::dontSendNotification);
        invertBtn.setToggleState (live->invert.load(), juce::dontSendNotification);
        trackLimBtn.setToggleState (live->limiterOn.load(), juce::dontSendNotification);
        trackLimBtn.setButtonText (live->limiterOn.load() ? "LIM " + juce::String (live->ceilingDb.load(), 0) : juce::String ("LIM"));
        eqBtn.setToggleState ((live->eq.enabled.load() && ! live->eq.isNeutral()) || live->comp.enabled.load(), juce::dontSendNotification);
        repaint (0, 0, getWidth(), 30);
        routeBtn.setButtonText (live->outBus.load() == 0 ? juce::String ("MASTER") : "GRUPPO " + juce::String (live->outBus.load()));
        sendA.setValue (live->sendDb[0].load() <= -79.5f ? -60.0 : (double) live->sendDb[0].load(), juce::dontSendNotification);
        sendB.setValue (live->sendDb[1].load() <= -79.5f ? -60.0 : (double) live->sendDb[1].load(), juce::dontSendNotification);
    }
    else if (aux)
        muteBtn.setToggleState (live->mute.load(), juce::dontSendNotification);
    else
    {
        limBtn.setToggleState (live->limiterOn.load(), juce::dontSendNotification);
        monoBtn.setToggleState (live->monoMon.load(), juce::dontSendNotification);
        dimBtn.setToggleState (live->dimMon.load(), juce::dontSendNotification);
        ceil.setValue (live->ceilingDb.load(), juce::dontSendNotification);
        ceil.setEnabled (live->limiterOn.load());
        repaint (ceilLabel);
    }
    refreshInserts();
}

void ChannelStrip::refreshInserts()
{
    auto chain = live->getChain();
    for (size_t i = 0; i < inserts.size(); ++i)
    {
        std::shared_ptr<InsertSlot> slot = (chain != nullptr && i < chain->slots.size()) ? chain->slots[i] : nullptr;
        inserts[i].setSlot (slot);
        juce::String tip = slot == nullptr ? juce::String ("Clic per aggiungere un plug-in VST3")
                         : slot->error.isNotEmpty() ? slot->error
                         : slot->name + (slot->desc.manufacturerName.isNotEmpty() ? " (" + slot->desc.manufacturerName + ")" : juce::String())
                               + (slot->ready.load() && slot->plugin != nullptr && slot->plugin->getLatencySamples() > 0
                                      ? "\nLatenza: " + juce::String (slot->plugin->getLatencySamples()) + " campioni ("
                                            + juce::String (slot->plugin->getLatencySamples() * 1000.0 / juce::jmax (1.0, project.getSampleRate()), 1) + " ms), compensata"
                                      : juce::String())
                               + "\nClic: apre l'editor.  Clic destro: bypass, sposta, rimuovi";
        inserts[i].setTooltip (tip);
    }
}

void ChannelStrip::onInsert (int index, const juce::MouseEvent& e)
{
    auto chain = live->getChain();
    std::shared_ptr<InsertSlot> slot = (chain != nullptr && index < (int) chain->slots.size()) ? chain->slots[(size_t) index] : nullptr;

    if (slot == nullptr) { host.pickAndAdd (live, this); return; }

    if (! e.mods.isPopupMenu())
    {
        if (slot->ready.load()) host.openEditor (slot, master ? juce::String ("Master") : name);
        return;
    }

    juce::PopupMenu m;
    m.addItem (1, "Apri l'editor", slot->ready.load());
    m.addItem (2, "Bypass", true, slot->bypass.load());
    m.addSeparator();
    m.addItem (3, juce::String (juce::CharPointer_UTF8 ("Sposta su")), index > 0);
    m.addItem (4, juce::String (juce::CharPointer_UTF8 ("Sposta gi\xc3\xb9")), index + 1 < (int) chain->slots.size());
    m.addSeparator();
    m.addItem (5, "Rimuovi");

    juce::Component::SafePointer<ChannelStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, index, slot] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& h = safe->host;
        auto& l = *safe->live;
        if (r == 1) h.openEditor (slot, safe->master ? juce::String ("Master") : safe->name);
        else if (r == 2) h.setBypass (l, index, ! slot->bypass.load());
        else if (r == 3) h.moveSlot (l, index, -1);
        else if (r == 4) h.moveSlot (l, index, +1);
        else if (r == 5) h.removeSlot (l, index);
    });
}

void ChannelStrip::showAutomatedVolume (float db)
{
    if (fader.isMouseButtonDown()) return;
    fader.setValue (db, juce::dontSendNotification);
    dbLabel.setText (db <= -79.5f ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) : juce::String (db, 1), juce::dontSendNotification);
}

void ChannelStrip::showAutomatedPan (float v)
{
    if (pan.isMouseButtonDown()) return;
    pan.setValue (v, juce::dontSendNotification);
    repaint (panLabel);
}

void ChannelStrip::showAutomatedSend (int k, float db)
{
    auto& sl = k == 0 ? sendA : sendB;
    if (sl.isMouseButtonDown()) return;
    sl.setValue (db <= -59.5f ? -60.0 : (double) db, juce::dontSendNotification);
}

void ChannelStrip::updateMeter()
{
    meter.setLevels (live->peakL.exchange (0.0f), live->peakR.exchange (0.0f));
    if (! master && ! aux)
    {
        const float gr = live->grDb.exchange (0.0f);
        const bool active = live->limiterOn.load() && gr > 0.2f;
        if (active != limActive)
        {
            limActive = active;
            trackLimBtn.setColour (juce::TextButton::buttonOnColourId, active ? yellow : green.darker (0.15f));
            trackLimBtn.setColour (juce::TextButton::textColourOnId, active ? juce::Colours::black : juce::Colours::white);
            trackLimBtn.repaint();
        }
    }
    if (master)
    {
        const float gr = live->grDb.exchange (0.0f);
        const float next = juce::jmax (gr, grDisp - 0.7f);
        const float nextHold = juce::jmax (gr, grHold - 0.04f);   // il valore numerico scende piano, cosi' si legge
        if (std::abs (next - grDisp) > 0.01f || std::abs (nextHold - grHold) > 0.02f) { grDisp = next; grHold = nextHold; repaint (grRect.expanded (12, 18)); }
    }
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    if (! master && ! aux && e.y < 22)
    {
        project.selectedTrackId = trackId;
        project.sendChangeMessage();
    }
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (3.0f);
    juce::Path shape;
    shape.addRoundedRectangle (b, 8.0f);

    g.setGradientFill (juce::ColourGradient (selected ? panelHi : panelTop, 0.0f, b.getY(), panel, 0.0f, b.getBottom(), false));
    g.fillPath (shape);

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (shape);
        const juce::Colour c = master ? accent : colour;
        g.setGradientFill (juce::ColourGradient (c.withAlpha (0.32f), 0.0f, b.getY(), c.withAlpha (0.10f), 0.0f, b.getY() + 26.0f, false));
        g.fillRect (b.withHeight (26.0f));
        g.setColour (c);
        g.fillRect (b.getX(), b.getY() + 25.0f, b.getWidth(), 2.0f);
    }

    g.setColour (selected ? blue.withAlpha (0.9f) : border);
    g.drawRoundedRectangle (b, 8.0f, selected ? 1.5f : 1.0f);

    g.setColour (text);
    g.setFont (font (12.0f, true));
    const int type = (! master && ! aux) ? live->trackType.load() : 0;
    g.drawText (master ? "MASTER" : name, getLocalBounds().withHeight (type > 0 ? 22 : 28).withTrimmedTop (type > 0 ? 1 : 4).reduced (8, 0),
                juce::Justification::centred, true);
    if (type > 0)   // tipo di traccia (dialogo, musica, effetti) sotto il nome
    {
        g.setColour (trackTypeColour (type));
        g.setFont (font (8.5f, true));
        g.drawText (trackTypeTag (type), getLocalBounds().withHeight (26).withTrimmedTop (15).reduced (8, 0), juce::Justification::centred);
    }

    if (master)
    {
        g.setColour (dim);
        g.setFont (mono (10.5f));
        g.drawText ((live->limiterOn.load() ? juce::String (live->ceilingDb.load(), 1) + " dB" : juce::String ("OFF")), ceilLabel, juce::Justification::centred);

        g.setColour (juce::Colour (0xff08090b));
        g.fillRoundedRectangle (grRect.toFloat(), 2.0f);
        const float frac = juce::jlimit (0.0f, 1.0f, grDisp / 12.0f);
        g.setColour (yellow);
        g.fillRect (grRect.toFloat().reduced (1.0f).withHeight ((float) (grRect.getHeight() - 2) * frac));
        for (int db = 3; db <= 12; db += 3)   // scala della riduzione: 3, 6, 9, 12 dB
        {
            const float ty = (float) grRect.getY() + 1.0f + (float) (grRect.getHeight() - 2) * (float) db / 12.0f;
            g.setColour (dim.withAlpha (0.7f));
            g.drawHorizontalLine ((int) ty, (float) grRect.getX() - 3.0f, (float) grRect.getX());
        }
        drawCaption (g, "GR", juce::Rectangle<int> (grRect.getX() - 6, grRect.getBottom() + 1, grRect.getWidth() + 12, 12), juce::Justification::centred);
        g.setColour (grHold > 0.1f ? yellow : dim);   // lettura numerica della riduzione
        g.setFont (mono (9.0f));
        g.drawText ((grHold > 0.05f ? "-" : "") + juce::String (grHold, 1), juce::Rectangle<int> (grRect.getX() - 10, grRect.getY() - 14, grRect.getWidth() + 20, 12), juce::Justification::centred);
    }

    if (! master && ! aux)
    {
        drawCaption (g, "Send A", sendCaptionA, juce::Justification::centred);
        drawCaption (g, "Send B", sendCaptionB, juce::Justification::centred);
    }

    if (! master && ! aux)
    {
        const float pv = live->pan.load();
        const juce::String ps = std::abs (pv) < 0.01f ? "C" : (pv < 0 ? "L" + juce::String ((int) std::round (-pv * 100)) : "R" + juce::String ((int) std::round (pv * 100)));
        g.setColour (dim);
        g.setFont (mono (10.5f));
        g.drawText (ps, panLabel, juce::Justification::centred);
    }
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (5);
    r.removeFromTop (28);
    if (! master && ! aux)
    {
        pan.setBounds (r.removeFromTop (46).withSizeKeepingCentre (46, 46));
        panLabel = r.removeFromTop (12);
        auto row = r.removeFromTop (26).reduced (2, 2);
        const int bw = (row.getWidth() - 8) / 3;
        muteBtn.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (4);
        soloBtn.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (4);
        invertBtn.setBounds (row);
        r.removeFromTop (2);
        auto sends = r.removeFromTop (36);
        auto left = sends.removeFromLeft (sends.getWidth() / 2);
        sendA.setBounds (left.withSizeKeepingCentre (36, 36));
        sendB.setBounds (sends.withSizeKeepingCentre (36, 36));
        auto caps = r.removeFromTop (10);
        sendCaptionA = caps.removeFromLeft (caps.getWidth() / 2);
        sendCaptionB = caps;
        {
            auto row = r.removeFromTop (22).reduced (2, 1);
            eqBtn.setBounds (row.removeFromLeft (34));
            row.removeFromLeft (3);
            routeBtn.setBounds (row);
        }
        {
            auto row = r.removeFromTop (22).reduced (2, 1);
            trackLimMenuBtn.setBounds (row.removeFromRight (24));
            row.removeFromRight (2);
            trackLimBtn.setBounds (row);
        }
    }
    else if (aux)
    {
        r.removeFromTop (6);
        muteBtn.setBounds (r.removeFromTop (26).reduced (4, 2));
    }
    else
    {
        limBtn.setBounds (r.removeFromTop (26).reduced (4, 2));
        {
            auto mrow = r.removeFromTop (22).reduced (4, 1);
            monoBtn.setBounds (mrow.removeFromLeft (mrow.getWidth() / 2 - 2));
            dimBtn.setBounds (mrow.withTrimmedLeft (4));
        }
        ceil.setBounds (r.removeFromTop (48).withSizeKeepingCentre (46, 46));
        ceilLabel = r.removeFromTop (14);
    }

    r.removeFromTop (4);
    for (auto& ins : inserts)
    {
        ins.setBounds (r.removeFromTop (17).reduced (1, 0));
        r.removeFromTop (1);
    }
    r.removeFromTop (4);

    dbLabel.setBounds (r.removeFromBottom (22).reduced (14, 2));
    r.removeFromBottom (4);
    auto m = r.removeFromRight (18 + LevelMeter::scaleWidth);
    meter.setBounds (m.withTrimmedBottom (8));
    if (master)
    {
        r.removeFromRight (3);
        grRect = r.removeFromRight (12).withTrimmedTop (LevelMeter::readoutHeight + 4).withTrimmedBottom (20);
    }
    fader.setBounds (r);
}

//==============================================================================
LoudnessPanel::LoudnessPanel (Project& p, Loudness::Meter& m) : project (p), meter (m)
{
    const auto& list = Loudness::standards();
    for (size_t i = 0; i < list.size(); ++i) standard.addItem (list[i].name, (int) i + 1);
    standard.setSelectedId (project.loudnessStandard + 1, juce::dontSendNotification);
    standard.onChange = [this]
    {
        project.loudnessStandard = standard.getSelectedId() - 1;
        project.liveChanged();
        repaint();
    };
    addAndMakeVisible (standard);

    resetBtn.setTooltip ("Azzera la misura integrata, il loudness range e i picchi");
    resetBtn.onClick = [this] { meter.requestReset(); tpDb = -100.0f; integ = -100.0f; repaint(); };
    addAndMakeVisible (resetBtn);
    startTimerHz (15);
}

void LoudnessPanel::resized()
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (30);
    standard.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    bigBox = r.removeFromTop (84);
    r.removeFromTop (8);
    barBox = r.removeFromTop (34);
    resetBtn.setBounds (getLocalBounds().reduced (10).removeFromBottom (28));
}

void LoudnessPanel::timerCallback()
{
    // l'indice dello standard puo' cambiare per una sessione appena caricata
    const int wanted = project.loudnessStandard + 1;
    if (standard.getSelectedId() != wanted) standard.setSelectedId (wanted, juce::dontSendNotification);

    const float tp = juce::Decibels::gainToDecibels (meter.truePeak.load(), -100.0f);
    const float nm = meter.momentary.load(), ns = meter.shortTerm.load(), ni = meter.integrated.load(), nl = meter.lra.load();
    if (nm != mom || ns != st || ni != integ || nl != lra || tp != tpDb)
    {
        mom = nm; st = ns; integ = ni; lra = nl; tpDb = tp;
        repaint();
    }
}

void LoudnessPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (3.0f);
    juce::Path shape;
    shape.addRoundedRectangle (b, 8.0f);
    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, b.getY(), panel, 0.0f, b.getBottom(), false));
    g.fillPath (shape);
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (shape);
        g.setGradientFill (juce::ColourGradient (blue.withAlpha (0.30f), 0.0f, b.getY(), blue.withAlpha (0.08f), 0.0f, b.getY() + 26.0f, false));
        g.fillRect (b.withHeight (26.0f));
        g.setColour (blue);
        g.fillRect (b.getX(), b.getY() + 25.0f, b.getWidth(), 2.0f);
    }
    g.setColour (border);
    g.drawRoundedRectangle (b, 8.0f, 1.0f);
    g.setColour (text);
    g.setFont (font (12.0f, true));

    const auto& std_ = Loudness::standards()[(size_t) juce::jlimit (0, (int) Loudness::standards().size() - 1, project.loudnessStandard)];
    const bool hasTarget = project.loudnessStandard > 0;
    auto fmt = [] (float v) { return v <= -99.0f ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) : juce::String (v, 1); };

    // valore integrato, colorato secondo la distanza dal target
    {
        const float dev = integ > -99.0f && hasTarget ? integ - (float) std_.target : 0.0f;
        juce::Colour c = text;
        if (hasTarget && integ > -99.0f)
            c = std::abs (dev) <= (float) std_.tolerance ? green : (std::abs (dev) <= (float) std_.tolerance + 1.5f ? yellow : accent);

        auto f = bigBox.toFloat();
        g.setColour (juce::Colour (0xff08090b));
        g.fillRoundedRectangle (f, 8.0f);
        g.setColour (hasTarget && integ > -99.0f ? c.withAlpha (0.7f) : border);
        g.drawRoundedRectangle (f.reduced (0.5f), 8.0f, 1.0f);
        drawCaption (g, "Integrato (LUFS)", bigBox.withTrimmedLeft (12).withHeight (18).translated (0, 4), juce::Justification::centredLeft);
        g.setColour (c);
        g.setFont (mono (34.0f));
        g.drawText (fmt (integ), bigBox.withTrimmedTop (16).withTrimmedBottom (18), juce::Justification::centred);
        g.setFont (font (11.0f));
        g.setColour (dim);
        juce::String sub = "nessun obiettivo";
        if (hasTarget)
            sub = "obiettivo " + juce::String (std_.target, 0) + " LUFS" + (integ > -99.0f ? "   " + juce::String (dev >= 0 ? "+" : "") + juce::String (dev, 1) + " LU" : juce::String());
        g.drawText (sub, bigBox.withTrimmedTop (bigBox.getHeight() - 20), juce::Justification::centred);
    }

    // barra del momentary con il target
    {
        auto f = barBox.toFloat().withTrimmedTop (14.0f);
        auto toX = [&f] (float lufs) { return f.getX() + juce::jlimit (0.0f, 1.0f, (lufs + 50.0f) / 45.0f) * f.getWidth(); };
        drawCaption (g, "Momentaneo", barBox.withHeight (13), juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (mono (11.0f));
        g.drawText (fmt (mom), barBox.withHeight (13), juce::Justification::centredRight);

        g.setColour (juce::Colour (0xff08090b));
        g.fillRoundedRectangle (f, 3.0f);
        if (hasTarget)
        {
            g.setColour (green.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (toX ((float) (std_.target - std_.tolerance)), f.getY(), toX ((float) (std_.target + std_.tolerance)) - toX ((float) (std_.target - std_.tolerance)), f.getHeight()));
        }
        if (mom > -99.0f)
        {
            const float x = toX (mom);
            g.setGradientFill (juce::ColourGradient (green, f.getX(), 0.0f, accent, f.getRight(), 0.0f, false));
            g.fillRect (juce::Rectangle<float> (f.getX(), f.getY() + 2.0f, x - f.getX(), f.getHeight() - 4.0f));
        }
        if (hasTarget)
        {
            g.setColour (text);
            g.fillRect (toX ((float) std_.target) - 0.75f, f.getY() - 1.0f, 1.5f, f.getHeight() + 2.0f);
        }
        g.setColour (dim.withAlpha (0.7f));
        g.setFont (font (8.5f));
        for (float v : { -40.0f, -30.0f, -20.0f, -10.0f })
            g.drawText (juce::String ((int) v), juce::Rectangle<float> (toX (v) - 10.0f, f.getBottom() - 0.0f, 20.0f, 10.0f).toNearestInt(), juce::Justification::centred);
    }

    // righe: short-term, LRA, true peak
    auto rows = getLocalBounds().reduced (12);
    rows.removeFromTop (30 + 26 + 8 + 84 + 8 + 34 + 12);
    auto row = [&] (const juce::String& label, const juce::String& value, juce::Colour c)
    {
        auto r = rows.removeFromTop (24);
        g.setColour (border);
        g.drawHorizontalLine (r.getBottom() - 1, (float) r.getX(), (float) r.getRight());
        drawCaption (g, label, r.withTrimmedRight (90), juce::Justification::centredLeft);
        g.setColour (c);
        g.setFont (mono (14.0f));
        g.drawText (value, r, juce::Justification::centredRight);
    };
    row ("Short-term (LUFS)", fmt (st), text);
    row ("Loudness range (LU)", lra > 0.0f ? juce::String (lra, 1) : juce::String (juce::CharPointer_UTF8 ("--")), text);
    const bool tpOver = hasTarget && tpDb > (float) std_.ceiling;
    row ("True peak (dBTP)", fmt (tpDb), tpOver ? accent : (tpDb > -1.0f ? yellow : text));
    if (hasTarget)
    {
        g.setColour (dim);
        g.setFont (font (10.0f));
        g.drawFittedText (juce::String (std_.detail), rows.removeFromTop (60).reduced (0, 4), juce::Justification::topLeft, 4);
    }
}

//==============================================================================
MixerView::MixerView (Project& p, PluginHost& h, Loudness::Meter& lm, SpectrumView::Reader rd, std::function<double()> sr)
    : project (p), host (h), loudnessPanel (p, lm), spectrum (std::move (rd), std::move (sr))
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
    masterStrip = std::make_unique<ChannelStrip> (project, host, project.getSession().master, -1, true);
    addAndMakeVisible (*masterStrip);
    addAndMakeVisible (loudnessPanel);
    addChildComponent (spectrum);
    for (auto* t : { &tabLoud, &tabSpec })
    {
        t->setClickingTogglesState (false);
        t->setColour (juce::TextButton::buttonOnColourId, blue);
        addAndMakeVisible (*t);
    }
    tabLoud.onClick = [this] { showAnalysis (false); };
    tabSpec.onClick = [this] { showAnalysis (true); };
    showAnalysis (false);
    for (int k = 0; k < 2; ++k) makeAux (k);
    for (int k = 0; k < Session::kNumGroups; ++k) makeGroup (k);
    project.addChangeListener (this);
    rebuild();
    startTimerHz (30);
}

void MixerView::makeGroup (int k)
{
    if (groupStrips[k] != nullptr) content.removeChildComponent (groupStrips[k].get());
    groupStrips[k] = std::make_unique<ChannelStrip> (project, host, project.getSession().group[(size_t) k], -10 - k, false, true);
    groupStrips[k]->setInfo ("GRUPPO " + juce::String (k + 1), juce::Colour (0xffb58cff), false);
    content.addAndMakeVisible (*groupStrips[k]);
}

void MixerView::makeAux (int k)
{
    if (auxStrips[k] != nullptr) removeChildComponent (auxStrips[k].get());
    auxStrips[k] = std::make_unique<ChannelStrip> (project, host, project.getSession().aux[(size_t) k], -2 - k, false, true);
    auxStrips[k]->setInfo (k == 0 ? "FX A" : "FX B", juce::Colour (0xff4aa8c9), false);
    addAndMakeVisible (*auxStrips[k]);
}

MixerView::~MixerView() { project.removeChangeListener (this); }

void MixerView::openEq (int trackId)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr) return;
    for (auto& w : eqWindows)
        if (w.first == t->live) { w.second->setName (t->name + " - EQ e compressore"); w.second->toFront (true); return; }

    auto live = t->live;
    juce::Component::SafePointer<MixerView> safe (this);
    auto win = std::make_unique<EqWindow> (t->name + " - EQ e compressore", [safe, live]
    {
        juce::MessageManager::callAsync ([safe, live]
        {
            if (safe == nullptr) return;
            auto& v = safe->eqWindows;
            v.erase (std::remove_if (v.begin(), v.end(), [&live] (auto& w) { return w.first == live; }), v.end());
        });
    });
    win->setContentOwned (new EqEditor (project, live, [this] { return project.getSampleRate(); }), true);
    win->centreAroundComponent (this, win->getWidth(), win->getHeight());
    win->setVisible (true);
    eqWindows.emplace_back (live, std::move (win));
}

void MixerView::showAnalysis (bool spec)
{
    loudnessPanel.setVisible (! spec);
    spectrum.setVisible (spec);
    tabLoud.setToggleState (! spec, juce::dontSendNotification);
    tabSpec.setToggleState (spec, juce::dontSendNotification);
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (border);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
}

void MixerView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (1);
    masterStrip->setBounds (r.removeFromRight (stripW + 8).reduced (2));
    const auto analysis = r.removeFromRight (344).reduced (2);
    loudnessPanel.setBounds (analysis);
    spectrum.setBounds (analysis);
    auto tabs = analysis.withHeight (28).withTrimmedTop (5).reduced (12, 1);
    tabLoud.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2 - 2));
    tabSpec.setBounds (tabs.withTrimmedLeft (4));
    for (int k = 1; k >= 0; --k) auxStrips[k]->setBounds (r.removeFromRight (stripW + 4).reduced (2));
    viewport.setBounds (r);
    const int h = r.getHeight() - viewport.getScrollBarThickness();
    const int groupX = (int) strips.size() * stripW + 16;   // i bus di gruppo seguono le tracce, separati da uno spazio
    content.setSize (juce::jmax (r.getWidth(), groupX + Session::kNumGroups * stripW + 8), h);
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds ((int) i * stripW + 4, 0, stripW, h);
    for (int k = 0; k < Session::kNumGroups; ++k)
        groupStrips[k]->setBounds (groupX + k * stripW, 0, stripW, h);
}

void MixerView::rebuild()
{
    strips.clear();
    for (auto& gs : groupStrips) if (gs != nullptr) content.removeChildComponent (gs.get());   // restano di proprieta' di MixerView
    content.removeAllChildren();
    for (auto& gs : groupStrips) if (gs != nullptr) content.addAndMakeVisible (*gs);
    for (auto& t : project.getSession().tracks)
    {
        auto s = std::make_unique<ChannelStrip> (project, host, t.live, t.id, false);
        s->onEq = [this, id = t.id] { openEq (id); };
        content.addAndMakeVisible (*s);
        strips.push_back (std::move (s));
    }
    resized();
}

void MixerView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // le strip vanno ricostruite se cambiano le tracce o gli oggetti live (nuova sessione, undo di una cancellazione...)
    const auto& tracks = project.getSession().tracks;
    bool same = strips.size() == tracks.size();
    for (size_t i = 0; same && i < tracks.size(); ++i)
        same = strips[i]->trackId == tracks[i].id && strips[i]->getLive() == tracks[i].live;
    if (! same) rebuild();
    eqWindows.erase (std::remove_if (eqWindows.begin(), eqWindows.end(), [&tracks] (auto& w)
    {
        for (auto& t : tracks) if (t.live == w.first) return false;
        return true;   // la traccia non c'e' piu': si chiude il suo equalizzatore
    }), eqWindows.end());

    if (masterStrip->getLive() != project.getSession().master)
    {
        removeChildComponent (masterStrip.get());
        masterStrip = std::make_unique<ChannelStrip> (project, host, project.getSession().master, -1, true);
        addAndMakeVisible (*masterStrip);
        resized();
    }

    for (int k = 0; k < 2; ++k)
    {
        if (auxStrips[k]->getLive() != project.getSession().aux[(size_t) k]) { makeAux (k); resized(); }
        auxStrips[k]->syncFromLive();
    }
    for (int k = 0; k < Session::kNumGroups; ++k)
    {
        if (groupStrips[k]->getLive() != project.getSession().group[(size_t) k]) { makeGroup (k); resized(); }
        groupStrips[k]->syncFromLive();
    }
    masterStrip->syncFromLive();
    for (size_t i = 0; i < strips.size() && i < tracks.size(); ++i)
    {
        strips[i]->setInfo (tracks[i].name, tracks[i].colour, tracks[i].id == project.selectedTrackId);
        strips[i]->syncFromLive();
    }
}

void MixerView::resetPeaks()
{
    for (auto& s : strips) s->resetPeak();
    for (auto& a : auxStrips) a->resetPeak();
    for (auto& a : groupStrips) a->resetPeak();
    masterStrip->resetPeak();
}

void MixerView::timerCallback()
{
    // con l'automazione attiva i fader seguono la curva durante la riproduzione
    if (project.isPlayingFn && project.isPlayingFn())
    {
        const auto pos = project.positionFn();
        const auto& tracks = project.getSession().tracks;
        for (size_t i = 0; i < strips.size() && i < tracks.size(); ++i)
        {
            const auto& t = tracks[i];
            if (t.live->autoMode.load() == 0) continue;
            if (! t.volAuto.empty()) strips[i]->showAutomatedVolume (evalAutomationDb (t.volAuto, pos));
            if (! t.panAuto.empty()) strips[i]->showAutomatedPan (evalAutomationDb (t.panAuto, pos));
            if (! t.sendAAuto.empty()) strips[i]->showAutomatedSend (0, evalAutomationDb (t.sendAAuto, pos));
            if (! t.sendBAuto.empty()) strips[i]->showAutomatedSend (1, evalAutomationDb (t.sendBAuto, pos));
        }
    }
    for (auto& s : strips) s->updateMeter();
    for (auto& a : auxStrips) a->updateMeter();
    for (auto& a : groupStrips) a->updateMeter();
    masterStrip->updateMeter();
}

} // namespace fc
