#include "Leveler.h"
#include "Loudness.h"
#include "ExportEngine.h"
#include <algorithm>
#include <cmath>

namespace fc::level
{
Analyser::Analyser (double sampleRate)
{
    subLen = juce::jmax (1, (int) std::lround (sampleRate * kHopSec));
    for (int c = 0; c < 2; ++c)
    {
        Loudness::Biquad shelf, hp;
        Loudness::makeKWeighting (sampleRate, shelf, hp);
        filt[(size_t) c][0] = { shelf.b0, shelf.b1, shelf.b2, shelf.a1, shelf.a2 };
        filt[(size_t) c][1] = { hp.b0, hp.b1, hp.b2, hp.a1, hp.a2 };
    }
}

void Analyser::process (const float* l, const float* r, int n)
{
    auto run = [] (Filt& f, double x)
    {
        const double y = f.b0 * x + f.z1;
        f.z1 = f.b1 * x - f.a1 * y + f.z2;
        f.z2 = f.b2 * x - f.a2 * y;
        return y;
    };
    for (int i = 0; i < n; ++i)
    {
        const double yl = run (filt[0][1], run (filt[0][0], (double) l[i]));
        const double yr = run (filt[1][1], run (filt[1][0], (double) r[i]));
        acc += yl * yl + yr * yr;
        blockPeak = juce::jmax (blockPeak, std::abs (l[i]), std::abs (r[i]));
        if (++count >= subLen)
        {
            e.push_back (acc / (double) count);
            pk.push_back (blockPeak);
            acc = 0.0; count = 0; blockPeak = 0.0f;
        }
    }
}

void Analyser::finish()
{
    if (count > subLen / 4)   // l'ultimo blocco parziale conta se ha abbastanza campioni
    {
        e.push_back (acc / (double) count);
        pk.push_back (blockPeak);
    }
    acc = 0.0; count = 0; blockPeak = 0.0f;
}

Frames framesFrom (const Analyser& a)
{
    Frames f;
    const int n = (int) a.energy().size();
    f.lufs.resize ((size_t) n);
    f.peakDb.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        // finestra di 4 blocchi centrata sul confine tra il blocco i-1 e i: blocchi i-2 .. i+1 (dove esistono)
        // i blocchi piu' di 20 dB sotto il piu' forte della finestra (pausa che entra o esce) non contano: ai bordi del parlato il livello e' quello vero
        double sum = 0.0, top = 0.0;
        int cnt = 0;
        float pk = 0.0f;
        const int k0 = i - kSubPerWindow / 2, k1 = i + kSubPerWindow / 2;
        for (int k = k0; k < k1; ++k)
            if (k >= 0 && k < n) { top = juce::jmax (top, a.energy()[(size_t) k]); pk = juce::jmax (pk, a.peak()[(size_t) k]); }
        for (int k = k0; k < k1; ++k)
            if (k >= 0 && k < n && a.energy()[(size_t) k] >= top * 0.01) { sum += a.energy()[(size_t) k]; ++cnt; }
        f.lufs[(size_t) i] = (float) Loudness::energyToLufs (cnt > 0 ? sum / cnt : 0.0);
        f.peakDb[(size_t) i] = juce::Decibels::gainToDecibels (pk, -120.0f);
    }
    return f;
}

namespace
{
double median (std::vector<float> v)
{
    if (v.empty()) return -100.0;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return (double) v[v.size() / 2];
}

double stddev (const std::vector<float>& v)
{
    if (v.size() < 2) return 0.0;
    double m = 0.0;
    for (auto x : v) m += x;
    m /= (double) v.size();
    double s = 0.0;
    for (auto x : v) s += (x - m) * (x - m);
    return std::sqrt (s / (double) v.size());
}

void simplify (const std::vector<std::pair<double, float>>& p, size_t a, size_t b, double tol, std::vector<bool>& keep)
{
    if (b <= a + 1) return;
    double worst = 0.0;
    size_t wi = a;
    for (size_t i = a + 1; i < b; ++i)
    {
        const double t = (p[i].first - p[a].first) / (p[b].first - p[a].first);
        const double expect = p[a].second + t * (p[b].second - p[a].second);
        const double d = std::abs (p[i].second - expect);
        if (d > worst) { worst = d; wi = i; }
    }
    if (worst > tol) { keep[wi] = true; simplify (p, a, wi, tol, keep); simplify (p, wi, b, tol, keep); }
}
} // namespace

