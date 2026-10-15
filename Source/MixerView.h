#pragma once

#include "Model.h"
#include "Loudness.h"
#include "PluginHost.h"
#include "SpectrumView.h"
#include "EqEditor.h"
#include "Theme.h"

namespace fc
{
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevels (float l, float r);
    void paint (juce::Graphics&) override;
    /** Mostra la scala numerica (dB) a destra delle barre: serve una larghezza di almeno 36 px. */
    void setShowScale (bool on) { showScale = on; repaint(); }
    static constexpr int scaleWidth = 20, readoutHeight = 15;
    /** Azzera il picco massimo mostrato. */
    void resetPeak() { peakMaxDb = -80.0f; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { resetPeak(); }

private:
    static float toPos (float lin);
    float dispDb[2] { -80.0f, -80.0f }, holdDb[2] { -80.0f, -80.0f };
    int holdCount[2] { 0, 0 };
    bool showScale = false;
    float peakMaxDb = -80.0f;   // picco massimo dall'ultimo azzeramento
};

/** Una riga della pila degli inserti plug-in. */
class InsertButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void (const juce::MouseEvent&)> onMouse;

    void setSlot (std::shared_ptr<InsertSlot> s) { slot = std::move (s); repaint(); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { if (onMouse) onMouse (e); }

private:
    std::shared_ptr<InsertSlot> slot;
};

class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (Project&, PluginHost&, std::shared_ptr<TrackLive>, int trackId, bool isMaster, bool isAux = false);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    void setInfo (const juce::String& n, juce::Colour c, bool selected);
    void syncFromLive();
    void updateMeter();
    /** Mostra il valore dell'automazione sul fader (senza modificarlo). */
    void showAutomatedVolume (float db);
    void showAutomatedPan (float pan);
    void showAutomatedSend (int k, float db);
    void resetPeak() { meter.resetPeak(); }
    const std::shared_ptr<TrackLive>& getLive() const noexcept { return live; }

    std::function<void()> onEq;   // apre l'equalizzatore della traccia
    int trackId;

private:
    void refreshInserts();
    void onInsert (int index, const juce::MouseEvent&);

    Project& project;
    PluginHost& host;
    std::shared_ptr<TrackLive> live;
    bool master;
    bool aux;
    juce::String name;
    juce::Colour colour;
    bool selected = false;

    juce::Slider pan { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::TextButton muteBtn { "M" }, soloBtn { "S" }, invertBtn { juce::String (juce::CharPointer_UTF8 ("\xc3\x98")) };
    juce::TextButton monoBtn { "MONO" }, dimBtn { "DIM" };
    juce::Label dbLabel;
    juce::TextButton eqBtn { "EQ" }, trackLimBtn { "LIM" }, trackLimMenuBtn { juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe")) }, routeBtn;   // uscita della traccia: master o bus di gruppo
    LevelMeter meter;
    juce::Rectangle<int> panLabel, ceilLabel, grRect, sendCaptionA, sendCaptionB;
    juce::Slider sendA { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox },
                 sendB { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    std::array<InsertButton, 4> inserts;

    // limiter (solo master)
    juce::TextButton limBtn { "LIMITER" };
    juce::Slider ceil { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    float grDisp = 0.0f, grHold = 0.0f;   // riduzione di guadagno del limiter mostrata e picco mantenuto (dB)
    bool limActive = false;
};

/** Pannello di misura del loudness (LUFS) con scelta dello standard di riferimento. */
class LoudnessPanel : public juce::Component, private juce::Timer
{
public:
    LoudnessPanel (Project&, Loudness::Meter&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    Project& project;
    Loudness::Meter& meter;
    juce::ComboBox standard;
    juce::TextButton resetBtn { "AZZERA" };
    float mom = -100.0f, st = -100.0f, integ = -100.0f, lra = 0.0f, tpDb = -100.0f;
    juce::Rectangle<int> bigBox, barBox;
};

class MixerView : public juce::Component, public juce::ChangeListener, public juce::Timer
{
public:
    static constexpr int stripW = 108;

    MixerView (Project&, PluginHost&, Loudness::Meter&, SpectrumView::Reader, std::function<double()> sampleRate);
    ~MixerView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    /** Azzera il picco massimo mostrato su tutti i canali. */
    void resetPeaks();

private:
    void rebuild();

    Project& project;
    PluginHost& host;
    LoudnessPanel loudnessPanel;
    SpectrumView spectrum;
    juce::TextButton tabLoud { "LOUDNESS" }, tabSpec { "SPETTRO" };   // schede del pannello di analisi
    void showAnalysis (bool spec);
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    std::unique_ptr<ChannelStrip> masterStrip;
    std::unique_ptr<ChannelStrip> auxStrips[2];
    std::unique_ptr<ChannelStrip> groupStrips[Session::kNumGroups];   // a destra delle tracce, nell'area scorrevole
    void makeAux (int k);
    void openEq (int trackId);
    std::vector<std::pair<std::shared_ptr<TrackLive>, std::unique_ptr<EqWindow>>> eqWindows;
    void makeGroup (int k);
};

} // namespace fc
