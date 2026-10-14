#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

/** Misura del loudness secondo ITU-R BS.1770-4 / EBU R128 (K-weighting, gating, LRA, true-peak stimato). */
namespace Loudness
{
//==============================================================================
struct Standard
{
    const char* name;
    double target;      // LUFS
    double ceiling;     // picco massimo consentito (dBTP)
    double tolerance;   // tolleranza in LU
    const char* detail;
};

/** L'indice 0 e' "nessuno": nessun obiettivo. */
inline const std::vector<Standard>& standards()
{
    static const std::vector<Standard> list =
    {
        { "Nessuno", 0.0, 0.0, 0.0, "Nessun obiettivo: la misura LUFS resta attiva." },
        { "EBU R128 - TV e radio Europa", -23.0, -1.0, 0.5, "TV e radio in Europa, usato anche in Italia. Target -23 LUFS (tolleranza 0,5 LU), picco max -1 dBTP." },
        { "ATSC A/85 - TV USA", -24.0, -2.0, 2.0, "TV americana (CALM Act). Target -24 LKFS (tolleranza 2 LU), picco max -2 dBTP." },
        { "ARIB TR-B32 - TV Giappone", -24.0, -1.0, 1.0, "TV giapponese. Target -24 LKFS (tolleranza 1 LU), picco max -1 dBTP." },
        { "Netflix - dialoghi", -27.0, -2.0, 2.0, "Specifica Netflix: loudness dei dialoghi -27 LKFS (tolleranza 2 LU), picco max -2 dBTP." },
        { "Podcast - Apple / Amazon", -16.0, -1.0, 1.0, "Raccomandazione per podcast: -16 LUFS (tolleranza 1 LU), picco max -1 dBTP." },
        { "Streaming - Spotify, YouTube", -14.0, -1.0, 1.0, "Streaming e social video: -14 LUFS, picco max -1 dBTP." },
        { "Audiolibri - ACX / Audible", -20.0, -3.0, 2.0, "ACX: RMS tra -23 e -18 dB, picco max -3 dB. Target -20 LUFS (centro dell'intervallo)." }
    };
    return list;
}

//==============================================================================
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
};