Plan planGains (const Frames& fr, const Settings& st)
{
    Plan p;
    const size_t n = fr.lufs.size();
    p.gainDb.assign (n, 0.0f);
    if (n == 0) return p;

    // 1) frame con parlato: sopra il gate assoluto, poi sopra (mediana - gate relativo)
    std::vector<float> cand;
    for (auto v : fr.lufs) if (v > (float) st.gateAbsLufs) cand.push_back (v);
    if (cand.empty()) return p;
    const double med0 = median (cand);
    std::vector<char> active (n, 0);
    std::vector<float> speech;
    for (size_t i = 0; i < n; ++i)
        if (fr.lufs[i] > (float) st.gateAbsLufs && fr.lufs[i] > (float) (med0 - st.gateRelLu)) { active[i] = 1; speech.push_back (fr.lufs[i]); }
    p.activeFrames = (int) speech.size();
    p.speechMedian = median (speech);
    p.targetUsed = st.autoTarget ? p.speechMedian : st.targetLufs;
    p.spreadBefore = stddev (speech);

    // 2) guadagno desiderato: obiettivo - livello, entro i limiti e il tetto dei picchi; nelle pause si mantiene l'ultimo valore
    std::vector<float> want (n, 0.0f);
    float last = 0.0f;
    bool haveLast = false;
    for (size_t i = 0; i < n; ++i)
    {
        if (active[i])
        {
            float d = (float) juce::jlimit (-st.maxCutDb, st.maxBoostDb, p.targetUsed - (double) fr.lufs[i]);
            if (d > 0.0f) d = juce::jmin (d, juce::jmax (0.0f, (float) st.peakCeilingDb - fr.peakDb[i]));
            last = d;
            haveLast = true;
        }
        want[i] = last;
    }
    if (haveLast)   // le pause all'inizio prendono il primo valore vero
        for (size_t i = 0; i < n && ! active[i]; ++i) want[i] = want[std::find (active.begin(), active.end(), 1) - active.begin()];

    // nella pausa subito prima di una frase (ultimo mezzo secondo) il guadagno si porta gia' al valore della frase: cosi' all'attacco e' a posto
    {
        constexpr size_t lead = 5;
        size_t nextActive = n;
        for (size_t i = n; i-- > 0;)
        {
            if (active[i]) nextActive = i;
            else if (nextActive < n && nextActive - i <= lead) want[i] = want[nextActive];
        }
    }

    // 3) zona morta e smussamento asimmetrico (si scende piu' in fretta di quanto si risale)
    static const double down[3] = { 0.6, 0.3, 0.15 }, up[3] = { 1.2, 0.8, 0.4 };
    const double tauDown = down[juce::jlimit (0, 2, st.reaction)], tauUp = up[juce::jlimit (0, 2, st.reaction)];
    float g = want[0];
    bool moving = false;   // isteresi: si parte quando la differenza supera la zona morta, ci si ferma quando e' quasi annullata
    for (size_t i = 0; i < n; ++i)
    {
        const float d = want[i];
        if (! moving && std::abs (d - g) >= (float) st.deadbandDb) moving = true;
        if (moving)
        {
            g += (float) ((d - g) * (1.0 - std::exp (-kHopSec / (d < g ? tauDown : tauUp))));
            if (std::abs (d - g) < 0.15f) moving = false;
        }
        p.gainDb[i] = g;
    }

    std::vector<float> after;
    for (size_t i = 0; i < n; ++i) if (active[i]) after.push_back (fr.lufs[i] + p.gainDb[i]);
    p.spreadAfter = stddev (after);
    return p;
}

std::vector<std::pair<int64_t, float>> curveFrom (const Plan& p, double rate, const Settings& st)
{
    std::vector<std::pair<double, float>> pts;
    for (size_t i = 0; i < p.gainDb.size(); ++i) pts.emplace_back ((double) i * kHopSec, p.gainDb[i]);
    std::vector<std::pair<int64_t, float>> out;
    if (pts.empty()) return out;
    if (pts.size() == 1) { out.emplace_back (0, pts[0].second); return out; }
    std::vector<bool> keep (pts.size(), false);
    keep.front() = keep.back() = true;
    simplify (pts, 0, pts.size() - 1, st.toleranceDb, keep);
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.emplace_back ((int64_t) std::llround (pts[i].first * rate), pts[i].second);
    return out;
}

