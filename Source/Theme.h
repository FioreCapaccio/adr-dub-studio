#pragma once

#include <JuceHeader.h>

namespace fc::theme
{
// superfici
inline const juce::Colour bg        { 0xff0e0f12 };
inline const juce::Colour panel     { 0xff16181c };
inline const juce::Colour panelHi   { 0xff22252b };
inline const juce::Colour panelTop  { 0xff1c1f24 };
inline const juce::Colour lane      { 0xff131519 };
inline const juce::Colour laneAlt   { 0xff171a1e };
inline const juce::Colour grid      { 0xff262930 };
inline const juce::Colour border    { 0xff2b2f37 };
// testo
inline const juce::Colour text      { 0xffe9ebef };
inline const juce::Colour dim       { 0xff848b97 };
// accenti: il rosso e' riservato a registrazione e testina, il blu agli stati attivi
inline const juce::Colour accent    { 0xffff5a47 };
inline const juce::Colour blue      { 0xff3d8bfd };
inline const juce::Colour playhead  { 0xffff4136 };
inline const juce::Colour green     { 0xff4cd27a };
inline const juce::Colour yellow    { 0xffffc94d };
inline const juce::Colour muteCol   { 0xff3d8bfd };
inline const juce::Colour soloCol   { 0xffffc94d };

inline const juce::Colour trackPalette[] = {
    juce::Colour (0xffe5584c), juce::Colour (0xffe9944a), juce::Colour (0xffe7c44a), juce::Colour (0xff6fbf73),
    juce::Colour (0xff4aa8c9), juce::Colour (0xff6f7be0), juce::Colour (0xffb36fd6), juce::Colour (0xffd66fa3)
};
constexpr int numPaletteColours = 8;

inline juce::Font font (float h, bool bold = false)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font mono (float h)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::bold));
}

/** Etichetta in maiuscolo piccolo, per titoli di sezione. */
inline void drawCaption (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                         juce::Justification j = juce::Justification::centredLeft, juce::Colour c = dim)
{
    g.setColour (c);
    g.setFont (font (10.0f, true));
    g.drawText (s.toUpperCase(), r, j, true);
}

//==============================================================================
/** Pulsante con icona vettoriale (trasporto). */
/** Tasto in rilievo: ombra sotto, corpo a gradiente con riflesso in alto e bordo luminoso; premuto diventa incassato.
    'bounds' e' l'area del componente: il corpo lascia 2 px in basso per l'ombra. Ritorna l'area del corpo (dove stanno testo e icone). */
inline juce::Rectangle<float> draw3dButton (juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour base, float radius, bool on, bool down, juce::Colour edgeOn = juce::Colours::transparentBlack)
{
    auto body = bounds.reduced (1.0f, 1.0f);
    body.removeFromBottom (2.0f);
    if (down) body = body.translated (0.0f, 1.0f);

    if (! down)   // ombra proiettata
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (body.translated (0.0f, 2.0f), radius);
    }

    const auto top = down ? base.darker (0.30f) : base.brighter (on ? 0.20f : 0.26f);
    const auto bottom = down ? base.brighter (0.04f) : base.darker (0.30f);
    g.setGradientFill (juce::ColourGradient (top, 0.0f, body.getY(), bottom, 0.0f, body.getBottom(), false));
    g.fillRoundedRectangle (body, radius);

    if (! down)   // riflesso sulla meta' alta
    {
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.12f), 0.0f, body.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), 0.0f, body.getCentreY(), false));
        g.fillRoundedRectangle (body.reduced (1.0f, 1.0f).withHeight (body.getHeight() * 0.5f), radius - 1.0f);
    }

    // bordo luminoso in alto (luce dall'alto), scuro in basso
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (down ? 0.05f : 0.34f), 0.0f, body.getY(),
                                             juce::Colours::black.withAlpha (down ? 0.5f : 0.35f), 0.0f, body.getBottom(), false));
    g.drawRoundedRectangle (body.reduced (0.5f), radius, 1.0f);
    if (on && ! edgeOn.isTransparent())
    {
        g.setColour (edgeOn.withAlpha (0.55f));
        g.drawRoundedRectangle (body.reduced (1.5f), radius - 1.0f, 1.0f);
    }
    return body;
}

