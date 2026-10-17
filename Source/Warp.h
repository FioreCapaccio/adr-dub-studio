#pragma once

#include <JuceHeader.h>
#include <vector>

#include "TimeStretch.h"

namespace fc::warp
{
/** Punto di ancoraggio del Time Warp, in campioni dall'inizio del clip: il materiale che era a 'src' finisce a 'dst'. */
struct Anchor { int64_t src = 0, dst = 0; };

/** Ordina i punti, scarta quelli fuori dal clip e forza distanze minime (60 ms) e rapporto fra 0,25 e 4 fra un punto e il successivo. */
inline std::vector<Anchor> normalise (std::vector<Anchor> a, int64_t len, double rate)
{
    const int64_t minGap = (int64_t) (0.06 * rate);
    std::sort (a.begin(), a.end(), [] (const Anchor& x, const Anchor& y) { return x.src < y.src; });
    std::vector<Anchor> out;
    Anchor prev { 0, 0 };
    for (auto p : a)
    {
        if (p.src <= prev.src + minGap || p.src >= len - minGap) continue;
        const int64_t srcGap = p.src - prev.src;
        p.dst = juce::jlimit (prev.dst + juce::jmax<int64_t> (minGap, srcGap / 4), prev.dst + srcGap * 4, p.dst);
        out.push_back (p);
        prev = p;
    }
    return out;
}

/** Durata del clip dopo il warp: il materiale dopo l'ultimo punto conserva la sua durata e segue lo spostamento. */
inline int64_t warpedLength (const std::vector<Anchor>& a, int64_t len)
{
    return a.empty() ? len : len + (a.back().dst - a.back().src);
}

/** Dove finisce, dopo il warp, un punto del materiale originale (interpolazione fra i punti). */
inline int64_t mapPosition (const std::vector<Anchor>& a, int64_t len, int64_t pos)
{
    const int64_t outLen = warpedLength (a, len);
    Anchor p0 { 0, 0 };
    for (size_t i = 0; i <= a.size(); ++i)
    {
        const Anchor p1 = i < a.size() ? a[i] : Anchor { len, outLen };
        if (pos <= p1.src)
            return p0.dst + (int64_t) std::llround ((double) (pos - p0.src) * (double) (p1.dst - p0.dst) / (double) juce::jmax<int64_t> (1, p1.src - p0.src));
        p0 = p1;
    }
    return outLen;
}

/** Genera l'audio con i punti spostati: fra due punti il materiale viene allungato o accorciato (tono invariato), con brevi
    sfumature incrociate sui punti. 'a' deve essere gia' normalizzato. */
inline juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& audio, int off, int len, const std::vector<Anchor>& a, double rate)
{
    const int nch = audio.getNumChannels(), outLen = (int) warpedLength (a, len);
    juce::AudioBuffer<float> out (nch, juce::jmax (1, outLen));
    out.clear();
    const int xf = juce::jmax (16, (int) (0.004 * rate));

    std::vector<Anchor> pts { { 0, 0 } };
    pts.insert (pts.end(), a.begin(), a.end());
    pts.push_back ({ len, outLen });

    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const int s0 = (int) pts[i].src, s1 = (int) pts[i + 1].src, d0 = (int) pts[i].dst, d1 = (int) pts[i + 1].dst;
        const bool last = i + 2 == pts.size(), first = i == 0;
        const int srcLen = s1 - s0, dstLen = d1 - d0;
        const double ratio = (double) dstLen / (double) juce::jmax (1, srcLen);
        // un po' di materiale oltre la fine del pezzo per la sfumatura incrociata col successivo
        const int ext = last ? 0 : juce::jmin (len - s1, (int) std::ceil ((double) xf / ratio));
        juce::AudioBuffer<float> seg;
        if (std::abs (ratio - 1.0) < 0.003)
        {
            seg.setSize (nch, srcLen + ext);
            for (int c = 0; c < nch; ++c) seg.copyFrom (c, 0, audio, c, off + s0, srcLen + ext);
        }
        else seg = timeStretch (audio, off + s0, srcLen + ext, ratio, rate);

        const int n = juce::jmin (seg.getNumSamples(), dstLen + (last ? 0 : xf));
        for (int k = 0; k < n; ++k)
        {
            float g = 1.0f;
            if (! first && k < xf) g *= (float) k / (float) xf;
            if (! last && k >= dstLen) g *= 1.0f - (float) (k - dstLen) / (float) xf;
            if (d0 + k >= out.getNumSamples()) break;
            for (int c = 0; c < nch; ++c) out.getWritePointer (c)[d0 + k] += seg.getSample (c, k) * g;
        }
    }
    return out;
}
} // namespace fc::warp
