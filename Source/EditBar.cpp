#include "EditBar.h"

namespace fc
{
using namespace theme;

namespace { constexpr double kMinSpp = 0.05, kMaxSpp = 100000.0; }   // estremi dello zoom (campioni per pixel)

void ToolButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const bool on = getToggleState();
    juce::Colour base = on ? blue : panelHi;
    if (over && ! down) base = base.brighter (0.10f);
    const auto r = draw3dButton (g, getLocalBounds().toFloat(), base, 6.0f, on, down, blue.brighter (0.5f));

    const auto ir = r.withSizeKeepingCentre (r.getHeight() * 0.56f, r.getHeight() * 0.56f);
    const auto ic = on ? juce::Colours::white : theme::text;
    g.setColour (ic);
    auto P = [&ir] (float x, float y) { return juce::Point<float> (ir.getX() + x * ir.getWidth(), ir.getY() + y * ir.getHeight()); };

    juce::Path p;
    switch (tool)
    {
        case Tool::Select:
            p.startNewSubPath (P (0.18f, 0.04f)); p.lineTo (P (0.18f, 0.86f)); p.lineTo (P (0.36f, 0.68f));
            p.lineTo (P (0.50f, 0.98f)); p.lineTo (P (0.63f, 0.92f)); p.lineTo (P (0.49f, 0.63f)); p.lineTo (P (0.76f, 0.63f));
            p.closeSubPath();
            g.fillPath (p);
            break;

        case Tool::Razor:
            g.fillRect (juce::Rectangle<float> (P (0.47f, 0.0f).x, ir.getY(), 2.0f, ir.getHeight()));
            p.addTriangle (P (0.02f, 0.22f).x, P (0.02f, 0.22f).y, P (0.02f, 0.78f).x, P (0.02f, 0.78f).y, P (0.36f, 0.5f).x, P (0.36f, 0.5f).y);
            p.addTriangle (P (0.98f, 0.22f).x, P (0.98f, 0.22f).y, P (0.98f, 0.78f).x, P (0.98f, 0.78f).y, P (0.64f, 0.5f).x, P (0.64f, 0.5f).y);
            g.fillPath (p);
            break;

        case Tool::Slip:
            g.fillRect (juce::Rectangle<float> (ir.getX(), ir.getY() + ir.getHeight() * 0.12f, 2.0f, ir.getHeight() * 0.76f));
            g.fillRect (juce::Rectangle<float> (ir.getRight() - 2.0f, ir.getY() + ir.getHeight() * 0.12f, 2.0f, ir.getHeight() * 0.76f));
            p.addTriangle (P (0.16f, 0.5f).x, P (0.16f, 0.5f).y, P (0.38f, 0.28f).x, P (0.38f, 0.28f).y, P (0.38f, 0.72f).x, P (0.38f, 0.72f).y);
            p.addTriangle (P (0.84f, 0.5f).x, P (0.84f, 0.5f).y, P (0.62f, 0.28f).x, P (0.62f, 0.28f).y, P (0.62f, 0.72f).x, P (0.62f, 0.72f).y);
            g.fillPath (p);
            g.fillRect (juce::Rectangle<float> (P (0.36f, 0.0f).x, ir.getCentreY() - 1.0f, ir.getWidth() * 0.28f, 2.0f));
            break;

        case Tool::Stretch:
        {
            // due barre con una freccia doppia e un'onda al centro
            g.fillRect (juce::Rectangle<float> (ir.getX(), ir.getY() + ir.getHeight() * 0.08f, 2.0f, ir.getHeight() * 0.84f));
            g.fillRect (juce::Rectangle<float> (ir.getRight() - 2.0f, ir.getY() + ir.getHeight() * 0.08f, 2.0f, ir.getHeight() * 0.84f));
            juce::Path w;
            w.startNewSubPath (P (0.18f, 0.5f));
            for (int i = 1; i <= 24; ++i)
            {
                const float t = (float) i / 24.0f;
                w.lineTo (P (0.18f + 0.64f * t, 0.5f - 0.30f * std::sin (t * juce::MathConstants<float>::twoPi * 1.5f) * (0.35f + 0.65f * std::sin (t * juce::MathConstants<float>::pi))));
            }
            g.strokePath (w, juce::PathStrokeType (1.6f));
            p.addTriangle (P (0.04f, 0.5f).x, P (0.04f, 0.5f).y, P (0.16f, 0.38f).x, P (0.16f, 0.38f).y, P (0.16f, 0.62f).x, P (0.16f, 0.62f).y);
            p.addTriangle (P (0.96f, 0.5f).x, P (0.96f, 0.5f).y, P (0.84f, 0.38f).x, P (0.84f, 0.38f).y, P (0.84f, 0.62f).x, P (0.84f, 0.62f).y);
            g.fillPath (p);
            break;
        }

        case Tool::Hand:
        {
            const float fw = ir.getWidth() * 0.16f;
            const float tops[] = { 0.18f, 0.04f, 0.0f, 0.1f };
            for (int i = 0; i < 4; ++i)
                g.fillRoundedRectangle (ir.getX() + ir.getWidth() * (0.14f + 0.20f * (float) i), ir.getY() + ir.getHeight() * tops[i],
                                        fw, ir.getHeight() * 0.62f, fw * 0.5f);
            g.fillRoundedRectangle (ir.getX() + ir.getWidth() * 0.08f, ir.getY() + ir.getHeight() * 0.46f, ir.getWidth() * 0.84f, ir.getHeight() * 0.54f, ir.getWidth() * 0.22f);
            break;
        }

        case Tool::Automation:
        {
            const juce::Point<float> pts[] = { P (0.04f, 0.78f), P (0.36f, 0.28f), P (0.68f, 0.62f), P (0.96f, 0.14f) };
            juce::Path line;
            line.startNewSubPath (pts[0]);
            for (int i = 1; i < 4; ++i) line.lineTo (pts[i]);
            g.strokePath (line, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
            for (auto& q : pts) g.fillEllipse (q.x - 2.2f, q.y - 2.2f, 4.4f, 4.4f);
            break;
        }

        case Tool::Range:
        {
            // intervallo tratteggiato con due parentesi
            juce::Path box;
            box.addRectangle (juce::Rectangle<float> (P (0.22f, 0.20f), P (0.78f, 0.80f)));
            const float dashes[] = { 3.0f, 2.5f };
            juce::Path dashed;
            juce::PathStrokeType (1.4f).createDashedStroke (dashed, box, dashes, 2);
            g.fillPath (dashed);
            g.fillRect (juce::Rectangle<float> (P (0.04f, 0.10f).x, P (0.04f, 0.10f).y, 2.0f, ir.getHeight() * 0.80f));
            g.fillRect (juce::Rectangle<float> (P (0.96f, 0.10f).x - 2.0f, P (0.96f, 0.10f).y, 2.0f, ir.getHeight() * 0.80f));
            break;
        }

        case Tool::Mute:
            g.setFont (font (ir.getHeight() * 1.05f, true));
            g.drawText ("M", ir.toNearestInt(), juce::Justification::centred);
            g.drawLine (P (0.05f, 0.95f).x, P (0.05f, 0.95f).y, P (0.95f, 0.05f).x, P (0.95f, 0.05f).y, 1.6f);
            break;

        case Tool::Erase:
        {
            juce::Path e;
            e.startNewSubPath (P (0.30f, 0.10f)); e.lineTo (P (0.95f, 0.10f)); e.lineTo (P (0.70f, 0.62f)); e.lineTo (P (0.05f, 0.62f)); e.closeSubPath();
            g.strokePath (e, juce::PathStrokeType (1.6f));
            p.startNewSubPath (P (0.18f, 0.36f)); p.lineTo (P (0.83f, 0.36f)); p.lineTo (P (0.70f, 0.62f)); p.lineTo (P (0.05f, 0.62f)); p.closeSubPath();
            g.fillPath (p);
            g.fillRect (juce::Rectangle<float> (P (0.05f, 0.80f).x, P (0.05f, 0.80f).y, ir.getWidth() * 0.9f, 2.0f));
            break;
        }

        case Tool::Comp:
        {
            // tre corsie di take: il pezzo scelto in quella di mezzo
            const float ys[] = { 0.10f, 0.42f, 0.74f };
            for (int i = 0; i < 3; ++i)
                g.drawRect (juce::Rectangle<float> (P (0.04f, ys[i]).x, P (0.04f, ys[i]).y, ir.getWidth() * 0.92f, ir.getHeight() * 0.22f), 1.2f);
            g.fillRect (juce::Rectangle<float> (P (0.30f, 0.42f).x, P (0.30f, 0.42f).y, ir.getWidth() * 0.40f, ir.getHeight() * 0.22f));
            g.fillRect (juce::Rectangle<float> (P (0.04f, 0.10f).x, P (0.04f, 0.10f).y, ir.getWidth() * 0.26f, ir.getHeight() * 0.22f));
            g.fillRect (juce::Rectangle<float> (P (0.70f, 0.74f).x, P (0.70f, 0.74f).y, ir.getWidth() * 0.26f, ir.getHeight() * 0.22f));
            break;
        }

        case Tool::Warp:
        {
            // onda con un punto trascinato: la linea si piega
            g.drawLine (P (0.05f, 0.5f).x, P (0.05f, 0.5f).y, P (0.38f, 0.5f).x, P (0.38f, 0.5f).y, 1.6f);
            g.drawLine (P (0.38f, 0.5f).x, P (0.38f, 0.5f).y, P (0.62f, 0.15f).x, P (0.62f, 0.15f).y, 1.6f);
            g.drawLine (P (0.62f, 0.15f).x, P (0.62f, 0.15f).y, P (0.95f, 0.15f).x, P (0.95f, 0.15f).y, 1.6f);
            g.fillEllipse (P (0.38f, 0.5f).x - 3.5f, P (0.38f, 0.5f).y - 3.5f, 7.0f, 7.0f);
            g.fillEllipse (P (0.62f, 0.15f).x - 3.5f, P (0.62f, 0.15f).y - 3.5f, 7.0f, 7.0f);
            g.fillRect (juce::Rectangle<float> (P (0.05f, 0.85f).x, P (0.05f, 0.85f).y, ir.getWidth() * 0.9f, 2.0f));
            break;
        }

        case Tool::Envelope:
        {
            // linea spezzata con due punti
            g.drawLine (P (0.04f, 0.35f).x, P (0.04f, 0.35f).y, P (0.34f, 0.35f).x, P (0.34f, 0.35f).y, 1.6f);
            g.drawLine (P (0.34f, 0.35f).x, P (0.34f, 0.35f).y, P (0.64f, 0.8f).x, P (0.64f, 0.8f).y, 1.6f);
            g.drawLine (P (0.64f, 0.8f).x, P (0.64f, 0.8f).y, P (0.96f, 0.8f).x, P (0.96f, 0.8f).y, 1.6f);
            g.fillEllipse (P (0.34f, 0.35f).x - 3.5f, P (0.34f, 0.35f).y - 3.5f, 7.0f, 7.0f);
            g.fillEllipse (P (0.64f, 0.8f).x - 3.5f, P (0.64f, 0.8f).y - 3.5f, 7.0f, 7.0f);
            break;
        }

        case Tool::Scrub:
        {
            // testina con le onde del suono che esce
            g.fillRect (juce::Rectangle<float> (P (0.48f, 0.0f).x, ir.getY(), 2.0f, ir.getHeight()));
            p.addTriangle (P (0.30f, 0.0f).x, P (0.30f, 0.0f).y, P (0.70f, 0.0f).x, P (0.70f, 0.0f).y, P (0.5f, 0.22f).x, P (0.5f, 0.22f).y);
            g.fillPath (p);
            for (float k : { 0.14f, 0.26f })
            {
                g.drawLine (P (0.5f + k, 0.45f).x, P (0.5f + k, 0.45f).y, P (0.5f + k, 0.75f).x, P (0.5f + k, 0.75f).y, 1.4f);
                g.drawLine (P (0.5f - k, 0.45f).x, P (0.5f - k, 0.45f).y, P (0.5f - k, 0.75f).x, P (0.5f - k, 0.75f).y, 1.4f);
            }
            break;
        }

        case Tool::Zoom:
        {
            const auto c = P (0.42f, 0.42f);
            const float rad = ir.getWidth() * 0.34f;
            g.drawEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f, 2.0f);
            g.drawLine (P (0.66f, 0.66f).x, P (0.66f, 0.66f).y, P (0.97f, 0.97f).x, P (0.97f, 0.97f).y, 2.6f);
            g.fillRect (c.x - rad * 0.5f, c.y - 0.9f, rad, 1.8f);
            g.fillRect (c.x - 0.9f, c.y - rad * 0.5f, 1.8f, rad);
            break;
        }
    }
}

