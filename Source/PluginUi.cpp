#include "PluginUi.h"

namespace fc
{
using namespace theme;

namespace
{
juce::Colour kindColour (BuiltinFx::Kind k)
{
    static const int idx[BuiltinFx::numKinds] = { 4, 1, 3, 5, 6, 2, 0, 7 };   // dalla palette delle tracce
    return trackPalette[idx[k]];
}
} // namespace

//==============================================================================
BuiltinEditor::BuiltinEditor (BuiltinFx& f, std::atomic<bool>& byp, std::function<void()> onBypass)
    : juce::AudioProcessorEditor (f), fx (f), bypass (byp), bypassChanged (std::move (onBypass))
{
    constexpr int cellW = 84, sliderH = 90, nameH = 16, pad = 12, titleH = 24, gap = 10, margin = 14, headerH = 60;
    const bool wide = fx.getKind() == BuiltinFx::Multiband;
    const int maxRowW = wide ? 880 : 520;

    bypassButton.setClickingTogglesState (true);
    bypassButton.setToggleState (bypass.load(), juce::dontSendNotification);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, soloCol.darker (0.35f));
    bypassButton.setTooltip ("Esclude l'effetto senza rimuoverlo dalla catena");
    bypassButton.onClick = [this]
    {
        bypass = bypassButton.getToggleState();
        if (bypassChanged) bypassChanged();
        repaint();
    };
    addAndMakeVisible (bypassButton);

    int x = margin, y = headerH + margin, rowH = 0, widest = 0;
    if (fx.getKind() == BuiltinFx::Filter)
    {
        curveArea = { margin, y, 1, 130 };   // la larghezza si completa sotto
        y += curveArea.getHeight() + gap;
    }

    for (auto& g : BuiltinFx::layout (fx.getKind()))
    {
        auto p = std::make_unique<Panel>();
        p->group = g;
        const int meterW = g.meter >= 0 ? 30 : 0;
        const int w = pad * 2 + (int) g.knobs.size() * cellW + meterW;
        const int h = titleH + nameH + sliderH + pad + 6;
        if (x > margin && (g.newRow || x + w > maxRowW)) { x = margin; y += rowH + gap; rowH = 0; }
        p->bounds = { x, y, w, h };
        if (g.meter >= 0) p->meterBounds = { x + w - pad - 18, y + titleH + nameH + 4, 18, sliderH - 16 };
        x += w + gap;
        rowH = juce::jmax (rowH, h);
        widest = juce::jmax (widest, x - gap);

        int cx = p->bounds.getX() + pad;
        for (auto& k : g.knobs)
        {
            auto* param = fx.param (k.id);
            if (param == nullptr) continue;
            auto c = std::make_unique<Control>();
            c->name.setText (k.label, juce::dontSendNotification);
            c->name.setJustificationType (juce::Justification::centred);
            c->name.setFont (font (11.0f, true));
            c->name.setColour (juce::Label::textColourId, dim);
            c->name.setBounds (cx, p->bounds.getY() + titleH, cellW, nameH);
            addAndMakeVisible (c->name);

            if (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr)
            {
                c->toggle = std::make_unique<juce::TextButton> ("OFF");
                c->toggle->setClickingTogglesState (true);
                c->toggle->setColour (juce::TextButton::buttonOnColourId, blue);
                c->toggle->setBounds (cx + 8, p->bounds.getY() + titleH + nameH + 24, cellW - 16, 30);
                c->buttonAttach = std::make_unique<juce::ButtonParameterAttachment> (*param, *c->toggle);
                auto* tb = c->toggle.get();
                tb->onStateChange = [tb] { tb->setButtonText (tb->getToggleState() ? "ON" : "OFF"); };
                tb->onStateChange();
                addAndMakeVisible (*c->toggle);
            }
            else
            {
                auto s = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
                s->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
                s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, cellW - 8, 18);
                s->setColour (juce::Slider::textBoxTextColourId, text);
                s->setColour (juce::Slider::textBoxBackgroundColourId, bg);
                s->setColour (juce::Slider::textBoxOutlineColourId, border);
                s->setMouseDragSensitivity (180);
                s->setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
                s->setTooltip ("Trascina per regolare, doppio clic per il valore iniziale, rotellina per i passi fini");
                const auto range = param->getNormalisableRange();
                if (range.start < 0.0f && range.end > 0.0f) s->getProperties().set ("centered", true);   // arco dal centro: guadagni e uscite
                s->getProperties().set ("ticks", 9);
                s->getProperties().set ("majorEvery", 4);
                s->setBounds (cx, p->bounds.getY() + titleH + nameH, cellW, sliderH);
                c->sliderAttach = std::make_unique<juce::SliderParameterAttachment> (*param, *s);
                s->textFromValueFunction = [param] (double v) { return param->getText (param->convertTo0to1 ((float) v), 0) + param->getLabel(); };   // valore con unita'
                s->valueFromTextFunction = [] (const juce::String& t) { return (double) t.retainCharacters ("0123456789.-").getFloatValue(); };
                s->updateText();
                c->slider = std::move (s);
                addAndMakeVisible (*c->slider);
            }
            cx += cellW;
            p->controls.push_back (std::move (c));
        }
        panels.push_back (std::move (p));
    }

    if (fx.getKind() == BuiltinFx::Filter) curveArea.setWidth (juce::jmax (widest, 500));
    const int width = juce::jmax (widest, curveArea.getWidth()) + margin * 2;
    headerArea = { 0, 0, width, headerH };
    setSize (width, y + rowH + margin);
    setResizable (false, false);
    startTimerHz (30);
}