Outcome process (const Request& rq, const std::function<bool (double)>& setProgress, const std::function<void (const juce::String&)>& setStatus)
{
    Outcome out;
    juce::WavAudioFormat wav;
    const int nt = (int) rq.trackIds.size();
    for (int ti = 0; ti < nt; ++ti)
    {
        const int id = rq.trackIds[(size_t) ti];
        juce::String name;
        for (auto& t : rq.snapshot.tracks) if (t.id == id) name = t.name;
        setStatus (TRANS ("Traccia ") + name + " (" + juce::String (ti + 1) + "/" + juce::String (nt) + ")");
        const double base = (double) ti / nt, share = 1.0 / nt;

        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("adr_livella_" + juce::String (juce::Random::getSystemRandom().nextInt64()) + ".wav");
        juce::String err;
        const bool rendered = renderTrackProcessed (rq.snapshot, id, rq.rate, rq.totalSamples, tmp,
                                                    [&] (double p) { return setProgress (base + share * 0.8 * p); }, err);
        if (! rendered)
        {
            tmp.deleteFile();
            out.cancelled = err.isEmpty();
            out.message = err.isEmpty() ? TRANS ("Annullato.") : err;
            return out;
        }
        std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (tmp.createInputStream().release(), true));
        if (rd == nullptr) { tmp.deleteFile(); out.message = TRANS ("Impossibile leggere l'audio della traccia."); return out; }
        Analyser an (rd->sampleRate);
        juce::AudioBuffer<float> buf (2, 1 << 16);
        for (int64_t pos = 0; pos < rd->lengthInSamples; pos += buf.getNumSamples())
        {
            const int n = (int) juce::jmin<int64_t> (buf.getNumSamples(), rd->lengthInSamples - pos);
            rd->read (&buf, 0, n, pos, true, true);
            an.process (buf.getReadPointer (0), buf.getReadPointer (rd->numChannels > 1 ? 1 : 0), n);
            if (! setProgress (base + share * (0.8 + 0.2 * (double) pos / (double) rd->lengthInSamples))) { rd.reset(); tmp.deleteFile(); out.cancelled = true; out.message = TRANS ("Annullato."); return out; }
        }
        an.finish();
        rd.reset();
        tmp.deleteFile();

        TrackCurve tc;
        tc.trackId = id;
        tc.trackName = name;
        tc.plan = planGains (framesFrom (an), rq.settings);
        if (tc.plan.activeFrames == 0) continue;   // niente parlato: nulla da livellare
        tc.points = curveFrom (tc.plan, rq.rate, rq.settings);
        out.curves.push_back (std::move (tc));
    }
    out.ok = true;
    if (out.curves.empty()) out.message = TRANS ("Non ho trovato audio da livellare (silenzio o livello troppo basso).");
    return out;
}

int apply (Project& p, const std::vector<TrackCurve>& curves, int64_t from, int64_t to, int* clamped)
{
    if (clamped != nullptr) *clamped = 0;
    if (curves.empty()) return 0;
    p.beginEdit();
    int made = 0;
    for (auto& c : curves)
    {
        auto* t = p.findTrack (c.trackId);
        if (t == nullptr || c.points.empty()) continue;
        const float base = t->live->volumeDb.load();
        auto& lane = t->volAuto;
        const int64_t lo = from >= 0 ? from : 0, hi = to >= 0 ? to : std::numeric_limits<int64_t>::max();
        const float atLo = lane.empty() ? base : evalAutomationDb (lane, lo), atHi = lane.empty() ? base : evalAutomationDb (lane, hi);
        lane.erase (std::remove_if (lane.begin(), lane.end(), [lo, hi] (const AutoPoint& a) { return a.pos >= lo && a.pos <= hi; }), lane.end());
        auto add = [&] (int64_t pos, float db)
        {
            AutoPoint a;
            a.id = p.nextAutoPointId();
            a.pos = pos;
            const float v = clampAutoValue (AutoVolume, db);
            if (v != db && clamped != nullptr) ++*clamped;
            a.db = v;
            lane.push_back (a);
        };
        const bool partial = from >= 0 || to >= 0;
        if (partial) add (lo, atLo);                         // il bordo dell'intervallo raccorda con cio' che c'era
        for (auto& pt : c.points)
        {
            const int64_t pos = pt.first;
            if (pos < lo || pos > hi) continue;
            if (partial && (pos == lo || pos == hi)) continue;
            add (pos, base + pt.second);
        }
        if (partial) add (hi, atHi);
        std::sort (lane.begin(), lane.end(), [] (const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
        if (t->live->autoMode.load() == 0) t->live->autoMode = 1;
        t->live->autoParam = AutoVolume;
        ++made;
    }
    p.changed();
    return made;
}
} // namespace fc::level
