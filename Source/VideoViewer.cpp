#include "VideoViewer.h"
#include "VideoSupport.h"

namespace fc
{
using namespace theme;

VideoViewer::VideoViewer()
{
    addChildComponent (video);

    for (auto* b : { &detachBtn, &fullBtn, &minusFrame, &minusMs, &plusMs, &plusFrame, &resetBtn })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    detachBtn.setTooltip ("Apre il video in una finestra separata (spostabile su un altro schermo)");
    fullBtn.setTooltip ("Video a tutto schermo (Esc per uscire)");
    minusFrame.setTooltip ("Anticipa il video di un fotogramma");
    plusFrame.setTooltip ("Ritarda il video di un fotogramma");
    minusMs.setTooltip ("Anticipa il video di 1 ms (tieni premuto per ripetere)");
    plusMs.setTooltip ("Ritarda il video di 1 ms (tieni premuto per ripetere)");
    resetBtn.setTooltip ("Azzera l'offset del video");
    for (auto* b : { &minusFrame, &minusMs, &plusMs, &plusFrame }) b->setRepeatSpeed (450, 45);

    detachBtn.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
    fullBtn.onClick = [this] { if (onFullscreenToggle) onFullscreenToggle(); };
    minusMs.onClick = [this] { nudge (-0.001); };
    plusMs.onClick = [this] { nudge (0.001); };
    minusFrame.onClick = [this] { if (asset) nudge (-1.0 / juce::jmax (1.0, asset->fps)); };
    plusFrame.onClick = [this] { if (asset) nudge (1.0 / juce::jmax (1.0, asset->fps)); };
    resetBtn.onClick = [this] { if (asset && onOffsetChanged) { onOffsetChanged (0.0); refreshOffset(); } };

    offsetLabel.setEditable (false, true, false);
    offsetLabel.setJustificationType (juce::Justification::centred);
    offsetLabel.setFont (mono (13.0f));
    offsetLabel.setColour (juce::Label::backgroundColourId, well);
    offsetLabel.setColour (juce::Label::outlineColourId, border);
    offsetLabel.setColour (juce::Label::textColourId, text);
    offsetLabel.setTooltip ("Offset del video in millisecondi: positivo = video in ritardo rispetto all'audio. Doppio clic per scrivere un valore.");
    offsetLabel.onTextChange = [this] { commitLabel(); };
    addAndMakeVisible (offsetLabel);
    refreshOffset();
}

VideoViewer::~VideoViewer()
{
    video.closeVideo();
}

juce::String VideoViewer::formatTimecode (double seconds, double fps)
{
    const int f = juce::jmax (1, (int) std::lround (fps));
    const auto total = (int64_t) std::floor (juce::jmax (0.0, seconds) * f + 1.0e-6);
    const int ff = (int) (total % f);
    const int64_t secs = total / f;
    return juce::String::formatted ("%02d:%02d:%02d:%02d", (int) (secs / 3600), (int) ((secs / 60) % 60), (int) (secs % 60), ff);
}

void VideoViewer::setDetached (bool d)
{
    detached = d;
    detachBtn.setButtonText (d ? "AGGANCIA" : "STACCA");
    detachBtn.setTooltip (d ? "Riporta il video nella finestra principale" : "Apre il video in una finestra separata (spostabile su un altro schermo)");
    repaint();
}

void VideoViewer::refreshOffset()
{
    const double ms = asset != nullptr ? asset->offsetSec * 1000.0 : 0.0;
    const auto v = (int) std::lround (ms);
    offsetLabel.setText ((v > 0 ? "+" : "") + juce::String (v) + " ms", juce::dontSendNotification);
    const bool en = asset != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &minusFrame, &minusMs, &plusMs, &plusFrame, &resetBtn, &offsetLabel })
        c->setEnabled (en);
}

void VideoViewer::nudge (double seconds)
{
    if (asset == nullptr || ! onOffsetChanged) return;
    onOffsetChanged (asset->offsetSec + seconds);
    refreshOffset();
}

void VideoViewer::commitLabel()
{
    if (asset == nullptr || ! onOffsetChanged) return;
    const auto t = offsetLabel.getText().retainCharacters ("-+0123456789.,").replaceCharacter (',', '.');
    onOffsetChanged (t.getDoubleValue() / 1000.0);
    refreshOffset();
}

void VideoViewer::setVideo (std::shared_ptr<VideoAsset> a)
{
    const bool sameFile = asset != nullptr && a != nullptr && asset->file == a->file;
    asset = std::move (a);
    refreshOffset();
    if (sameFile) { repaint(); return; }

    loaded = false;
    running = false;
    lastSetPos = -1.0;
    video.setVisible (false);
    video.closeVideo();
    if (asset == nullptr) { repaint(); return; }

    juce::Component::SafePointer<VideoViewer> safe (this);
    video.loadAsync (juce::URL (asset->file), [safe] (const juce::URL&, juce::Result r)
    {
        if (safe == nullptr || ! r.wasOk()) return;
        safe->video.setAudioVolume (0.0f);   // l'audio lo suona il motore di ADR Dub Studio
        safe->loaded = true;
        safe->video.setVisible (true);
        safe->resized();
        safe->video.setPlayPosition (0.0);
    });
    repaint();
}

