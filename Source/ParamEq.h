#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>

namespace fc
{
/** Equalizzatore parametrico a 4 bande (filtri biquad RBJ). I parametri sono atomici: la GUI li cambia mentre il thread audio li legge,
    e il filtro li insegue con una rampa breve per evitare scatti. */
class ParamEq
{
public:
    static constexpr int kBands = 4;
    enum Type { Bell = 0, LowShelf, HighShelf, HighPass, LowPass, kNumTypes };

    static juce::String typeName (int t)
    {
        static const char* n[] = { TRX ("Campana"), TRX ("Shelf basso"), TRX ("Shelf alto"), TRX ("Passa-alto"), TRX ("Passa-basso") };
        return juce::translate (n[juce::jlimit (0, kNumTypes - 1, t)]);
    }

    struct Band
    {
        std::atomic<float> freq { 1000.0f }, gainDb { 0.0f }, q { 1.0f };
        std::atomic<int> type { Bell };
        std::atomic<bool> on { true };
    };

    std::atomic<bool> enabled { false };
    Band band[kBands];

    ParamEq()
    {
        setBand (0, LowShelf, 100.0f, 0.0f, 0.7f);
        setBand (1, Bell, 500.0f, 0.0f, 1.0f);
        setBand (2, Bell, 2500.0f, 0.0f, 1.0f);
        setBand (3, HighShelf, 8000.0f, 0.0f, 0.7f);
    }

    void setBand (int i, int type, float f, float g, float q)
    {
        band[i].type = type; band[i].freq = f; band[i].gainDb = g; band[i].q = q; band[i].on = true;
    }

    void copyFrom (const ParamEq& o)
    {
        enabled = o.enabled.load();
        for (int i = 0; i < kBands; ++i)
        {
            band[i].type = o.band[i].type.load(); band[i].freq = o.band[i].freq.load(); band[i].gainDb = o.band[i].gainDb.load();
            band[i].q = o.band[i].q.load(); band[i].on = o.band[i].on.load();
        }
    }

    /** Vero se il filtro non cambia il segnale (spento, o tutte le bande a guadagno zero e senza passa-alto/basso). */
    bool isNeutral() const
    {
        if (! enabled.load()) return true;
        for (auto& b : band)
        {
            if (! b.on.load()) continue;
            const int t = b.type.load();
            if (t == HighPass || t == LowPass || std::abs (b.gainDb.load()) > 0.01f) return false;
        }
        return true;
    }

    struct Coefs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    static Coefs makeCoefs (int type, double freq, double gainDb, double q, double rate)
    {
        freq = juce::jlimit (10.0, rate * 0.45, freq);
        q = juce::jlimit (0.1, 20.0, q);
        const double w0 = juce::MathConstants<double>::twoPi * freq / rate, c = std::cos (w0), s = std::sin (w0);
        const double alpha = s / (2.0 * q), A = std::pow (10.0, gainDb / 40.0);
        double b0, b1, b2, a0, a1, a2;
        switch (type)
        {
            case LowShelf:
            {
                const double k = 2.0 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * c + k); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - k);
                a0 = (A + 1) + (A - 1) * c + k;       a1 = -2 * ((A - 1) + (A + 1) * c);    a2 = (A + 1) + (A - 1) * c - k;
                break;
            }
            case HighShelf:
            {
                const double k = 2.0 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * c + k); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - k);
                a0 = (A + 1) - (A - 1) * c + k;       a1 = 2 * ((A - 1) - (A + 1) * c);      a2 = (A + 1) - (A - 1) * c - k;
                break;
            }
            case HighPass:
                b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
                break;
            case LowPass:
                b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
                break;
            default:   // campana
                b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A;
                break;
        }
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    static double magnitudeDb (const Coefs& k, double hz, double rate)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / rate;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
        const auto num = k.b0 + k.b1 * z1 + k.b2 * z2, den = 1.0 + k.a1 * z1 + k.a2 * z2;
        return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (num / den)));
    }

    /** Risposta totale (dB) alla frequenza indicata, dai parametri attuali: per il disegno della curva. */
    double responseDb (double hz, double rate) const
    {
        if (! enabled.load()) return 0.0;
        double db = 0.0;
        for (auto& b : band)
            if (b.on.load()) db += magnitudeDb (makeCoefs (b.type.load(), b.freq.load(), b.gainDb.load(), b.q.load(), rate), hz, rate);
        return db;
    }

    /** Thread audio / esportazione: filtra in place due canali. */
    void process (float* l, float* r, int n, double rate)
    {
        if (isNeutral()) { resetState(); return; }

        constexpr int sub = 64;
        for (int pos = 0; pos < n; pos += sub)
        {
            const int m = juce::jmin (sub, n - pos);
            for (int i = 0; i < kBands; ++i)
            {
                auto& st = state[(size_t) i];
                const bool on = band[i].on.load();
                const int type = band[i].type.load();
                const float tf = band[i].freq.load(), tg = on ? band[i].gainDb.load() : 0.0f, tq = band[i].q.load();
                const bool active = on && (type == HighPass || type == LowPass || std::abs (tg) > 0.01f || std::abs (st.curG) > 0.01f);
                if (! active) { st.z[0][0] = st.z[0][1] = st.z[1][0] = st.z[1][1] = 0.0; st.curG = tg; st.curF = tf; st.curQ = tq; st.valid = false; continue; }

                // inseguimento dei parametri (frequenza in scala logaritmica)
                if (! st.valid) { st.curF = tf; st.curG = tg; st.curQ = tq; }
                else
                {
                    st.curF = std::exp (std::log (st.curF) + (std::log (tf) - std::log (st.curF)) * 0.3f);
                    st.curG += (tg - st.curG) * 0.3f;
                    st.curQ += (tq - st.curQ) * 0.3f;
                }
                if (! st.valid || st.lastType != type || st.curF != st.lastF || st.curG != st.lastG || st.curQ != st.lastQ || st.lastRate != rate)
                {
                    st.k = makeCoefs (type, st.curF, st.curG, st.curQ, rate);
                    st.lastType = type; st.lastF = st.curF; st.lastG = st.curG; st.lastQ = st.curQ; st.lastRate = rate; st.valid = true;
                }
                float* ch[2] = { l + pos, r + pos };
                for (int c = 0; c < 2; ++c)
                    for (int s = 0; s < m; ++s)
                    {
                        const double x = ch[c][s];
                        const double y = st.k.b0 * x + st.z[c][0];
                        st.z[c][0] = st.k.b1 * x - st.k.a1 * y + st.z[c][1];
                        st.z[c][1] = st.k.b2 * x - st.k.a2 * y;
                        ch[c][s] = (float) y;
                    }
            }
        }
    }

private:
    struct State
    {
        Coefs k;
        double z[2][2] {};
        float curF = 1000.0f, curG = 0.0f, curQ = 1.0f, lastF = 0.0f, lastG = 0.0f, lastQ = 0.0f;
        double lastRate = 0.0;
        int lastType = -1;
        bool valid = false;
    };
    std::array<State, kBands> state;

    void resetState()
    {
        for (auto& st : state) { st.z[0][0] = st.z[0][1] = st.z[1][0] = st.z[1][1] = 0.0; st.valid = false; st.curG = 0.0f; }
    }
};

} // namespace fc
