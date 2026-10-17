#include "AudioTools.h"
#include "Loudness.h"

namespace fc::tools
{
namespace
{
/** Livello (dB RMS) di ogni finestra di 'hop' campioni, sul canale piu' forte di ciascuna. */
std::vector<float> levels (const juce::AudioBuffer<float>& a, int64_t offset, int64_t length, int hop)
{
    std::vector<float> out;
    const int nch = a.getNumChannels();
    for (int64_t i = 0; i < length; i += hop)
    {
        const int n = (int) juce::jmin<int64_t> (hop, length - i);
        float peakRms = 0.0f;
        for (int ch = 0; ch < nch; ++ch) peakRms = juce::jmax (peakRms, a.getRMSLevel (ch, (int) (offset + i), n));
        out.push_back (juce::Decibels::gainToDecibels (peakRms, -120.0f));
    }
    return out;
}
}

std::vector<std::pair<int64_t, int64_t>> soundRegions (const juce::AudioBuffer<float>& a, int64_t offset, int64_t length, double rate,
                                                       float thresholdDb, double minSilenceSec, double padSec)
{
    const int hop = juce::jmax (32, (int) (rate * 0.005));
    const auto lv = levels (a, offset, length, hop);
    const int minGap = juce::jmax (1, (int) std::ceil (minSilenceSec * rate / hop));
    const int pad = (int) std::llround (padSec * rate);

    std::vector<std::pair<int64_t, int64_t>> regions;
    int64_t start = -1, lastSound = -1;
    for (size_t i = 0; i < lv.size(); ++i)
    {
        if (lv[i] > thresholdDb)
        {
            if (start < 0) start = (int64_t) i;
            lastSound = (int64_t) i;
        }
        else if (start >= 0 && (int64_t) i - lastSound >= minGap)   // pausa abbastanza lunga: la zona finisce
        {
            regions.emplace_back (start * hop, (lastSound + 1) * hop);
            start = -1;
        }
    }
    if (start >= 0) regions.emplace_back (start * hop, juce::jmin<int64_t> (length, (lastSound + 1) * hop));

    // margini e unione delle zone che si toccano
    std::vector<std::pair<int64_t, int64_t>> out;
    for (auto& r : regions)
    {
        const int64_t a0 = juce::jmax<int64_t> (0, r.first - pad), a1 = juce::jmin<int64_t> (length, r.second + pad);
        if (! out.empty() && a0 <= out.back().second) out.back().second = juce::jmax (out.back().second, a1);
        else out.emplace_back (a0, a1);
    }
    return out;
}

double speechPercent (const juce::AudioBuffer<float>& a, int64_t offset, int64_t length, double rate)
{
    const int block = juce::jmax (64, (int) (rate * 0.4));
    const auto lv = levels (a, offset, length, block);
    if (lv.empty()) return 0.0;
    int speech = 0;
    for (auto v : lv) speech += v > -45.0f ? 1 : 0;   // sotto -45 dB RMS e' rumore di fondo o silenzio
    return 100.0 * speech / (double) lv.size();
}

double measureLufs (const juce::AudioBuffer<float>& a, int64_t offset, int64_t length, double rate, bool dialogueOnly)
{
    if (length <= 0) return -100.0;
    Loudness::Meter meter;
    meter.prepare (rate);
    const int block = juce::jmax (64, (int) (rate * 0.4));
    const int nch = a.getNumChannels();
    std::vector<float> l, r;
    for (int64_t i = 0; i < length; i += block)
    {
        const int n = (int) juce::jmin<int64_t> (block, length - i);
        if (dialogueOnly)
        {
            float rms = 0.0f;
            for (int ch = 0; ch < nch; ++ch) rms = juce::jmax (rms, a.getRMSLevel (ch, (int) (offset + i), n));
            if (juce::Decibels::gainToDecibels (rms, -120.0f) < -45.0f) continue;   // niente parlato in questo blocco
        }
        const float* pl = a.getReadPointer (0) + offset + i;
        const float* pr = a.getReadPointer (juce::jmin (1, nch - 1)) + offset + i;
        meter.process (pl, pr, n);
    }
    return (double) meter.integrated.load();
}
} // namespace fc::tools