class IconButton : public juce::Button
{
public:
    enum class Icon { Rewind, Play, Stop, Record };

    IconButton (Icon i, juce::Colour activeColour) : juce::Button ({}), icon (i), active (activeColour) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();

        juce::Colour base = on ? active.darker (0.45f) : panelHi;
        if (over && ! down) base = base.brighter (0.10f);
        r = draw3dButton (g, getLocalBounds().toFloat(), base, 7.0f, on, down, active);

        auto ir = r.withSizeKeepingCentre (r.getHeight() * 0.38f, r.getHeight() * 0.38f);
        juce::Colour ic = theme::text;
        if (icon == Icon::Record) ic = on ? juce::Colours::white : accent;
        else if (icon == Icon::Play) ic = on ? active.brighter (0.5f) : theme::text;
        g.setColour (ic.withAlpha (isEnabled() ? 1.0f : 0.4f));

        juce::Path p;
        switch (icon)
        {
            case Icon::Play:
                p.addTriangle (ir.getX() + ir.getWidth() * 0.1f, ir.getY(), ir.getX() + ir.getWidth() * 0.1f, ir.getBottom(),
                               ir.getRight() + ir.getWidth() * 0.1f, ir.getCentreY());
                break;
            case Icon::Stop:  p.addRoundedRectangle (ir, 1.5f); break;
            case Icon::Record: p.addEllipse (ir.expanded (ir.getWidth() * 0.05f)); break;
            case Icon::Rewind:
                p.addRectangle (ir.getX() - 1.0f, ir.getY(), ir.getWidth() * 0.18f, ir.getHeight());
                p.addTriangle (ir.getRight(), ir.getY(), ir.getRight(), ir.getBottom(), ir.getX() + ir.getWidth() * 0.28f, ir.getCentreY());
                break;
        }
        g.fillPath (p);
    }

private:
    Icon icon;
    juce::Colour active;
};