BuiltinEditor::~BuiltinEditor() { stopTimer(); }

void BuiltinEditor::resized()
{
    bypassButton.setBounds (getWidth() - 14 - 92, (headerArea.getHeight() - 28) / 2, 92, 28);
}

void BuiltinEditor::timerCallback()
{
    bool changed = false;
    for (auto& p : panels)
        if (p->group.meter >= 0)
        {
            const float v = fx.meterDb (p->group.meter);
            auto& shown = shownMeter[(size_t) p->group.meter];
            const float next = v > shown ? v : juce::jmax (v, shown - 0.9f);   // sale subito, scende piano
            if (std::abs (next - shown) > 0.05f) { shown = next; repaint (p->meterBounds.expanded (6, 16)); changed = true; }
        }
    if (! curveArea.isEmpty()) repaint (curveArea);
    juce::ignoreUnused (changed);
}

void BuiltinEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    // intestazione
    g.setGradientFill (juce::ColourGradient (panelHi, 0.0f, 0.0f, panelTop, 0.0f, (float) headerArea.getHeight(), false));
    g.fillRect (headerArea);
    g.setColour (border);
    g.drawHorizontalLine (headerArea.getBottom() - 1, 0.0f, (float) getWidth());
    const auto colour = kindColour (fx.getKind());
    auto badge = juce::Rectangle<float> (14.0f, (float) (headerArea.getHeight() - 36) * 0.5f, 36.0f, 36.0f);
    g.setColour (colour.withAlpha (0.9f));
    g.fillRoundedRectangle (badge, 9.0f);
    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.setFont (font (14.0f, true));
    g.drawText (BuiltinFx::kindBadge (fx.getKind()), badge.toNearestInt(), juce::Justification::centred);
    g.setColour (text);
    g.setFont (font (17.0f, true));
    g.drawText (BuiltinFx::kindName (fx.getKind()), 62, 10, getWidth() - 62 - 120, 22, juce::Justification::centredLeft, true);
    g.setColour (dim);
    g.setFont (font (11.5f));
    g.drawText (BuiltinFx::kindHelp (fx.getKind()), 62, 32, getWidth() - 62 - 120, 18, juce::Justification::centredLeft, true);

    if (! curveArea.isEmpty()) paintCurve (g, curveArea.toFloat());

    for (auto& p : panels)
    {
        auto b = p->bounds.toFloat();
        g.setColour (panelTop);
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (border);
        g.drawRoundedRectangle (b.reduced (0.5f), 8.0f, 1.0f);
        g.setColour (colour.withAlpha (0.85f));
        g.fillRoundedRectangle (b.getX() + 12.0f, b.getY() + 10.0f, 3.0f, 11.0f, 1.5f);
        drawCaption (g, p->group.title, { p->bounds.getX() + 20, p->bounds.getY() + 6, p->bounds.getWidth() - 30, 18 });

        if (p->group.meter >= 0)
        {
            const float v = shownMeter[(size_t) p->group.meter];
            auto m = p->meterBounds.toFloat();
            g.setColour (bg);
            g.fillRoundedRectangle (m, 3.0f);
            const float full = fx.getKind() == BuiltinFx::Gate ? 40.0f : 18.0f;   // fondo scala della riduzione (dB)
            const float fill = juce::jlimit (0.0f, 1.0f, v / full);
            if (fill > 0.0f)
            {
                g.setGradientFill (juce::ColourGradient (accent, 0.0f, m.getY(), yellow, 0.0f, m.getBottom(), false));
                g.fillRoundedRectangle (m.withHeight (m.getHeight() * fill), 3.0f);
            }
            g.setColour (border);
            g.drawRoundedRectangle (m.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (dim.withAlpha (0.7f));
            for (int db = 6; db < 18; db += 6)
                g.drawHorizontalLine ((int) (m.getY() + m.getHeight() * (float) db / 18.0f * (18.0f / full)), m.getX() - 3.0f, m.getX());
            g.setColour (v > 0.1f ? yellow : dim);
            g.setFont (mono (9.0f));
            g.drawText (juce::String (v, 1), p->meterBounds.getX() - 10, p->meterBounds.getBottom() + 2, p->meterBounds.getWidth() + 20, 12, juce::Justification::centred);
            g.setColour (dim);
            g.setFont (font (8.5f, true));
            g.drawText ("GR dB", p->meterBounds.getX() - 10, p->meterBounds.getY() - 12, p->meterBounds.getWidth() + 20, 11, juce::Justification::centred);
        }
    }
}

