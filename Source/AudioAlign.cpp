#include "AudioAlign.h"
#include "AudioTools.h"
#include "TrackPresets.h"

namespace fc::align
{
std::vector<float> envelope (const juce::AudioBuffer<float>& b, int hop)
{
    hop = juce::jmax (1, hop);
    const int n = b.getNumSamples(), ch = b.getNumChannels();
    std::vector<float> e ((size_t) ((n + hop - 1) / hop), 0.0f);
    for (size_t i = 0; i < e.size(); ++i)
    {
        const int from = (int) i * hop, to = juce::jmin (n, from + hop);
        double sum = 0.0;
        for (int c = 0; c < ch; ++c)
        {
            const float* d = b.getReadPointer (c);
            for (int s = from; s < to; ++s) sum += (double) d[s] * d[s];
        }
        e[i] = (float) std::sqrt (sum / (double) juce::jmax (1, (to - from) * ch));
    }
    return e;
}

namespace
{
/** Compressione logaritmica e rimozione della media: il parlato forte e quello debole pesano in modo simile. */
std::vector<float> prepare (const std::vector<float>& env)
{
    std::vector<float> v (env.size());
    double mean = 0.0;
    for (size_t i = 0; i < env.size(); ++i) { v[i] = std::log (1.0f + 1000.0f * env[i]); mean += v[i]; }
    if (! v.empty()) mean /= (double) v.size();
    for (auto& x : v) x -= (float) mean;
    return v;
}

/** Correlazione normalizzata di ref[i] con tgt[i - lag] (lag in finestre) sulla zona di sovrapposizione. */
double corrAt (const std::vector<float>& r, const std::vector<float>& t, int lag, double nr, double nt)
{
    double s = 0.0;
    const int n = (int) r.size();
    const int from = juce::jmax (0, lag), to = juce::jmin (n, (int) t.size() + lag);
    if (to - from < 4) return 0.0;
    for (int i = from; i < to; ++i) s += (double) r[(size_t) i] * t[(size_t) (i - lag)];
    return s / juce::jmax (1.0e-9, nr * nt);
}

struct Best { int lag = 0; double corr = -2.0, mean = 0.0; };
Best search (const std::vector<float>& r, const std::vector<float>& t, int lo, int hi)
{
    double nr = 0.0, nt = 0.0;
    for (auto x : r) nr += (double) x * x;
    for (auto x : t) nt += (double) x * x;
    nr = std::sqrt (nr); nt = std::sqrt (nt);
    Best b;
    double sum = 0.0;
    for (int lag = lo; lag <= hi; ++lag)
    {
        const double c = corrAt (r, t, lag, nr, nt);
        sum += c;
        if (c > b.corr) { b.corr = c; b.lag = lag; }
    }
    b.mean = sum / (double) juce::jmax (1, hi - lo + 1);
    return b;
}
} // namespace

Shift findShift (const juce::AudioBuffer<float>& ref, const juce::AudioBuffer<float>& target, double rate, double maxShiftSec)
{
    Shift out;
    if (ref.getNumSamples() < 2 || target.getNumSamples() != ref.getNumSamples()) return out;

    // passo grossolano: finestre di 10 ms
    const int hop = juce::jmax (1, (int) std::lround (rate * 0.010));
    const auto r = prepare (envelope (ref, hop)), t = prepare (envelope (target, hop));
    if (r.size() < 8) return out;
    const int maxLag = juce::jmax (1, (int) std::ceil (maxShiftSec / 0.010));
    const auto coarse = search (r, t, -maxLag, maxLag);
    if (coarse.corr <= 0.0) return out;

    // affinamento: finestre di 1 ms attorno al risultato grossolano (+-20 ms)
    const int hop2 = juce::jmax (1, (int) std::lround (rate * 0.001));
    const auto r2 = prepare (envelope (ref, hop2)), t2 = prepare (envelope (target, hop2));
    const int centre = coarse.lag * 10;
    const auto fine = search (r2, t2, centre - 20, centre + 20);
    out.ok = true;
    out.samples = (double) fine.lag * (double) hop2;
    // somiglianza = quanto il picco di correlazione emerge dal resto dei ritardi (un parlato diverso ha un picco debole)
    out.confidence = juce::jlimit (0.0, 1.0, (coarse.corr - coarse.mean) / juce::jmax (0.05, 1.0 - coarse.mean));
    return out;
}

Speech speechBounds (const juce::AudioBuffer<float>& b, double rate, double rangeDb)
{
    Speech s;
    const int hop = juce::jmax (1, (int) std::lround (rate * 0.010));
    const auto e = envelope (b, hop);
    if (e.empty()) return s;
    const float peak = *std::max_element (e.begin(), e.end());
    if (peak < 1.0e-4f) return s;
    const float thr = peak * (float) juce::Decibels::decibelsToGain (-rangeDb);
    const int minRun = 3;   // 30 ms sopra soglia: ignora i click
    int first = -1, last = -1, run = 0;
    for (int i = 0; i < (int) e.size(); ++i)
    {
        if (e[(size_t) i] > thr) { if (++run >= minRun) { if (first < 0) first = i - minRun + 1; last = i; } }
        else run = 0;
    }
    if (first < 0) return s;
    s.ok = true;
    s.start = (int64_t) first * hop;
    s.end = (int64_t) (last + 1) * hop;
    return s;
}

juce::AudioBuffer<float> renderMono (const std::vector<const Clip*>& clips, int64_t t0, int64_t n)
{
    juce::AudioBuffer<float> out (1, (int) juce::jmax<int64_t> (1, n));
    out.clear();
    float* d = out.getWritePointer (0);
    for (auto* c : clips)
    {
        if (c == nullptr || c->asset == nullptr || c->asset->offline || c->muted) continue;
        const int64_t a = juce::jmax (t0, c->start), z = juce::jmin (t0 + n, c->end());
        if (z <= a) continue;
        const float g = dbToGain (c->gainDb);
        const int chn = c->asset->numChannels();
        for (int64_t p = a; p < z; ++p)
        {
            const int64_t src = c->offset + (p - c->start);
            if (src < 0 || src >= c->asset->length()) continue;
            float v = 0.0f;
            for (int k = 0; k < chn; ++k) v += c->asset->audio.getSample (k, (int) src);
            d[p - t0] += g * v / (float) juce::jmax (1, chn);
        }
    }
    return out;
}

std::vector<Item> alignToGuide (Project& p, const std::vector<int64_t>& clipIds, double maxShiftSec, double minConfidence, bool adaptDuration)
{
    std::vector<Item> items;
    const double rate = p.getSampleRate();
    const auto& s = p.getSession();

    std::vector<const Clip*> guide;
    for (auto& t : s.tracks)
        if (t.live->trackType.load() == TrackGuide && ! t.live->mute.load())
            for (auto& c : t.clips) guide.push_back (&c);
    if (guide.empty()) return items;

    std::vector<std::pair<int64_t, int64_t>> moves;   // clip, nuova posizione
    for (auto id : clipIds)
    {
        Item it;
        it.clipId = id;
        int ti = -1;
        const Clip* c = p.findClip (id, &ti);
        if (c == nullptr || c->asset == nullptr) { it.note = TRANS ("clip non valido"); items.push_back (it); continue; }
        if (s.tracks[(size_t) ti].live->trackType.load() == TrackGuide) { it.note = TRANS ("e' sulla traccia Guida"); items.push_back (it); continue; }

        const int64_t margin = (int64_t) std::llround (maxShiftSec * rate);
        const int64_t t0 = juce::jmax<int64_t> (0, c->start - margin), t1 = c->end() + margin, n = t1 - t0;
        std::vector<const Clip*> near;
        for (auto* g : guide) if (g->end() > t0 && g->start < t1) near.push_back (g);
        if (near.empty()) { it.note = TRANS ("nessun dialogo di guida in questa posizione"); items.push_back (it); continue; }

        const auto ref = renderMono (near, t0, n), tgt = renderMono ({ c }, t0, n);
        const auto sh = findShift (ref, tgt, rate, maxShiftSec);
        it.shiftMs = sh.samples * 1000.0 / rate;
        it.confidence = sh.confidence;
        if (! sh.ok || sh.confidence < minConfidence) { it.note = TRANS ("somiglianza troppo bassa (") + juce::String (sh.confidence, 2) + ")"; items.push_back (it); continue; }

        int64_t newStart = juce::jmax<int64_t> (0, c->start + (int64_t) std::llround (sh.samples));
        if (adaptDuration)
        {
            // dopo lo spostamento si fanno coincidere inizio e fine del parlato con quelli della guida
            const auto gs = speechBounds (ref, rate), ts = speechBounds (tgt, rate);
            if (gs.ok && ts.ok && ts.end - ts.start > (int64_t) (0.3 * rate))
            {
                const double ratio = (double) (gs.end - gs.start) / (double) (ts.end - ts.start);
                if (ratio >= 0.8 && ratio <= 1.25 && std::abs (ratio - 1.0) > 0.01)
                {
                    it.stretchRatio = ratio;
                    // l'inizio del parlato del take (a ts.start dalla finestra) deve cadere sull'inizio del parlato della guida
                    const int64_t speechInClip = t0 + ts.start - c->start;
                    it.stretchStart = juce::jmax<int64_t> (0, t0 + gs.start - (int64_t) std::llround ((double) speechInClip * ratio));
                    newStart = it.stretchStart;
                }
            }
        }
        it.applied = true;
        it.note = it.stretchRatio != 1.0 ? TRANS ("spostato e adattato") : TRANS ("spostato");
        moves.emplace_back (id, newStart);
        items.push_back (it);
    }

    if (! moves.empty())
    {
        p.beginEdit();
        for (auto& m : moves)
            if (auto* c = p.findClip (m.first))
            {
                const auto it = std::find_if (items.begin(), items.end(), [&m] (const Item& i) { return i.clipId == m.first; });
                if (it != items.end() && it->stretchRatio != 1.0) continue;   // lo sposta lo stretch
                c->start = m.second;
            }
        p.changed();
    }
    return items;
}

std::vector<PhraseItem> alignPhrases (Project& p, const std::vector<int64_t>& clipIds, double maxShiftSec, double minConfidence)
{
    std::vector<PhraseItem> items;
    const double rate = p.getSampleRate();
    const auto& s = p.getSession();
    std::vector<const Clip*> guide;
    for (auto& t : s.tracks)
        if (t.live->trackType.load() == TrackGuide && ! t.live->mute.load())
            for (auto& c : t.clips) guide.push_back (&c);
    if (guide.empty()) return items;

    for (auto id : clipIds)
    {
        PhraseItem it;
        it.clipId = id;
        int ti = -1;
        const Clip* c = p.findClip (id, &ti);
        if (c == nullptr || c->asset == nullptr || c->asset->offline) { it.note = TRANS ("clip non valido"); items.push_back (it); continue; }
        if (s.tracks[(size_t) ti].live->trackType.load() == TrackGuide) { it.note = TRANS ("e' sulla traccia Guida"); items.push_back (it); continue; }

        const int64_t margin = (int64_t) std::llround (maxShiftSec * rate);
        const int64_t t0 = juce::jmax<int64_t> (0, c->start - margin), t1 = c->end() + margin, n = t1 - t0;
        std::vector<const Clip*> near;
        for (auto* g : guide) if (g->end() > t0 && g->start < t1) near.push_back (g);
        if (near.empty()) { it.note = TRANS ("nessun dialogo di guida in questa posizione"); items.push_back (it); continue; }

        const auto ref = renderMono (near, t0, n), tgt = renderMono ({ c }, t0, n);
        const auto sh = findShift (ref, tgt, rate, maxShiftSec);
        it.confidence = sh.confidence;
        it.shiftMs = sh.samples * 1000.0 / rate;
        if (! sh.ok || sh.confidence < minConfidence) { it.note = TRANS ("somiglianza troppo bassa (") + juce::String (sh.confidence, 2) + ")"; items.push_back (it); continue; }

        const int64_t shift = (int64_t) std::llround (sh.samples);   // spostamento vero: il clip non puo' andare prima dello zero, ma le frasi si abbinano comunque
        it.newStart = juce::jmax<int64_t> (0, c->start + shift);
        const auto gr = tools::soundRegions (ref, 0, n, rate, -42.0f, 0.2, 0.0);
        auto tr = tools::soundRegions (tgt, 0, n, rate, -42.0f, 0.2, 0.0);
        const int64_t minLen = (int64_t) (0.12 * rate);
        std::erase_if (tr, [minLen] (const auto& r) { return r.second - r.first < minLen; });
        it.phrases = (int) tr.size();

        // ogni frase del take (spostata) si abbina alla frase della guida con cui si sovrappone di piu', nell'ordine
        size_t lastG = 0;
        bool haveLast = false;
        for (auto& r : tr)
        {
            const int64_t a0 = r.first + shift, a1 = r.second + shift;
            int best = -1;
            double bestOv = 0.4;
            for (size_t g = haveLast ? lastG + 1 : 0; g < gr.size(); ++g)
            {
                const int64_t ov = juce::jmin (a1, gr[g].second) - juce::jmax (a0, gr[g].first);
                const double f = (double) ov / (double) juce::jmax<int64_t> (1, juce::jmin (a1 - a0, gr[g].second - gr[g].first));
                if (f > bestOv) { bestOv = f; best = (int) g; }
            }
            if (best < 0) continue;
            lastG = (size_t) best;
            haveLast = true;
            const auto& gg = gr[(size_t) best];
            const int64_t durT = r.second - r.first, durG = gg.second - gg.first;
            const double ratio = (double) durG / (double) juce::jmax<int64_t> (1, durT);
            const int64_t dstEnd = (ratio >= 0.85 && ratio <= 1.2) ? gg.first + durG : gg.first + durT;   // durate troppo diverse: si sposta soltanto
            // posizioni relative all'inizio del clip (dopo lo spostamento globale)
            const int64_t base = t0 - it.newStart;
            it.anchors.push_back ({ r.first + t0 - c->start, gg.first + base });
            it.anchors.push_back ({ r.second + t0 - c->start, dstEnd + base });
            ++it.matched;
        }
        it.anchors = warp::normalise (std::move (it.anchors), c->length, rate);
        it.note = juce::String (it.matched) + TRANS (" frasi su ") + juce::String (it.phrases) + TRANS (" agganciate alla guida");
        items.push_back (it);
    }
    return items;
}
} // namespace fc::align