//==============================================================================
class StudioLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StudioLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, bg);
        setColour (juce::TextButton::buttonColourId, panelHi);
        setColour (juce::TextButton::buttonOnColourId, blue);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::Label::textColourId, text);
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1f2227));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, blue);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::AlertWindow::backgroundColourId, juce::Colour (0xff1f2227));
        setColour (juce::AlertWindow::textColourId, text);
        setColour (juce::AlertWindow::outlineColourId, border);
        setColour (juce::TextEditor::backgroundColourId, bg);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::outlineColourId, border);
        setColour (juce::TextEditor::focusedOutlineColourId, blue);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff4a505b));
        setColour (juce::DocumentWindow::backgroundColourId, bg);
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff2a2e35));
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, border);
    }

    // --- pulsanti ---------------------------------------------------------
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        const bool on = b.getToggleState();
        juce::Colour base = on ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
        if (! b.isEnabled()) base = base.withMultipliedSaturation (0.4f).darker (0.2f);
        else if (over && ! down) base = base.brighter (0.10f);
        draw3dButton (g, b.getLocalBounds().toFloat(), base, 6.0f, on, down && b.isEnabled(), base.brighter (0.5f));
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                         .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        auto area = b.getLocalBounds().reduced (3, 0).withTrimmedBottom (2);
        if (down) area.translate (0, 1);
        g.drawFittedText (b.getButtonText(), area, juce::Justification::centred, 1);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return font (juce::jmin (12.0f, (float) buttonHeight * 0.36f), true);
    }

    // --- barre di scorrimento ----------------------------------------------
    int getDefaultScrollbarWidth() override { return 12; }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override
    {
        g.setColour (juce::Colour (0xff0b0c0e));
        g.fillRect (x, y, w, h);
        juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float> ((float) x + 3.0f, (float) thumbStart, (float) w - 6.0f, (float) thumbSize)
                                                : juce::Rectangle<float> ((float) thumbStart, (float) y + 3.0f, (float) thumbSize, (float) h - 6.0f);
        g.setColour (juce::Colour (0xff4a505b).brighter (down ? 0.3f : over ? 0.15f : 0.0f));
        g.fillRoundedRectangle (thumb, 3.0f);
    }

    // --- fader e manopole ---------------------------------------------------
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minPos, float maxPos, juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minPos, maxPos, style, s);
            return;
        }

        const float cx = (float) x + (float) width * 0.5f;
        g.setColour (juce::Colour (0xff08090b));
        g.fillRoundedRectangle (cx - 3.0f, (float) y, 6.0f, (float) height, 3.0f);
        g.setColour (border);
        g.drawRoundedRectangle (cx - 3.0f, (float) y, 6.0f, (float) height, 3.0f, 1.0f);

        // parte "piena" del canale, dal fondo alla posizione del cursore
        g.setColour (blue.withAlpha (0.55f));
        g.fillRoundedRectangle (cx - 1.5f, sliderPos, 3.0f, (float) (y + height) - sliderPos, 1.5f);

        // per gli slider a un solo valore minPos/maxPos coincidono con sliderPos: la scala si ricava dalla geometria
        auto yOf = [&] (float db) { return (float) y + (1.0f - (float) s.valueToProportionOfLength ((double) db)) * (float) height; };

        // tacche minori (ogni dB nella zona utile, poi piu' rade)
        static const float minors[] = { 5, 4, 3, 2, 1, -1, -2, -3, -4, -6, -7, -8, -9, -12, -14, -16, -18, -25, -35, -45, -55, -70 };
        float lastY = -1000.0f;
        g.setColour (dim.withAlpha (0.45f));
        for (float db : minors)
        {
            const float ty = yOf (db);
            if (std::abs (ty - lastY) < 3.0f) continue;
            lastY = ty;
            g.drawHorizontalLine ((int) ty, cx - 8.0f, cx - 5.5f);
            g.drawHorizontalLine ((int) ty, cx + 5.5f, cx + 8.0f);
        }

        // tacche maggiori con etichetta: in ordine di priorita', saltando quelle troppo vicine
        struct Major { float db; const char* label; };
        static const Major majors[] = { { 0.0f, "0" }, { 6.0f, "+6" }, { -10.0f, "-10" }, { -20.0f, "-20" }, { -40.0f, "-40" }, { -5.0f, "-5" },
                                        { -30.0f, "-30" }, { -15.0f, "-15" }, { -60.0f, "-60" }, { -50.0f, "-50" } };
        std::vector<float> placed;
        g.setFont (font (9.0f, true));
        auto drawMajor = [&] (float ty, const juce::String& label, bool zero)
        {
            g.setColour (zero ? text.withAlpha (0.95f) : dim.withAlpha (0.85f));
            g.drawHorizontalLine ((int) ty, cx - 12.0f, cx - 5.5f);
            g.drawHorizontalLine ((int) ty, cx + 5.5f, cx + 12.0f);
            g.setColour (zero ? text : dim);
            g.drawText (label, (int) cx + 13, (int) ty - 6, 24, 12, juce::Justification::centredLeft);
        };
        const float bottomY = yOf (-80.0f);
        for (auto& m : majors)
        {
            const float ty = yOf (m.db);
            bool clash = std::abs (ty - bottomY) < 11.0f;
            for (float p : placed) clash = clash || std::abs (ty - p) < 11.0f;
            if (clash) continue;
            placed.push_back (ty);
            drawMajor (ty, m.label, m.db == 0.0f);
        }
        drawMajor (bottomY, juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")), false);

        // cursore
        const float capW = 32.0f, capH = 20.0f;
        juce::Rectangle<float> cap (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (cap.translated (0.0f, 1.5f), 4.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7a808c), cap.getX(), cap.getY(),
                                                 juce::Colour (0xff3b3f47), cap.getX(), cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (cap, 4.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        for (int i = -1; i <= 1; i += 2)
            g.drawHorizontalLine ((int) (cap.getCentreY () + (float) i * 4.5f), cap.getX() + 4.0f, cap.getRight() - 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 1.0f, cap.getWidth() - 6.0f, 2.0f);
    }

    /** Manopola graduata. Proprieta' opzionali dello Slider: "ticks" (numero di tacche, default 11), "majorEvery" (default 5),
        "lblMin" / "lblMax" (etichette agli estremi), "centered" (l'arco parte dal centro, come per il pan). */
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        auto full = juce::Rectangle<int> (x, y, width, height).toFloat();
        const int nTicks = juce::jmax (2, (int) s.getProperties().getWithDefault ("ticks", 11));
        const int majorEvery = juce::jmax (1, (int) s.getProperties().getWithDefault ("majorEvery", 5));
        const bool centered = (bool) s.getProperties().getWithDefault ("centered", false);
        const juce::String lblMin = s.getProperties().getWithDefault ("lblMin", "").toString();
        const juce::String lblMax = s.getProperties().getWithDefault ("lblMax", "").toString();

        const bool hasLabels = lblMin.isNotEmpty() || lblMax.isNotEmpty();
        auto knobArea = full.reduced (2.0f);
        if (hasLabels) knobArea.removeFromBottom (9.0f);
        const float R = juce::jmin (knobArea.getWidth(), knobArea.getHeight()) * 0.5f;
        const auto centre = knobArea.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float mid = startAngle + 0.5f * (endAngle - startAngle);
        const float lo = centered ? juce::jmin (mid, angle) : startAngle;
        const float hi = centered ? juce::jmax (mid, angle) : angle;

        // tacche
        for (int i = 0; i < nTicks; ++i)
        {
            const float a = startAngle + (float) i / (float) (nTicks - 1) * (endAngle - startAngle);
            const bool major = (i % majorEvery) == 0 || i == nTicks - 1;
            const bool lit = s.isEnabled() && a >= lo - 0.001f && a <= hi + 0.001f && (centered ? true : a <= angle + 0.001f);
            const float r1 = R - (major ? 5.0f : 3.0f), r2 = R;
            const float sa = std::sin (a), ca = -std::cos (a);   // 0 rad = in alto
            g.setColour (lit ? blue : (major ? dim : dim.withAlpha (0.55f)));
            g.drawLine (centre.x + r1 * sa, centre.y + r1 * ca, centre.x + r2 * sa, centre.y + r2 * ca, major ? 1.6f : 1.0f);
        }

        // corpo
        const float rBody = R - 8.0f;
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (centre.x - rBody, centre.y - rBody + 1.5f, rBody * 2.0f, rBody * 2.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3e46), centre.x, centre.y - rBody,
                                                 juce::Colour (0xff1a1c20), centre.x, centre.y + rBody, false));
        g.fillEllipse (centre.x - rBody, centre.y - rBody, rBody * 2.0f, rBody * 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (centre.x - rBody, centre.y - rBody, rBody * 2.0f, rBody * 2.0f, 1.0f);

        // indice
        juce::Path pointer;
        pointer.addRoundedRectangle (-1.3f, -rBody + 2.5f, 2.6f, rBody * 0.55f, 1.2f);
        g.setColour (s.isEnabled() ? juce::Colours::white : dim);
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));

        // etichette agli estremi
        if (hasLabels)
        {
            g.setFont (font (9.0f, true));
            g.setColour (dim);
            g.drawText (lblMin, juce::Rectangle<float> (full.getX(), full.getBottom() - 10.0f, full.getWidth() * 0.5f, 10.0f).toNearestInt(), juce::Justification::centredLeft);
            g.drawText (lblMax, juce::Rectangle<float> (full.getCentreX(), full.getBottom() - 10.0f, full.getWidth() * 0.5f, 10.0f).toNearestInt(), juce::Justification::centredRight);
        }
    }
};

} // namespace fc::theme