//==============================================================================
EditBar::EditBar (juce::ApplicationCommandManager& cm) : commands (cm)
{
    struct T { Tool t; const char* tip; };
    const T tools[] = {
        { Tool::Select, TRX ("Selezione (V): seleziona, sposta e rifila i clip. Cmd + trascina (Ctrl su Windows) su una traccia: seleziona la porzione e la mette in loop") },
        { Tool::Razor,  TRX ("Lama (B): clic su un clip per dividerlo. Maiusc: divide tutte le tracce") },
        { Tool::Slip,   TRX ("Slip (Y): trascina per scorrere il contenuto dentro il clip senza spostarlo") },
        { Tool::Stretch, TRX ("Stretch (E): trascina il bordo di un clip per allungarlo o accorciarlo; l'audio si adatta senza cambiare il tono e il bordo si aggancia ai cue") },
        { Tool::Hand,   TRX ("Mano (H): trascina per spostare la vista") },
        { Tool::Zoom,   TRX ("Zoom (Z): clic per ingrandire, Alt + clic per ridurre, trascina per zoomare su un'area") },
        { Tool::Range,  TRX ("Range (X): trascina per scegliere un intervallo di tempo su una o piu' tracce. Poi Modifica > Range: dividi, elimina, rimuovi (chiude il vuoto), tieni solo il range") },
        { Tool::Mute,   TRX ("Mute (K): clic su un clip per silenziarlo o riattivarlo (su un take ADR lo rende quello attivo)") },
        { Tool::Erase,  TRX ("Gomma (G): clic su un clip per eliminarlo. In modalita' Ripple o Shuffle il vuoto si chiude") },
        { Tool::Comp,   TRX ("Comp (C): su un clip di una traccia con piu' take sovrapposti trascina un intervallo: quel take diventa quello che suona li', gli altri si silenziano") },
        { Tool::Warp,   TRX ("Time Warp (O): clic su un clip per mettere un punto sulla parola, trascinalo dove deve arrivare. Invio applica (il resto si adatta senza cambiare il tono), Esc annulla, Alt + clic toglie un punto") },
        { Tool::Envelope, TRX ("Inviluppo del clip (D): clic su un clip per mettere un punto di guadagno (in alto +12 dB, la riga chiara e' 0 dB), trascina per spostarlo, Alt + clic lo toglie. Serve per abbassare un respiro o un colpo di vento solo in quel punto") },
        { Tool::Scrub,  TRX ("Scrub (J): trascina sulle tracce per ascoltare a scatti la posizione, come muovendo il nastro a mano. Anche Alt + trascina sul righello") },
        { Tool::Automation, TRX ("Automazione (U): clic su una traccia per aggiungere un punto alla curva mostrata (VOL, PAN, SEND A o SEND B: si cambia dal pulsante nell'intestazione della traccia), trascina per spostarlo, clic destro per eliminarlo") }
    };
    for (auto& t : tools)
    {
        auto b = std::make_unique<ToolButton> (t.t);
        b->setTooltip (juce::translate (t.tip));
        b->setRadioGroupId (4711);
        b->setWantsKeyboardFocus (false);
        b->onClick = [this, tool = t.t] { if (onToolSelected) onToolSelected (tool); };
        addAndMakeVisible (*b);
        toolButtons.push_back (std::move (b));
    }
    toolButtons.front()->setToggleState (true, juce::dontSendNotification);

    struct A { const char* label; int w; int cmd; const char* tip; };
    const A acts[] = {
        { TRX ("DIVIDI"),    62, cmdSplit,        TRX ("Divide i clip alla testina (S)") },
        { TRX ("DUPLICA"),   68, cmdDuplicate,    TRX ("Duplica i clip selezionati (Cmd + D)") },
        { TRX ("ELIMINA"),   68, cmdDelete,       TRX ("Elimina i clip selezionati (Canc)") },
        { TRX ("RIPPLE"),    62, cmdRippleDelete, TRX ("Elimina i clip selezionati chiudendo il vuoto sulla traccia (Maiusc + Canc)") },
        { TRX ("TAGLIA IN"), 76, cmdTrimStart,    TRX ("Taglia l'inizio dei clip alla testina (Q)") },
        { TRX ("TAGLIA OUT"), 84, cmdTrimEnd,     TRX ("Taglia la fine dei clip alla testina (W)") },
        { TRX ("NORMALE"),   82, cmdEditMode,     TRX ("Modalita' di modifica: Normale, Ripple (eliminare chiude il vuoto) o Shuffle (anche gli spostamenti: i clip vicini scivolano). Clic per cambiare") }
    };
    for (auto& a : acts)
    {
        auto b = std::make_unique<juce::TextButton> (juce::translate (a.label));
        b->setTooltip (juce::translate (a.tip));
        b->setWantsKeyboardFocus (false);
        b->onClick = [this, id = a.cmd] { commands.invokeDirectly ((juce::CommandID) id, true); };
        addAndMakeVisible (*b);
        if (a.cmd == cmdEditMode) modeButton = b.get();
        actions.push_back ({ std::move (b), a.w });
    }

    heightSlider.setRange (48.0, 240.0, 1.0);
    heightSlider.setValue (96.0, juce::dontSendNotification);
    heightSlider.setTooltip (TRANS ("Altezza delle tracce (si puo' anche trascinare il bordo di ogni traccia)"));
    heightSlider.onValueChange = [this] { if (onHeightChanged) onHeightChanged ((int) heightSlider.getValue()); };
    addAndMakeVisible (heightSlider);

    zoomSlider.setRange (0.0, 1.0, 0.0);
    zoomSlider.setValue (0.5, juce::dontSendNotification);
    zoomSlider.setTooltip (TRANS ("Zoom orizzontale della timeline (anche Cmd + rotella, pizzico sul trackpad, tasti + e -)"));
    zoomSlider.onValueChange = [this] { if (onZoomChanged) onZoomChanged (std::exp (std::log (kMaxSpp) + (std::log (kMinSpp) - std::log (kMaxSpp)) * zoomSlider.getValue())); };
    addAndMakeVisible (zoomSlider);

    fitBtn.setTooltip (TRANS ("Adatta l'altezza delle tracce alla finestra (Maiusc + F)"));
    fitBtn.setWantsKeyboardFocus (false);
    fitBtn.onClick = [this] { if (onFitHeights) onFitHeights(); };
    addAndMakeVisible (fitBtn);
}

