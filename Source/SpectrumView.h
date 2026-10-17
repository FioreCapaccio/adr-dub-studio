#pragma once

#include "Theme.h"
#include <JuceHeader.h>
#include <functional>

namespace fc
{
/** Analizzatore di spettro in tempo reale (FFT 8192 punti, scala logaritmica 20 Hz - 20 kHz) con picchi mantenuti. */
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    /** Copia in dest gli ultimi n campioni (mono) dell'uscita master. */
    using Reader = std::function<void (float* dest, int n)>;
    static constexpr int kOrder = 13, kSize = 1 << kOrder;

    SpectrumView (Reader reader, std::function<double()> sampleRate);

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void mouseMove (const juce::MouseEvent& e) override { hoverX = e.x; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hoverX = -1; repaint(); }

    /** Calcola lo spettro (dB) di kSize campioni: bins[0..kSize/2], 0 dB = sinusoide a fondo scala. Usato anche dall'autotest. */
    static void magnitudesDb (const float* samples, std::vector<float>& bins);

private:
    void timerCallback() override;
    float freqToX (float hz) const;
    float xToFreq (float x) const;
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (10.0f, 0.0f).withTrimmedTop (36.0f).withTrimmedBottom (46.0f).withTrimmedLeft (22.0f); }

    Reader read;
    std::function<double()> rate;
    std::vector<float> samples, bins, disp;     // disp: valore mostrato per bin (dB), con rilascio graduale
    std::vector<float> colDb;                    // livello mostrato per colonna (per la lettura sotto il mouse)
    std::vector<float> hold;                     // picco mantenuto per colonna (dB)
    std::vector<int> holdAge;
    juce::Path line, fill;
    juce::TextButton tiltBtn { TRANS ("TILT") }, peakBtn { TRANS ("PICCHI") };
    float peakHz = 0.0f, peakDb = -120.0f;
    int hoverX = -1;
    bool tilt = false, showPeaks = true;
    static constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;
};

} // namespace fc
