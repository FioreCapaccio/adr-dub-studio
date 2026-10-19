#include "VoiceGender.h"

namespace fc::gender
{
namespace
{
constexpr double kRate = 16000.0;
constexpr int kWin = 512;          // 32 ms
constexpr int kHop = 320;          // 20 ms
constexpr int kTauMin = 32, kTauMax = 267;   // 500 .. 60 Hz a 16 kHz
constexpr int kMinVoiced = 6;      // frame sonori per fidarsi del tono di un pezzo

/** Porta l'audio a 16 kHz mono (passa-basso a 3,5 kHz a due stadi + interpolazione lineare). */
std::vector<float> to16k (const float* x, int64_t n, double rate)
{
    std::vector<float> y (x, x + n);
    const double fc = juce::jmin (3500.0, rate * 0.45);
    for (int pass = 0; pass < 2; ++pass)
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * fc / rate, c = std::cos (w0), a = std::sin (w0) / 1.4142135;
        const double b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2, a0 = 1 + a, a1 = -2 * c, a2 = 1 - a;
        double z1 = 0, z2 = 0;
        for (auto& s : y)
        {
            const double in = s, out = (b0 * in + z1) / a0;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            s = (float) out;
        }
    }
    const int64_t m = (int64_t) ((double) n * kRate / rate);
    std::vector<float> o ((size_t) juce::jmax<int64_t> (0, m));
    for (int64_t i = 0; i < m; ++i)
    {
        const double p = (double) i * rate / kRate;
        const int64_t i0 = (int64_t) p;
        const float f = (float) (p - (double) i0);
        o[(size_t) i] = y[(size_t) juce::jmin<int64_t> (i0, n - 1)] * (1.0f - f) + y[(size_t) juce::jmin<int64_t> (i0 + 1, n - 1)] * f;
    }
    return o;
}

/** Tono del frame che comincia a 'x' (servono kWin + kTauMax campioni); 0 se non e' sonoro. */
float yinPitch (const float* x)
{
    float d[kTauMax + 1];
    d[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau <= kTauMax; ++tau)
    {
        float s = 0.0f;
        for (int j = 0; j < kWin; ++j) { const float v = x[j] - x[j + tau]; s += v * v; }
        running += s;
        d[tau] = running > 0.0f ? s * (float) tau / running : 1.0f;
    }
    int best = 0;
    for (int tau = kTauMin; tau < kTauMax; ++tau)
        if (d[tau] < 0.15f)
        {
            while (tau + 1 < kTauMax && d[tau + 1] < d[tau]) ++tau;
            best = tau;
            break;
        }
    if (best == 0) return 0.0f;
    float t = (float) best;
    if (best > 1 && best < kTauMax)   // interpolazione parabolica
    {
        const float s0 = d[best - 1], s1 = d[best], s2 = d[best + 1], den = s0 + s2 - 2.0f * s1;
        if (std::abs (den) > 1.0e-9f) t += 0.5f * (s0 - s2) / den;
    }
    return (float) (kRate / (double) t);
}

float medianOf (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

struct Piece
{
    int firstFrame = 0, lastFrame = 0;   // frame di parlato (inclusi)
    std::vector<float> f0;               // toni dei frame sonori
    float median = 0.0f;
    int cls = -1;                        // 0 maschile, 1 femminile
    bool ambiguous = false;
};
} // namespace

std::vector<VoiceSeg> analyse (const float* mono, int64_t n, double sampleRate, const Settings& st)
{
    std::vector<VoiceSeg> out;
    if (n < (int64_t) sampleRate / 4) return out;
    const auto x = to16k (mono, n, sampleRate);
    const int nf = ((int) x.size() - kWin - kTauMax) / kHop + 1;
    if (nf < 3) return out;

    // 1) energia dei frame e soglia del parlato
    std::vector<float> e ((size_t) nf);
    for (int f = 0; f < nf; ++f)
    {
        double s = 0.0;
        const float* p = x.data() + (size_t) f * kHop;
        for (int j = 0; j < kWin; ++j) s += (double) p[j] * p[j];
        e[(size_t) f] = juce::Decibels::gainToDecibels ((float) std::sqrt (s / kWin), -120.0f);
    }
    auto sorted = e;
    std::sort (sorted.begin(), sorted.end());
    const float thr = juce::jmax (-50.0f, sorted[(size_t) ((double) (nf - 1) * 0.95)] - 30.0f);
    std::vector<char> speech ((size_t) nf);
    for (int f = 0; f < nf; ++f) speech[(size_t) f] = e[(size_t) f] > thr;

    // 2) pezzi di parlato separati da pause di almeno pauseSec
    const int pauseFrames = juce::jmax (2, (int) std::lround (st.pauseSec * kRate / kHop));
    std::vector<Piece> pieces;
    for (int f = 0; f < nf; ++f)
    {
        if (! speech[(size_t) f]) continue;
        if (! pieces.empty() && f - pieces.back().lastFrame - 1 < pauseFrames) pieces.back().lastFrame = f;
        else { Piece p; p.firstFrame = p.lastFrame = f; pieces.push_back (std::move (p)); }
    }
    if (pieces.empty()) return out;
    for (auto& p : pieces)
    {
        for (int f = p.firstFrame; f <= p.lastFrame; ++f)
            if (speech[(size_t) f]) { const float h = yinPitch (x.data() + (size_t) f * kHop); if (h > 0.0f) p.f0.push_back (h); }
        if ((int) p.f0.size() >= kMinVoiced) p.median = medianOf (p.f0);
    }

    // 3) soglia: due gruppi di toni ben distanti (rapporto >= 1,25) -> punto medio, altrimenti quella fissa
    std::vector<double> logs;
    for (auto& p : pieces) if (p.median > 0.0f) logs.push_back (std::log ((double) p.median));
    double threshold = st.fixedThresholdHz;
    if (logs.size() >= 2)
    {
        double lo = *std::min_element (logs.begin(), logs.end()), hi = *std::max_element (logs.begin(), logs.end());
        for (int it = 0; it < 12; ++it)
        {
            double sl = 0, sh = 0; int nl = 0, nh = 0;
            for (double v : logs) { if (std::abs (v - lo) <= std::abs (v - hi)) { sl += v; ++nl; } else { sh += v; ++nh; } }
            if (nl) lo = sl / nl;
            if (nh) hi = sh / nh;
        }
        if (hi - lo >= std::log (1.25)) threshold = juce::jlimit (st.minThresholdHz, st.maxThresholdHz, std::exp ((lo + hi) * 0.5));
    }

    // 4) genere di ogni pezzo; vicino alla soglia e' ambiguo e prende quello del pezzo chiaro piu' vicino
    for (auto& p : pieces)
    {
        if (p.median <= 0.0f) continue;
        const double r = (double) p.median / threshold;
        if (r > st.ambiguityRatio) p.cls = 1;
        else if (r < 1.0 / st.ambiguityRatio) p.cls = 0;
        else p.ambiguous = true;
    }
    int prev = -1;
    for (auto& p : pieces) { if (p.cls >= 0) prev = p.cls; else if (prev >= 0) p.cls = prev; }
    prev = -1;
    for (size_t i = pieces.size(); i-- > 0;) { if (! pieces[i].ambiguous && pieces[i].median > 0.0f) prev = pieces[i].cls; else if (pieces[i].cls < 0 && prev >= 0) pieces[i].cls = prev; }
    for (auto& p : pieces) if (p.cls < 0) p.cls = 0;   // nessun tono in tutto l'audio: tutto "maschile" e incerto, ma hasBoth() sara' falso

    // 5) tagli a meta' delle pause tra pezzi di genere diverso; pezzi vicini dello stesso genere si uniscono
    const double toSrc = sampleRate / kRate;
    auto frameStart = [&] (int f) { return (int64_t) std::llround ((double) f * kHop * toSrc); };
    auto frameEnd = [&] (int f) { return (int64_t) std::llround ((double) (f * kHop + kWin) * toSrc); };
    struct Acc { int cls; int firstPiece, lastPiece; };
    std::vector<Acc> groups;
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        if (! groups.empty() && groups.back().cls == pieces[i].cls) groups.back().lastPiece = (int) i;
        else groups.push_back ({ pieces[i].cls, (int) i, (int) i });
    }
    int64_t cursor = 0;
    for (size_t g = 0; g < groups.size(); ++g)
    {
        VoiceSeg s;
        s.female = groups[g].cls == 1;
        s.start = cursor;
        int64_t end = n;
        if (g + 1 < groups.size())
        {
            const int64_t a = frameEnd (pieces[(size_t) groups[g].lastPiece].lastFrame), b = frameStart (pieces[(size_t) groups[g + 1].firstPiece].firstFrame);
            end = juce::jlimit<int64_t> (cursor, n, (a + b) / 2);
        }
        s.length = end - cursor;
        cursor = end;

        std::vector<float> all;
        int voiced = 0, amb = 0;
        for (int k = groups[g].firstPiece; k <= groups[g].lastPiece; ++k)
        {
            auto& p = pieces[(size_t) k];
            all.insert (all.end(), p.f0.begin(), p.f0.end());
            if (p.median > 0.0f) { voiced += (int) p.f0.size(); if (p.ambiguous) amb += (int) p.f0.size(); }
        }
        s.f0 = medianOf (all);
        s.uncertain = voiced < 15 || amb * 10 > voiced * 3;   // poco tono (< 0,3 s) o piu' del 30% vicino alla soglia
        if (! out.empty() && out.back().female == s.female) { out.back().length += s.length; out.back().uncertain = out.back().uncertain || s.uncertain; }
        else out.push_back (s);
    }
    return out;
}

std::vector<VoiceSeg> analyse (const juce::AudioBuffer<float>& b, int64_t offset, int64_t length, double sampleRate, const Settings& st)
{
    const int nch = b.getNumChannels();
    length = juce::jmin<int64_t> (length, b.getNumSamples() - offset);
    if (nch == 0 || length <= 0) return {};
    std::vector<float> mono ((size_t) length, 0.0f);
    for (int c = 0; c < nch; ++c)
    {
        const float* p = b.getReadPointer (c) + offset;
        for (int64_t i = 0; i < length; ++i) mono[(size_t) i] += p[i] / (float) nch;
    }
    return analyse (mono.data(), length, sampleRate, st);
}

bool hasBoth (const std::vector<VoiceSeg>& v)
{
    bool m = false, f = false;
    for (auto& s : v) (s.female ? f : m) = true;
    return m && f;
}
} // namespace fc::gender
