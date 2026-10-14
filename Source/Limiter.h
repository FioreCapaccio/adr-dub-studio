#pragma once

#include <JuceHeader.h>
#include <vector>

namespace fc
{
/** Limiter "brickwall" stereo con look-ahead.

    Per ogni campione il guadagno richiesto e' min(1, tetto / picco). Il guadagno applicato e' la media mobile
    (su N campioni) del minimo di questi valori sugli ultimi N campioni, e il segnale viene ritardato di N-1 campioni:
    cosi' il guadagno e' sempre <= a quello richiesto da ogni campione nella finestra, quindi nessun picco supera il tetto,
    e la riduzione arriva in rampa (senza clic) prima del picco. Il rilascio e' esponenziale. */
class Limiter
{
public:
    void prepare (double rate)
    {
        sampleRate = rate;
        N = juce::jmax (16, (int) std::lround (0.003 * rate));          // look-ahead di ~3 ms
        releaseCoeff = 1.0f - std::exp (-1.0f / (0.080f * (float) rate)); // rilascio di ~80 ms
        reset();
    }

    int latency() const noexcept { return N - 1; }
    /** Latenza (campioni) che il limiter avra' a quella frequenza, senza doverlo preparare. */
    static int latencyFor (double rate) noexcept { return juce::jmax (16, (int) std::lround (0.003 * rate)) - 1; }

    void reset()
    {
        dl[0].assign ((size_t) (N - 1), 0.0f);
        dl[1].assign ((size_t) (N - 1), 0.0f);
        targets.assign ((size_t) N, 1.0f);
        released.assign ((size_t) N, 1.0f);
        sum = (double) N;
        wpD = wpT = 0;
        relGain = 1.0f;
    }

    /** Elabora in place n campioni stereo. Ritorna la massima riduzione di guadagno nel blocco, in dB (>= 0). */
    float process (juce::AudioBuffer<float>& b, int start, int n, float ceilingLinear)
    {
        float* L = b.getWritePointer (0, start);
        float* R = b.getWritePointer (1, start);
        float minGain = 1.0f;
        const int D = N - 1;

        for (int i = 0; i < n; ++i)
        {
            const float x0 = L[i], x1 = R[i];
            const float peak = juce::jmax (std::abs (x0), std::abs (x1));
            const float t = peak > ceilingLinear ? ceilingLinear / peak : 1.0f;

            targets[(size_t) wpT] = t;
            float m = 1.0f;
            for (int k = 0; k < N; ++k) m = juce::jmin (m, targets[(size_t) k]);

            if (m < relGain) relGain = m;
            else relGain += (m - relGain) * releaseCoeff;

            sum += (double) relGain - (double) released[(size_t) wpT];
            released[(size_t) wpT] = relGain;
            wpT = (wpT + 1) % N;
            const float g = (float) (sum / (double) N);
            minGain = juce::jmin (minGain, g);

            // segnale ritardato di D campioni
            const float d0 = dl[0][(size_t) wpD], d1 = dl[1][(size_t) wpD];
            dl[0][(size_t) wpD] = x0;
            dl[1][(size_t) wpD] = x1;
            wpD = (wpD + 1) % D;

            L[i] = juce::jlimit (-ceilingLinear, ceilingLinear, d0 * g);
            R[i] = juce::jlimit (-ceilingLinear, ceilingLinear, d1 * g);
        }
        return minGain >= 1.0f ? 0.0f : -juce::Decibels::gainToDecibels (minGain);
    }

private:
    double sampleRate = 48000.0;
    int N = 144, wpD = 0, wpT = 0;
    float releaseCoeff = 0.0003f, relGain = 1.0f;
    double sum = 0.0;
    std::vector<float> dl[2], targets, released;
};

} // namespace fc