void BuiltinEditor::paintOverChildren (juce::Graphics& g)
{
    if (! bypass.load()) return;
    g.setColour (bg.withAlpha (0.55f));
    g.fillRect (getLocalBounds().withTrimmedTop (headerArea.getHeight()));
    auto pill = getLocalBounds().withTrimmedTop (headerArea.getHeight()).toFloat().withSizeKeepingCentre (170.0f, 30.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (pill, 15.0f);
    g.setColour (yellow);
    g.drawRoundedRectangle (pill.reduced (0.5f), 15.0f, 1.0f);
    g.setFont (font (12.0f, true));
    g.drawText ("EFFETTO ESCLUSO", pill.toNearestInt(), juce::Justification::centred);
}

void BuiltinEditor::paintCurve (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (bg);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
    auto plot = r.reduced (34.0f, 14.0f);
    const float f0 = std::log (20.0f), f1 = std::log (20000.0f), dbMin = -36.0f, dbMax = 6.0f;
    auto xOf = [&] (float hz) { return plot.getX() + plot.getWidth() * (std::log (hz) - f0) / (f1 - f0); };
    auto yOf = [&] (float db) { return plot.getY() + plot.getHeight() * (dbMax - juce::jlimit (dbMin, dbMax, db)) / (dbMax - dbMin); };

    g.setFont (font (9.0f, true));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        g.setColour (grid);
        g.drawVerticalLine ((int) xOf (hz), plot.getY(), plot.getBottom());
        g.setColour (dim);
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz), (int) xOf (hz) - 15, (int) plot.getBottom() + 1, 30, 11, juce::Justification::centred);
    }
    for (float db : { 0.0f, -12.0f, -24.0f })
    {
        g.setColour (db == 0.0f ? border.brighter (0.2f) : grid);
        g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
        g.setColour (dim);
        g.drawText (juce::String ((int) db), (int) r.getX() + 4, (int) yOf (db) - 6, 26, 12, juce::Justification::centredRight);
    }

    juce::Path path;
    for (int i = 0; i <= 200; ++i)
    {
        const float hz = std::exp (f0 + (f1 - f0) * (float) i / 200.0f);
        const float px = plot.getX() + plot.getWidth() * (float) i / 200.0f, py = yOf (fx.filterResponseDb (hz));
        if (i == 0) path.startNewSubPath (px, py); else path.lineTo (px, py);
    }
    juce::Path fill (path);
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.lineTo (plot.getX(), plot.getBottom());
    fill.closeSubPath();
    g.setColour (kindColour (fx.getKind()).withAlpha (0.18f));
    g.fillPath (fill);
    g.setColour (kindColour (fx.getKind()));
    g.strokePath (path, juce::PathStrokeType (2.0f));
}

