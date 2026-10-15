#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

namespace fc
{
/** Compressore di traccia: rilevamento sul picco dei due canali (stereo linkato), curva con ginocchio morbido,
    attacco e rilascio sul guadagno. Nessuna latenza. I parametri sono atomici (la GUI li cambia mentre l'audio li legge). */
class TrackComp
{
public:
    std::atomic<bool> enabled { false };
    std::atomic<float> thresholdDb { -18.0f }, ratio { 3.0f }, attackMs { 10.0f }, releaseMs { 120.0f }, kneeDb { 6.0f }, makeupDb { 0.0f };
    std::atomic<float> grDb { 0.0f };   // massima riduzione di guadagno nell'ultimo blocco (dB, >= 0), per i misuratori

    void copyFrom (const TrackComp& o)
    {
        enabled = o.enabled.load(); thresholdDb = o.thresholdDb.load(); ratio = o.ratio.load(); attackMs = o.attackMs.load();
        releaseMs = o.releaseMs.load(); kneeDb = o.kneeDb.load(); makeupDb = o.makeupDb.load();
    }

    /** Riduzione di guadagno statica (dB, <= 0) per un livello d'ingresso in dB. */
    static float staticGainDb (float levelDb, float thr, float ratio, float knee)
    {
        const float over = levelDb - thr, slope = 1.0f / juce::jmax (1.0f, ratio) - 1.0f;
        if (2.0f * over < -knee) return 0.0f;
        if (2.0f * std::abs (over) <= knee) { const float x = over + knee * 0.5f; return slope * x * x / (2.0f * juce::jmax (0.01f, knee)); }
        return slope * over;
    }

    void process (float* l, float* r, int n, double rate)
    {
        if (! enabled.load()) { env = 0.0f; return; }
        const float thr = thresholdDb.load(), ra = ratio.load(), knee = kneeDb.load();
        const float atk = std::exp (-1.0f / juce::jmax (1.0f, attackMs.load() * 0.001f * (float) rate));
        const float rel = std::exp (-1.0f / juce::jmax (1.0f, releaseMs.load() * 0.001f * (float) rate));
        const float makeup = makeupDb.load();
        float minEnv = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float lvl = juce::jmax (std::abs (l[i]), std::abs (r[i]));
            const float db = lvl > 1.0e-6f ? 20.0f * std::log10 (lvl) : -120.0f;
            const float target = staticGainDb (db, thr, ra, knee);
            const float c = target < env ? atk : rel;   // piu' riduzione = attacco
            env = c * env + (1.0f - c) * target;
            minEnv = juce::jmin (minEnv, env);
            const float g = std::exp ((env + makeup) * 0.11512925f);   // dB -> guadagno lineare
            l[i] *= g;
            r[i] *= g;
        }
        const float gr = -minEnv;
        if (gr > grDb.load()) grDb.store (gr);
    }

private:
    float env = 0.0f;   // riduzione di guadagno smussata, dB (<= 0)
};

} // namespace fc
