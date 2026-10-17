#include "SpectrumView.h"
#include <complex>

namespace fc
{
using namespace theme;

SpectrumView::SpectrumView (Reader r, std::function<double()> sr) : read (std::move (r)), rate (std::move (sr))
{
    samples.assign ((size_t) kSize * 2, 0.0f);
    bins.assign ((size_t) kSize / 2 + 1, kMinDb);
    disp.assign ((size_t) kSize / 2 + 1, kMinDb);

    tiltBtn.setClickingTogglesState (true);
    tiltBtn.setColour (juce::TextButton::buttonOnColourId, blue);
    tiltBtn.setTooltip (TRANS ("Inclinazione +4,5 dB/ottava (come il rumore rosa appare piatto): rende piu' leggibili le alte frequenze"));
    tiltBtn.onClick = [this] { tilt = tiltBtn.getToggleState(); };
    peakBtn.setClickingTogglesState (true);
    peakBtn.setToggleState (true, juce::dontSendNotification);
    peakBtn.setColour (juce::TextButton::buttonOnColourId, blue);
    peakBtn.setTooltip (TRANS ("Mostra i picchi mantenuti"));
    peakBtn.onClick = [this] { showPeaks = peakBtn.getToggleState(); };
    addAndMakeVisible (tiltBtn);
    addAndMakeVisible (peakBtn);
}

void SpectrumView::visibilityChanged()
{
    if (isShowing()) startTimerHz (30); else stopTimer();
}

void SpectrumView::resized()
{
    auto r = getLocalBounds().withTrimmedBottom (8).removeFromBottom (24).reduced (12, 0);
    tiltBtn.setBounds (r.removeFromLeft (56));
    r.removeFromLeft (6);
    peakBtn.setBounds (r.removeFromLeft (72));
    hold.assign ((size_t) juce::jmax (1, (int) plot().getWidth() + 1), kMinDb);
    holdAge.assign (hold.size(), 0);
}

float SpectrumView::freqToX (float hz) const
{
    const auto p = plot();
    return p.getX() + p.getWidth() * std::log (juce::jmax (20.0f, hz) / 20.0f) / std::log (1000.0f);
}

float SpectrumView::xToFreq (float x) const
{
    const auto p = plot();
    return 20.0f * std::exp (juce::jlimit (0.0f, 1.0f, (x - p.getX()) / p.getWidth()) * std::log (1000.0f));
}

void SpectrumView::magnitudesDb (const float* in, std::vector<float>& out)
{
    static const std::vector<float> window = []
    {
        std::vector<float> w ((size_t) kSize);
        for (int i = 0; i < kSize; ++i) w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kSize);
        return w;
    }();
    static const std::vector<std::complex<float>> twiddle = []
    {
        std::vector<std::complex<float>> t ((size_t) kSize / 2);
        for (size_t k = 0; k < t.size(); ++k) t[k] = std::polar (1.0f, -juce::MathConstants<float>::twoPi * (float) k / (float) kSize);
        return t;
    }();

    std::vector<std::complex<float>> x ((size_t) kSize);
    for (int i = 0; i < kSize; ++i) x[(size_t) i] = in[i] * window[(size_t) i];

    // FFT radix-2 iterativa (riordino a bit invertiti, poi farfalle)
    for (unsigned i = 1, j = 0; i < (unsigned) kSize; ++i)
    {
        unsigned bit = (unsigned) kSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (x[i], x[j]);
    }
    for (size_t len = 2; len <= (size_t) kSize; len <<= 1)
        for (size_t i = 0; i < (size_t) kSize; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = x[i + k], v = x[i + k + len / 2] * twiddle[k * ((size_t) kSize / len)];
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
            }

    out.resize ((size_t) kSize / 2 + 1);
    const float norm = 4.0f / (float) kSize;   // finestra di Hann: somma = N/2, e meta' dell'ampiezza sta nel bin negativo
    for (size_t i = 0; i < out.size(); ++i) out[i] = juce::Decibels::gainToDecibels (std::abs (x[i]) * norm, kMinDb);
}