/** Filtro K-weighting (BS.1770-4) a qualunque frequenza di campionamento. */
inline void makeKWeighting (double fs, Biquad& shelf, Biquad& highpass)
{
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (K * K - 1.0) / a0;
        highpass.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

inline double energyToLufs (double e) { return e > 1.0e-12 ? -0.691 + 10.0 * std::log10 (e) : -100.0; }
inline double lufsToEnergy (double l) { return std::pow (10.0, (l + 0.691) / 10.0); }

//==============================================================================
/** Misuratore: momentary (400 ms), short-term (3 s), integrato con gating, LRA, true-peak (stima con interpolazione cubica 4x). */
class Meter
{
public:
    std::atomic<float> momentary { -100.0f }, shortTerm { -100.0f }, integrated { -100.0f }, lra { 0.0f };
    std::atomic<float> samplePeak { 0.0f }, truePeak { 0.0f };   // lineari

    void prepare (double sampleRate)
    {
        subLen = juce::jmax (1, (int) std::round (sampleRate * 0.1));
        for (int c = 0; c < 2; ++c)
            makeKWeighting (sampleRate, shelf[c], highpass[c]);
        resetAll();
    }

    /** Azzera da un thread qualsiasi: l'azzeramento avviene al prossimo blocco audio. */
    void requestReset() { resetFlag = true; }

    void resetAll()
    {
        for (int c = 0; c < 2; ++c) { shelf[c].reset(); highpass[c].reset(); hist4[c].fill (0.0f); }
        ring.fill (0.0);
        ringPos = filled = count = 0;
        acc = 0.0;
        histogram.fill (0);
        stHistogram.fill (0);
        momentary = shortTerm = integrated = -100.0f;
        lra = 0.0f;
        samplePeak = truePeak = 0.0f;
    }

    /** Elabora un blocco stereo (a valle di tutto: cio' che va all'uscita). */
    void process (const float* l, const float* r, int n)
    {
        if (resetFlag.exchange (false)) resetAll();

        float blockPeak = 0.0f, blockTrue = 0.0f;
        const float* in[2] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            const double yl = highpass[0].process (shelf[0].process ((double) l[i]));
            const double yr = highpass[1].process (shelf[1].process ((double) r[i]));
            acc += yl * yl + yr * yr;
            blockPeak = juce::jmax (blockPeak, std::abs (l[i]), std::abs (r[i]));

            // true-peak: stima dei picchi tra i campioni con interpolazione di Hermite a 4 punti
            for (int c = 0; c < 2; ++c)
            {
                auto& h = hist4[c];
                h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = in[c][i];
                const float xm1 = h[0], x0 = h[1], x1 = h[2], x2 = h[3];
                const float c0 = x0, c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                blockTrue = juce::jmax (blockTrue, std::abs (x0), std::abs (x1));
                for (float t : { 0.25f, 0.5f, 0.75f })
                    blockTrue = juce::jmax (blockTrue, std::abs (((c3 * t + c2) * t + c1) * t + c0));
            }

            if (++count >= subLen) finishSubBlock();
        }
        if (blockPeak > samplePeak.load()) samplePeak.store (blockPeak);
        if (blockTrue > truePeak.load()) truePeak.store (blockTrue);
    }

private:
    static constexpr int histBins = 800;   // da -70 a +10 LUFS, passo 0,1 LU

    void finishSubBlock()
    {
        ring[(size_t) ringPos] = acc / (double) count;
        ringPos = (ringPos + 1) % 30;
        filled = juce::jmin (filled + 1, 30);
        acc = 0.0;
        count = 0;
        if (filled < 4) return;

        auto meanOfLast = [this] (int num)
        {
            double s = 0.0;
            for (int k = 1; k <= num; ++k) s += ring[(size_t) ((ringPos - k + 30) % 30)];
            return s / (double) num;
        };

        const double lm = energyToLufs (meanOfLast (4));
        momentary = (float) lm;
        const double ls = energyToLufs (meanOfLast (filled));
        shortTerm = (float) ls;

        if (lm > -70.0)
        {
            ++histogram[(size_t) juce::jlimit (0, histBins - 1, (int) std::floor ((lm + 70.0) * 10.0))];
            integrated = (float) computeIntegrated();
        }
        if (filled >= 30 && ls > -70.0)
        {
            ++stHistogram[(size_t) juce::jlimit (0, histBins - 1, (int) std::floor ((ls + 70.0) * 10.0))];
            lra = (float) computeLra();
        }
    }

    double gatedMeanEnergy (const std::array<uint32_t, histBins>& h, double minLufs) const
    {
        double sum = 0.0, n = 0.0;
        for (int i = 0; i < histBins; ++i)
        {
            const double centre = -70.0 + ((double) i + 0.5) * 0.1;
            if (h[(size_t) i] > 0 && centre >= minLufs) { sum += (double) h[(size_t) i] * lufsToEnergy (centre); n += (double) h[(size_t) i]; }
        }
        return n > 0.0 ? sum / n : 0.0;
    }

    double computeIntegrated() const
    {
        const double e1 = gatedMeanEnergy (histogram, -70.0);
        if (e1 <= 0.0) return -100.0;
        const double e2 = gatedMeanEnergy (histogram, energyToLufs (e1) - 10.0);
        return e2 > 0.0 ? energyToLufs (e2) : -100.0;
    }

    double computeLra() const
    {
        const double e1 = gatedMeanEnergy (stHistogram, -70.0);
        if (e1 <= 0.0) return 0.0;
        const double gate = energyToLufs (e1) - 20.0;
        double total = 0.0;
        for (int i = 0; i < histBins; ++i)
            if (-70.0 + ((double) i + 0.5) * 0.1 >= gate) total += (double) stHistogram[(size_t) i];
        if (total < 2.0) return 0.0;
        auto percentile = [&] (double p)
        {
            double cum = 0.0;
            for (int i = 0; i < histBins; ++i)
            {
                const double centre = -70.0 + ((double) i + 0.5) * 0.1;
                if (centre < gate) continue;
                cum += (double) stHistogram[(size_t) i];
                if (cum >= p * total) return centre;
            }
            return gate;
        };
        return juce::jmax (0.0, percentile (0.95) - percentile (0.10));
    }

    Biquad shelf[2], highpass[2];
    std::array<float, 4> hist4[2] {};
    std::array<double, 30> ring {};
    std::array<uint32_t, histBins> histogram {}, stHistogram {};
    int ringPos = 0, filled = 0, count = 0, subLen = 4800;
    double acc = 0.0;
    std::atomic<bool> resetFlag { false };
};

} // namespace Loudness