juce::Rectangle<int> VideoViewer::pictureArea() const
{
    auto area = getLocalBounds().withTrimmedTop (topH).withTrimmedBottom (bottomH + adrH()).reduced (10, 8);
    if (asset == nullptr || asset->width <= 0 || asset->height <= 0) return area;
    const double ar = (double) asset->width / (double) asset->height;
    int w = area.getWidth(), h = (int) std::round (w / ar);
    if (h > area.getHeight()) { h = area.getHeight(); w = (int) std::round (h * ar); }
    return juce::Rectangle<int> (juce::jmax (1, w), juce::jmax (1, h)).withCentre (area.getCentre());
}

void VideoViewer::resized()
{
    video.setBounds (pictureArea());

    auto top = getLocalBounds().removeFromTop (topH).reduced (8, 5);
    fullBtn.setBounds (top.removeFromRight (112));
    top.removeFromRight (6);
    detachBtn.setBounds (top.removeFromRight (86));

    auto bar = getLocalBounds().removeFromBottom (bottomH).reduced (12, 0).withTrimmedTop (22).withTrimmedBottom (8);
    // controlli offset a destra: [-F][-][ valore ][+][+F][0]
    resetBtn.setBounds (bar.removeFromRight (30));
    bar.removeFromRight (6);
    plusFrame.setBounds (bar.removeFromRight (34));
    plusMs.setBounds (bar.removeFromRight (30));
    offsetLabel.setBounds (bar.removeFromRight (84));
    minusMs.setBounds (bar.removeFromRight (30));
    minusFrame.setBounds (bar.removeFromRight (34));
}

void VideoViewer::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (asset != nullptr && pictureArea().contains (e.getPosition()) && onFullscreenToggle) onFullscreenToggle();
}

void VideoViewer::sync (double t, bool playing)
{
    curT = t;
    if (adr.active) repaint (adrStrip());
    if (asset == nullptr) return;

    const double dur = asset->duration;
    const double frame = 1.0 / juce::jmax (1.0, asset->fps);
    const double vt = t - asset->offsetSec;                       // tempo all'interno del video
    const double tc = juce::jlimit (0.0, juce::jmax (0.0, dur - frame), vt);

    if (std::abs (t - lastShown) > 1.0e-4)
    {
        lastShown = t;
        timecodeText = tcFormatter ? tcFormatter (juce::jmax (0.0, t)) : formatTimecode (juce::jmax (0.0, t), asset->fps);
        repaint (getLocalBounds().removeFromBottom (bottomH));
    }
    if (! loaded) return;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (playing && vt >= 0.0 && vt < dur)
    {
        if (! running)
        {
            // il lettore parte con un ritardo (decodifica del primo fotogramma): si parte un po' avanti, e il ritardo
            // misurato la prima volta serve a partire meglio le volte dopo
            video.setPlaySpeed (1.0);
            video.setPlayPosition (juce::jmin (tc + startLead, juce::jmax (0.0, dur - frame)));
            video.play();
            running = true;
            lastSpeed = 1.0;
            measuredStart = false;
            holdoffUntil = now + 0.25;
        }
        else if (now > holdoffUntil)
        {
            // l'audio comanda: la differenza si recupera accelerando o rallentando di poco il video (senza salti, che con i
            // video pesanti sono lenti e si vedono come scatti); solo se e' enorme si riposiziona
            const double err = video.getPlayPosition() - vt - displayLead;   // > 0: il video e' avanti
            if (! measuredStart) { measuredStart = true; startLead = juce::jlimit (0.0, 0.5, startLead - 0.7 * err); }
            if (std::abs (err) > 0.6)
            {
                video.setPlayPosition (tc + startLead);
                holdoffUntil = now + 0.8;
                lastSpeed = 1.0;
                fc::video::setNativePlayerRate (video, 1.0);
            }
            else
            {
                double speed = 1.0;
                if (std::abs (err) > 0.008) speed = 1.0 - juce::jlimit (-0.06, 0.06, err * 1.2);
                if (std::abs (speed - lastSpeed) > 0.004) { fc::video::setNativePlayerRate (video, speed); lastSpeed = speed; }
            }
        }
    }
    else
    {
        if (running)
        {
            lastSpeed = 1.0;
            video.stop();
            running = false;
            lastSetPos = -1.0;
        }
        if (std::abs (tc - lastSetPos) > 0.0005)
        {
            video.setPlayPosition (tc);
            lastSetPos = tc;
        }
    }
}