//==============================================================================
PluginBrowser::PluginBrowser (juce::Array<juce::File> vst3Files, Callbacks callbacks) : cb (std::move (callbacks))
{
    for (int k = 0; k < BuiltinFx::numKinds; ++k)
    {
        const auto kind = (BuiltinFx::Kind) k;
        Entry e;
        e.builtin = true;
        e.kind = kind;
        e.title = BuiltinFx::kindName (kind);
        e.desc = BuiltinFx::kindHelp (kind);
        e.tag = BuiltinFx::kindCategory (kind);
        e.badge = BuiltinFx::kindBadge (kind);
        e.colour = kindColour (kind);
        all.push_back (e);
    }
    for (auto& f : vst3Files)
    {
        Entry e;
        e.file = f;
        e.title = f.getFileNameWithoutExtension();
        e.desc = f.getParentDirectory().getFullPathName();
        e.tag = "VST3";
        e.badge = "V3";
        e.colour = juce::Colour (0xff5b6472);
        all.push_back (e);
    }

    search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Cerca per nome o categoria (es. riverbero, dinamica, EQ)"), dim);
    search.setFont (font (14.0f));
    search.onTextChange = [this] { applyFilter(); };
    search.onReturnKey = [this] { choose (list.getSelectedRow()); };
    search.onEscapeKey = [this] { closeDialog(); };
    search.onKey = [this] (const juce::KeyPress& k)
    {
        const int dir = k == juce::KeyPress::downKey ? 1 : k == juce::KeyPress::upKey ? -1 : 0;
        if (dir == 0) return false;
        const int next = juce::jlimit (0, juce::jmax (0, (int) shown.size() - 1), list.getSelectedRow() + dir);
        list.selectRow (next);
        return true;
    };
    addAndMakeVisible (search);

    for (int i = 0; i < 3; ++i)
    {
        tabs[i].setClickingTogglesState (true);
        tabs[i].setRadioGroupId (7);
        tabs[i].setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 2 ? juce::Button::ConnectedOnRight : 0));
        tabs[i].onClick = [this, i] { tab = i; applyFilter(); };
        addAndMakeVisible (tabs[i]);
    }
    tabs[0].setToggleState (true, juce::dontSendNotification);

    list.setRowHeight (56);
    list.setColour (juce::ListBox::backgroundColourId, bg);
    list.setColour (juce::ListBox::outlineColourId, border);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    addButton.onClick = [this] { choose (list.getSelectedRow()); };
    cancelButton.onClick = [this] { closeDialog(); };
    browseButton.setTooltip ("Scegli un file .vst3 da una cartella qualsiasi");
    browseButton.onClick = [this]
    {
        auto callback = cb.browse;
        closeDialog();
        if (callback) callback();
    };
    addAndMakeVisible (addButton);
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (browseButton);

    applyFilter();
    juce::Component::SafePointer<PluginBrowser> safe (this);
    juce::Timer::callAfterDelay (150, [safe] { if (safe != nullptr) safe->search.grabKeyboardFocus(); });   // la finestra deve essere gia' attiva
}

void PluginBrowser::applyFilter()
{
    const auto terms = juce::StringArray::fromTokens (search.getText().trim(), " ", "");
    auto matches = [&] (const Entry& e)
    {
        if (tab == 1 && ! e.builtin) return false;
        if (tab == 2 && e.builtin) return false;
        for (auto& t : terms)
            if (! (e.title.containsIgnoreCase (t) || e.desc.containsIgnoreCase (t) || e.tag.containsIgnoreCase (t))) return false;
        return true;
    };

    shown.clear();
    for (int pass = 0; pass < 2; ++pass)   // prima gli integrati, poi i VST3
    {
        std::vector<Entry> part;
        for (auto& e : all) if (e.builtin == (pass == 0) && matches (e)) part.push_back (e);
        if (part.empty()) continue;
        Entry h;
        h.header = true;
        h.title = (pass == 0 ? juce::String ("Effetti integrati") : juce::String ("Plug-in VST3 installati")) + "  (" + juce::String ((int) part.size()) + ")";
        shown.push_back (h);
        for (auto& e : part) shown.push_back (e);
    }
    list.updateContent();
    list.repaint();
    lastRow = -1;
    for (int i = 0; i < (int) shown.size(); ++i)
        if (! shown[(size_t) i].header) { list.selectRow (i); break; }
    repaint();
}

