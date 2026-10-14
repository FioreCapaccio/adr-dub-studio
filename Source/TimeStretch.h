#pragma once

#include <JuceHeader.h>
#include <vector>

namespace fc
{
/** Cambia la durata di una regione di audio mantenendo il tono (WSOLA: sovrapposizione di finestre scelte per somiglianza d'onda).
    ratio = durata finale / durata originale (2 = il doppio piu' lento). Adatto a voce e dialoghi; i transienti forti possono perdere nitidezza. */
inline juce::AudioBuffer<float> timeStretch (const juce::AudioBuffer<float>& in, int start, int length, double ratio, double rate)
{
    const int nch = in.getNumChannels();
    const int outLen = juce::jmax (1, (int) std::llround ((double) length * ratio));
    juce::AudioBuffer<float> out (nch, outLen);
    out.clear();
    if (length <= 0) return out;

    constexpr int D = 4;                                                   // decimazione per la ricerca grossolana
    const int W = juce::jmax (256, ((int) std::round (rate * 0.04) / 16) * 16);   // finestra ~40 ms
    const int Hs = W / 2, delta = W / 4;                                  // passo di sintesi e tolleranza di ricerca
    const int padL = delta, padR = W + delta + Hs;                        // zeri attorno al segnale: padL e' multiplo di D

    std::vector<float> mono ((size_t) (padL + length + padR), 0.0f);
    for (int i = 0; i < length; ++i)
    {
        float s = 0.0f;
        for (int c = 0; c < nch; ++c) s += in.getSample (c, start + i);
        mono[(size_t) (padL + i)] = s / (float) juce::jmax (1, nch);
    }
    std::vector<float> dm (mono.size() / (size_t) D, 0.0f);
    for (size_t j = 0; j < dm.size(); ++j)
    {
        float s = 0.0f;
        for (int k = 0; k < D; ++k) s += mono[j * D + (size_t) k];
        dm[j] = s / (float) D;
    }

    std::vector<float> hann ((size_t) W);
    for (int i = 0; i < W; ++i) hann[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) W);

    const int total = (int) mono.size();
    const int frames = (outLen - 1) / Hs + 1;
    int prev = 0;                                                         // inizio (nella regione) del frame precedente
    for (int k = 0; k < frames; ++k)
    {
        int pos = 0;
        if (k > 0)
        {
            const int nominal = (int) std::llround ((double) k * Hs / ratio);
            const int t0 = prev + Hs;                                     // continuazione naturale del frame precedente
            pos = nominal;
            const int lo = juce::jmax (-padL, nominal - delta), hi = juce::jmin (total - padL - W, nominal + delta);
            if (t0 + padL + W <= total && t0 >= -padL && hi > lo)
            {
                // ricerca grossolana sul segnale decimato, poi rifinitura al campione
                auto corr = [&] (const std::vector<float>& x, int step, int a, int b, int n)
                {
                    double num = 0.0, en = 1.0e-9;
                    for (int i = 0; i < n; ++i) { const double u = x[(size_t) (a + i * step)], v = x[(size_t) (b + i * step)]; num += u * v; en += v * v; }
                    return num / std::sqrt (en);
                };
                int best = nominal;
                double bestV = -1.0e30;
                for (int c = lo; c <= hi; c += D)
                {
                    const double v = corr (dm, 1, (t0 + padL) / D, (c + padL) / D, W / D);
                    if (v > bestV) { bestV = v; best = c; }
                }
                bestV = -1.0e30;
                for (int c = juce::jmax (lo, best - D); c <= juce::jmin (hi, best + D); ++c)
                {
                    const double v = corr (mono, 2, t0 + padL, c + padL, W / 2);
                    if (v > bestV) { bestV = v; pos = c; }
                }
            }
        }
        prev = pos;

        const int o = k * Hs;
        for (int i = 0; i < W && o + i < outLen; ++i)
        {
            const int si = pos + i;
            if (si < 0 || si >= length) continue;
            const float w = (k == 0 && i < Hs) ? 1.0f : hann[(size_t) i];   // il primo mezzo frame non sfuma: l'attacco resta intatto
            for (int c = 0; c < nch; ++c) out.getWritePointer (c)[o + i] += in.getSample (c, start + si) * w;
        }
    }
    return out;
}

/** Cambia la durata insieme al tono (come rallentare un nastro): ricampionamento. */
inline juce::AudioBuffer<float> varispeed (const juce::AudioBuffer<float>& in, int start, int length, double ratio)
{
    const int nch = in.getNumChannels();
    const int outLen = juce::jmax (1, (int) std::llround ((double) length * ratio));
    juce::AudioBuffer<float> out (nch, outLen);
    for (int c = 0; c < nch; ++c)
    {
        juce::LagrangeInterpolator interp;
        interp.process (1.0 / ratio, in.getReadPointer (c) + start, out.getWritePointer (c), outLen);
    }
    return out;
}

} // namespace fc