void VideoViewer::paint (juce::Graphics& g)
{
    g.fillAll (well);

    // barra superiore
    auto top = getLocalBounds().removeFromTop (topH);
    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, 0.0f, panel, 0.0f, (float) topH, false));
    g.fillRect (top);
    g.setColour (border);
    g.drawHorizontalLine (topH - 1, 0.0f, (float) getWidth());
    g.setColour (hilite);
    g.fillRoundedRectangle (12.0f, 11.0f, 4.0f, 12.0f, 2.0f);
    g.setColour (text);
    g.setFont (font (12.0f, true));
    g.drawText ("VIDEO", 24, 0, 60, topH, juce::Justification::centredLeft);
    if (asset != nullptr)
    {
        g.setColour (dim);
        g.setFont (font (11.0f));
        g.drawText (asset->file.getFileName() + "   " + juce::String (asset->width) + "x" + juce::String (asset->height)
                        + "   " + juce::String (asset->fps, 3) + " fps",
                    78, 0, getWidth() - 78 - 212, topH, juce::Justification::centredLeft, true);
    }

    // area immagine con cornice
    auto pic = pictureArea();
    if (asset != nullptr)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (pic.expanded (1));
        g.setColour (border);
        g.drawRect (pic.expanded (1));
        if (! loaded)
        {
            g.setColour (dim);
            g.setFont (font (13.0f));
            g.drawText ("Caricamento video...", pic, juce::Justification::centred);
        }
    }
    else
    {
        g.setColour (dim);
        g.setFont (font (13.0f));
        g.drawText ("Nessun video", pic, juce::Justification::centred);
    }

    if (adr.active)
    {
        auto st = adrStrip();
        g.setColour (well);
        g.fillRect (st);
        g.setColour (adr.recording ? accent : border);
        g.drawHorizontalLine (st.getY(), 0.0f, (float) getWidth());
        auto inner = st.reduced (12, 6);
        g.setColour (adr.recording ? accent : juce::Colour (0xffffc94d));
        g.setFont (font (12.0f, true));
        g.drawText (adr.title, inner.removeFromTop (16), juce::Justification::centredLeft, true);
        // streamer: nei 3 secondi prima dell'ingresso una linea attraversa la striscia, poi la barra si riempie fino all'uscita
        auto bar = inner.removeFromBottom (8).toFloat();
        g.setColour (well);
        g.fillRoundedRectangle (bar, 3.0f);
        if (adr.streamer)
        {
            if (curT >= adr.inSec - 3.0 && curT < adr.inSec)
            {
                const float x = bar.getX() + bar.getWidth() * (float) ((curT - (adr.inSec - 3.0)) / 3.0);
                g.setColour (juce::Colours::white);
                g.fillRect (x - 1.5f, bar.getY() - 2.0f, 3.0f, bar.getHeight() + 4.0f);
            }
            else if (curT >= adr.inSec && curT <= adr.outSec && adr.outSec > adr.inSec)
            {
                g.setColour ((adr.recording ? accent : green).withAlpha (0.85f));
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) ((curT - adr.inSec) / (adr.outSec - adr.inSec))), 3.0f);
            }
        }
        g.setColour (text);
        // dialogo al centro; se e' troppo lungo per lo spazio il carattere si riduce da solo
        const auto area = inner.withTrimmedTop (4);
        const auto txt = adr.dialogue.replaceCharacters ("\r\n", "  ");
        const auto key = txt + "|" + juce::String (adr.dialogueSize) + "|" + juce::String (area.getWidth()) + "x" + juce::String (area.getHeight());
        if (key != dialogueKey)
        {
            dialogueKey = key;
            for (float size = (float) adr.dialogueSize;; size -= 1.0f)
            {
                juce::AttributedString as;
                as.setJustification (juce::Justification::centred);
                as.setWordWrap (juce::AttributedString::byWord);
                as.append (txt, font (size), text);
                dialogueLayout.createLayout (as, (float) area.getWidth());
                if (dialogueLayout.getHeight() <= (float) area.getHeight() || size <= 10.0f) break;
            }
        }
        dialogueLayout.draw (g, area.toFloat().withTrimmedTop (juce::jmax (0.0f, ((float) area.getHeight() - dialogueLayout.getHeight()) * 0.5f)));
    }

    // barra inferiore
    auto bar = getLocalBounds().removeFromBottom (bottomH);
    g.setGradientFill (juce::ColourGradient (panel, 0.0f, (float) bar.getY(), panelTop, 0.0f, (float) bar.getBottom(), false));
    g.fillRect (bar);
    g.setColour (border);
    g.drawHorizontalLine (bar.getY(), 0.0f, (float) getWidth());

    g.setColour (well);
    g.fillRoundedRectangle (juce::Rectangle<float> (12.0f, (float) bar.getY() + 10.0f, 150.0f, (float) bar.getHeight() - 20.0f), 6.0f);
    g.setColour (playhead);
    g.setFont (mono (18.0f));
    g.drawText (timecodeText.isEmpty() ? "00:00:00:00" : timecodeText, 12, bar.getY() + 10, 150, bar.getHeight() - 20, juce::Justification::centred);

    drawCaption (g, "Offset video", juce::Rectangle<int> (getWidth() - 12 - 244, bar.getY() + 6, 244, 12), juce::Justification::centredRight);
}

} // namespace fc
