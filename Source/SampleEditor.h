#pragma once

#include "AudioEngine.h"
#include "Model.h"
#include "Theme.h"

namespace fc
{
/** Spettrogramma (STFT a 1024 punti, frequenze su scala logaritmica 60 Hz - 12 kHz) di una porzione di audio, come immagine larga 'width' colonne. */
juce::Image makeSpectrogram (const juce::AudioBuffer<float>& audio, int64_t from, int64_t length, double rate, int width, int height);

/** Sample Editor: un clip a tutto schermo con zoom, selezione di una porzione, spettrogramma ed elaborazioni sulla sola selezione. */
class SampleEditor : public juce::Component, private juce::ChangeListener, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    SampleEditor (Project&, AudioEngine&, int64_t clipId, std::function<void (juce::String)> showError);
    ~SampleEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

    int64_t getClipId() const noexcept { return clipId; }
    /** Selezione in campioni dall'inizio del clip (a < 0: nessuna). Usato anche dall'autotest. */
    void setSelection (int64_t a, int64_t b) { selA = a; selB = b; repaint(); }
    bool applyRange (Project::RangeOp, double param = 0.0);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { spectro = {}; findHits(); updateScroll(); repaint(); }
    void timerCallback() override { repaint(); }
    void scrollBarMoved (juce::ScrollBar*, double start) override { viewStart = start; repaint(); }
    juce::Rectangle<int> plot() const;
    int64_t xToPos (float x) const { return (int64_t) std::llround (viewStart + (x - (float) plot().getX()) * spp); }
    float posToX (int64_t p) const { return (float) plot().getX() + (float) (((double) p - viewStart) / spp); }
    void updateScroll();
    void zoomAt (float x, double factor);
    void listen();

    Project& project;
    AudioEngine& engine;
    int64_t clipId;
    std::function<void (juce::String)> showError;
    double spp = 100.0, viewStart = 0.0;
    int64_t selA = -1, selB = -1, anchor = 0;
    bool spectrogram = false, fitted = false;
    juce::Image spectro;
    double spectroSpp = 0.0, spectroStart = -1.0;
    juce::ScrollBar bar { false };
    juce::TextButton viewBtn { TRANS ("SPETTROGRAMMA") }, allBtn { TRANS ("TUTTO") }, listenBtn { TRANS ("ASCOLTA") }, fitBtn { TRANS ("ADATTA") },
                     gainBtn { TRANS ("GUADAGNO") }, normBtn { TRANS ("NORMALIZZA") }, fadeInBtn { TRANS ("FADE IN") }, fadeOutBtn { TRANS ("FADE OUT") },
                     hitBtn { TRANS ("ATTACCHI") }, sliceBtn { TRANS ("DIVIDI") },
                     silBtn { TRANS ("SILENZIO") }, revBtn { TRANS ("REVERSE") }, invBtn { TRANS ("INVERTI") }, dcBtn { TRANS ("TOGLI DC") };
    juce::TextEditor gainEd;
    juce::Slider hitSens { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::vector<int64_t> hits;   // punti di attacco (campioni dall'inizio del clip) quando sono mostrati
    void findHits();
    juce::Label hint;
};
} // namespace fc