void EditBar::setEditModeLabel (const juce::String& label)
{
    if (modeButton != nullptr) modeButton->setButtonText (label);
}

void EditBar::setTool (Tool t)
{
    for (auto& b : toolButtons) b->setToggleState (b->getTool() == t, juce::dontSendNotification);
}

void EditBar::setHeightValue (int h)
{
    heightSlider.setValue ((double) h, juce::dontSendNotification);
}

void EditBar::setZoomValue (double spp)
{
    const double v = (std::log (kMaxSpp) - std::log (juce::jlimit (kMinSpp, kMaxSpp, spp))) / (std::log (kMaxSpp) - std::log (kMinSpp));
    if (std::abs (zoomSlider.getValue() - v) > 1.0e-4) zoomSlider.setValue (v, juce::dontSendNotification);
}

void EditBar::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, 0.0f, panel, 0.0f, (float) getHeight(), false));
    g.fillRect (getLocalBounds());
    g.setColour (border);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.setColour (border);
    g.fillRect (separatorX, 8, 1, getHeight() - 16);
    if (sliderCaptionX > 0) drawCaption (g, TRANS ("Altezza"), juce::Rectangle<int> (sliderCaptionX, 0, 50, getHeight()));
    if (zoomCaptionX > 0) drawCaption (g, TRANS ("Zoom"), juce::Rectangle<int> (zoomCaptionX, 0, 40, getHeight()));
}