void SpectrumView::timerCallback()
{
    read (samples.data(), kSize);
    magnitudesDb (samples.data(), bins);

    const double sr = juce::jmax (8000.0, rate());
    const float hzPerBin = (float) sr / (float) kSize;
    for (size_t i = 0; i < bins.size(); ++i)
    {
        float v = bins[i];
        if (tilt) v += 4.5f * std::log2 (juce::jmax (1.0f, (float) i * hzPerBin) / 1000.0f);
        disp[i] = v > disp[i] ? v : disp[i] * 0.82f + v * 0.18f;
    }

    // una colonna per pixel: massimo dei bin compresi nell'intervallo, interpolazione sotto un bin
    const auto p = plot();
    const int cols = (int) p.getWidth();
    if ((int) hold.size() != cols + 1) { hold.assign ((size_t) cols + 1, kMinDb); holdAge.assign (hold.size(), 0); }
    line.clear();
    fill.clear();
    colDb.assign ((size_t) cols + 1, kMinDb);
    peakDb = kMinDb;
    for (int x = 0; x <= cols; ++x)
    {
        const float f0 = xToFreq (p.getX() + (float) x - 0.5f), f1 = xToFreq (p.getX() + (float) x + 0.5f);
        const float b0 = f0 / hzPerBin, b1 = f1 / hzPerBin;
        float v;
        if (b1 - b0 < 1.0f)
        {
            const float c = juce::jlimit (0.0f, (float) disp.size() - 1.001f, 0.5f * (b0 + b1));
            const int i = (int) c;
            v = disp[(size_t) i] + (disp[(size_t) i + 1] - disp[(size_t) i]) * (c - (float) i);
        }
        else
        {
            v = kMinDb;
            for (int i = juce::jmax (1, (int) std::ceil (b0)); i <= juce::jmin ((int) disp.size() - 1, (int) std::floor (b1)); ++i) v = juce::jmax (v, disp[(size_t) i]);
        }
        colDb[(size_t) x] = v;
        const float y = p.getBottom() - p.getHeight() * juce::jlimit (0.0f, 1.0f, (v - kMinDb) / (kMaxDb - kMinDb));
        if (x == 0) { line.startNewSubPath (p.getX(), y); fill.startNewSubPath (p.getX(), p.getBottom()); fill.lineTo (p.getX(), y); }
        else { line.lineTo (p.getX() + (float) x, y); fill.lineTo (p.getX() + (float) x, y); }
        if (v > peakDb) { peakDb = v; peakHz = 0.5f * (f0 + f1); }

        auto& h = hold[(size_t) x];
        auto& age = holdAge[(size_t) x];
        if (v >= h) { h = v; age = 0; }
        else if (++age > 45) h = juce::jmax (kMinDb, h - 0.6f);
    }
    fill.lineTo (p.getRight(), p.getBottom());
    fill.closeSubPath();
    repaint();
}

void SpectrumView::paint (juce::Graphics& g)
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

    const auto p = plot();
    g.setColour (well);
    g.fillRect (p);

    // griglia: frequenze e livelli
    g.setFont (mono (9.5f));
    for (float hz : { 30.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
    {
        const bool major = hz == 100.f || hz == 1000.f || hz == 10000.f;
        const float x = freqToX (hz);
        g.setColour (grid.withAlpha (major ? 0.9f : 0.5f));
        g.drawVerticalLine ((int) x, p.getY(), p.getBottom());
        if (major || hz == 50.f || hz == 500.f || hz == 5000.f)
        {
            g.setColour (dim);
            g.drawText (hz >= 1000.f ? juce::String ((int) (hz / 1000.f)) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (x - 18.0f, p.getBottom() + 1.0f, 36.0f, 12.0f).toNearestInt(), juce::Justification::centred);
        }
    }
    for (int db = -80; db <= 0; db += 20)
    {
        const float y = p.getBottom() - p.getHeight() * (float) (db - kMinDb) / (kMaxDb - kMinDb);
        g.setColour (grid.withAlpha (0.7f));
        g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
        g.setColour (dim);
        g.drawText (juce::String (db), juce::Rectangle<int> ((int) b.getX() + 2, (int) y - 6, 22, 12), juce::Justification::centredRight);
    }

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (p.toNearestInt());
        g.setGradientFill (juce::ColourGradient (blue.withAlpha (0.55f), 0.0f, p.getY(), blue.withAlpha (0.05f), 0.0f, p.getBottom(), false));
        g.fillPath (fill);
        g.setColour (hilite);
        g.strokePath (line, juce::PathStrokeType (1.3f));

        if (showPeaks)
        {
            juce::Path pk;
            for (size_t x = 0; x < hold.size(); ++x)
            {
                const float y = p.getBottom() - p.getHeight() * juce::jlimit (0.0f, 1.0f, (hold[x] - kMinDb) / (kMaxDb - kMinDb));
                if (x == 0) pk.startNewSubPath (p.getX(), y); else pk.lineTo (p.getX() + (float) x, y);
            }
            g.setColour (yellow.withAlpha (0.8f));
            g.strokePath (pk, juce::PathStrokeType (1.0f));
        }
    }

    // lettura: picco dello spettro, oppure il punto sotto il mouse
    g.setFont (mono (11.0f));
    g.setColour (text);
    auto fmtHz = [] (float hz) { return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String ((int) std::round (hz)) + " Hz"; };
    juce::String info;
    if (hoverX >= 0 && p.contains ((float) hoverX, p.getCentreY()))
    {
        const int col = juce::jlimit (0, (int) colDb.size() - 1, hoverX - (int) p.getX());
        info = fmtHz (xToFreq ((float) hoverX)) + (colDb.empty() ? juce::String() : "  " + juce::String (colDb[(size_t) col], 1) + " dB");
        g.setColour (dim);
        g.drawVerticalLine (hoverX, p.getY(), p.getBottom());
    }
    else if (peakDb > kMinDb + 1.0f)
        info = TRANS ("picco ") + fmtHz (peakHz) + "  " + juce::String (peakDb, 1) + " dB";
    g.setColour (text);
    g.drawText (info, getLocalBounds().withTrimmedTop (34).removeFromTop (16).reduced (12, 0).withTrimmedLeft (20), juce::Justification::centredRight);
}

} // namespace fc