void PluginBrowser::selectedRowsChanged (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size())) return;
    if (shown[(size_t) row].header)   // le intestazioni non si selezionano: si salta nella direzione del movimento
    {
        const int dir = row >= lastRow ? 1 : -1;
        int next = row + dir;
        if (! juce::isPositiveAndBelow (next, (int) shown.size())) next = row - dir;
        if (juce::isPositiveAndBelow (next, (int) shown.size()) && ! shown[(size_t) next].header) list.selectRow (next);
        return;
    }
    lastRow = row;
}

void PluginBrowser::choose (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size()) || shown[(size_t) row].header) return;
    const auto e = shown[(size_t) row];
    auto callbacks = cb;
    closeDialog();
    if (e.builtin) { if (callbacks.builtin) callbacks.builtin (e.kind); }
    else if (callbacks.vst3) callbacks.vst3 (e.file);
}

void PluginBrowser::closeDialog()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
}

void PluginBrowser::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size())) return;
    const auto& e = shown[(size_t) row];
    if (e.header)
    {
        g.setColour (panelTop);
        g.fillRect (0, 0, w, h);
        drawCaption (g, e.title, { 14, h - 22, w - 28, 18 });
        return;
    }
    auto r = juce::Rectangle<float> (4.0f, 2.0f, (float) w - 8.0f, (float) h - 4.0f);
    if (selected)
    {
        g.setColour (blue.withAlpha (0.28f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (blue.withAlpha (0.7f));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
    }
    auto badge = juce::Rectangle<float> (r.getX() + 10.0f, r.getCentreY() - 17.0f, 34.0f, 34.0f);
    g.setColour (e.colour.withAlpha (0.9f));
    g.fillRoundedRectangle (badge, 8.0f);
    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.setFont (font (13.0f, true));
    g.drawText (e.badge, badge.toNearestInt(), juce::Justification::centred);

    const int tagW = 84;
    g.setColour (text);
    g.setFont (font (14.0f, true));
    g.drawText (e.title, (int) badge.getRight() + 12, (int) r.getY() + 7, w - (int) badge.getRight() - 12 - tagW - 16, 20, juce::Justification::centredLeft, true);
    g.setColour (dim);
    g.setFont (font (11.5f));
    g.drawText (e.desc, (int) badge.getRight() + 12, (int) r.getY() + 28, w - (int) badge.getRight() - 12 - 16, 18, juce::Justification::centredLeft, true);

    auto tag = juce::Rectangle<float> (r.getRight() - (float) tagW - 8.0f, r.getY() + 9.0f, (float) tagW, 17.0f);
    g.setColour (e.colour.withAlpha (0.18f));
    g.fillRoundedRectangle (tag, 8.5f);
    g.setColour (e.builtin ? e.colour.brighter (0.3f) : dim);
    g.setFont (font (10.0f, true));
    g.drawText (e.tag.toUpperCase(), tag.toNearestInt(), juce::Justification::centred);
}

void PluginBrowser::paint (juce::Graphics& g)
{
    g.fillAll (panel);
    bool any = false;
    for (auto& e : shown) any = any || ! e.header;
    if (any) return;
    g.setColour (dim);
    g.setFont (font (13.0f));
    const bool noVst = tab == 2 && search.getText().isEmpty();
    g.drawFittedText (noVst ? juce::String ("Nessun plug-in VST3 trovato nelle cartelle standard.\nUsa \"Sfoglia...\" per sceglierne uno.") : juce::String ("Nessun risultato."),
                      list.getBounds().reduced (30), juce::Justification::centred, 3);
}

void PluginBrowser::resized()
{
    auto r = getLocalBounds().reduced (16);
    search.setBounds (r.removeFromTop (34));
    r.removeFromTop (10);
    auto tabRow = r.removeFromTop (28);
    const int tw = 86;
    for (int i = 0; i < 3; ++i) tabs[i].setBounds (tabRow.removeFromLeft (tw));
    r.removeFromTop (10);
    auto bottom = r.removeFromBottom (36);
    r.removeFromBottom (10);
    list.setBounds (r);
    browseButton.setBounds (bottom.removeFromLeft (110));
    addButton.setBounds (bottom.removeFromRight (120));
    bottom.removeFromRight (8);
    cancelButton.setBounds (bottom.removeFromRight (100));
}
} // namespace fc