void EditBar::resized()
{
    auto r = getLocalBounds().reduced (10, 5);
    for (auto& b : toolButtons)
    {
        b->setBounds (r.removeFromLeft (32));
        r.removeFromLeft (4);
    }
    r.removeFromLeft (6);
    separatorX = r.getX();
    r.removeFromLeft (12);

    // gruppo di destra (altezza tracce), solo se c'e' spazio
    sliderCaptionX = zoomCaptionX = 0;
    const bool roomRight = r.getWidth() > 960;
    heightSlider.setVisible (roomRight);
    zoomSlider.setVisible (roomRight);
    fitBtn.setVisible (roomRight);
    if (roomRight)
    {
        fitBtn.setBounds (r.removeFromRight (64));
        r.removeFromRight (8);
        heightSlider.setBounds (r.removeFromRight (120));
        r.removeFromRight (6);
        sliderCaptionX = r.getRight() - 50;
        r.removeFromRight (56);
        r.removeFromRight (14);
        zoomSlider.setBounds (r.removeFromRight (120));
        r.removeFromRight (6);
        zoomCaptionX = r.getRight() - 36;
        r.removeFromRight (42);
    }

    for (auto& a : actions)
    {
        if (r.getWidth() >= a.width) { a.button->setVisible (true); a.button->setBounds (r.removeFromLeft (a.width)); r.removeFromLeft (6); }
        else a.button->setVisible (false);
    }
}

} // namespace fc
