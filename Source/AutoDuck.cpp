#include "AutoDuck.h"
#include "AudioAlign.h"
#include "TrackPresets.h"

namespace fc::duck
{
std::vector<std::pair<int64_t, int64_t>> speechIntervals (const Session& s, double rate, const Settings& st)
{
    const int hop = juce::jmax (1, (int) std::lround (rate * 0.010));
    const int64_t end = s.endSample();
    std::vector<float> env ((size_t) (end / hop + 2), 0.0f);

    for (auto& t : s.tracks)
    {
        if (t.live->trackType.load() != TrackDialogue || t.live->mute.load()) continue;
        for (auto& c : t.clips)
        {
            if (c.asset == nullptr || c.muted) continue;
            const float g = dbToGain (c.gainDb);
            const int chn = c.asset->numChannels();
            for (int64_t pos = c.start; pos < c.end(); pos += hop)
            {
                const int64_t src = c.offset + (pos - c.start);
                const int n = (int) juce::jmin<int64_t> (hop, c.end() - pos);
                if (src < 0 || src + n > c.asset->length()) continue;
                double sum = 0.0;
                for (int k = 0; k < chn; ++k)
                {
                    const float* d = c.asset->audio.getReadPointer (k) + src;
                    for (int i = 0; i < n; ++i) sum += (double) d[i] * d[i];
                }
                const float rms = g * (float) std::sqrt (sum / (double) juce::jmax (1, n * chn));
                auto& e = env[(size_t) (pos / hop)];
                e = juce::jmax (e, rms);
            }
        }
    }

    const float thr = (float) juce::Decibels::decibelsToGain (st.thresholdDb);
    const int holdHops = (int) std::ceil (st.holdSec / 0.010);
    std::vector<std::pair<int64_t, int64_t>> out;
    int start = -1, lastLoud = -1;
    for (int i = 0; i < (int) env.size(); ++i)
    {
        if (env[(size_t) i] > thr)
        {
            if (start < 0) start = i;
            lastLoud = i;
        }
        else if (start >= 0 && i - lastLoud > holdHops)
        {
            out.emplace_back ((int64_t) start * hop, (int64_t) (lastLoud + 1) * hop);
            start = -1;
        }
    }
    if (start >= 0) out.emplace_back ((int64_t) start * hop, (int64_t) (lastLoud + 1) * hop);
    return out;
}

std::vector<std::pair<int64_t, float>> duckCurve (const std::vector<std::pair<int64_t, int64_t>>& iv, double rate, const Settings& st)
{
    std::vector<std::pair<int64_t, float>> pts;
    if (iv.empty()) return pts;
    const int64_t margin = (int64_t) std::llround (st.marginSec * rate), atk = (int64_t) std::llround (st.attackSec * rate), rel = (int64_t) std::llround (st.releaseSec * rate);
    const float depth = (float) -std::abs (st.depthDb);

    // intervalli vicini (la risalita non avrebbe il tempo di finire) vengono uniti
    std::vector<std::pair<int64_t, int64_t>> merged;
    for (auto& r : iv)
    {
        if (! merged.empty() && r.first - margin - atk <= merged.back().second + margin + rel) merged.back().second = juce::jmax (merged.back().second, r.second);
        else merged.push_back (r);
    }
    for (auto& r : merged)
    {
        pts.emplace_back (juce::jmax<int64_t> (0, r.first - margin - atk), 0.0f);
        pts.emplace_back (juce::jmax<int64_t> (1, r.first - margin), depth);
        pts.emplace_back (r.second + margin, depth);
        pts.emplace_back (r.second + margin + rel, 0.0f);
    }
    // la prima posizione puo' essere schiacciata a 0: si tolgono i punti che non avanzano
    std::vector<std::pair<int64_t, float>> clean;
    for (auto& p : pts)
        if (clean.empty() || p.first > clean.back().first) clean.push_back (p);
    return clean;
}

int apply (Project& p, const Settings& st, int fallbackTrackId)
{
    const double rate = p.getSampleRate();
    const auto curve = duckCurve (speechIntervals (p.getSession(), rate, st), rate, st);
    if (curve.empty()) return 0;

    std::vector<int> targets;
    for (auto& t : p.getSession().tracks)
    {
        const int type = t.live->trackType.load();
        if (type == TrackME || type == TrackEffects) targets.push_back (t.id);
    }
    if (targets.empty() && p.findTrack (fallbackTrackId) != nullptr) targets.push_back (fallbackTrackId);
    if (targets.empty()) return 0;

    const int64_t from = curve.front().first, to = curve.back().first;
    p.beginEdit();
    for (int id : targets)
    {
        auto* t = p.findTrack (id);
        const float base = t->live->volumeDb.load();
        auto& lane = t->volAuto;
        lane.erase (std::remove_if (lane.begin(), lane.end(), [from, to] (const AutoPoint& a) { return a.pos >= from && a.pos <= to; }), lane.end());
        for (auto& c : curve)
        {
            AutoPoint a;
            a.id = p.nextAutoPointId();
            a.pos = c.first;
            a.db = clampAutoValue (AutoVolume, base + c.second);
            lane.push_back (a);
        }
        std::sort (lane.begin(), lane.end(), [] (const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
        if (t->live->autoMode.load() == 0) t->live->autoMode = 1;
        t->live->autoParam = AutoVolume;
    }
    p.changed();
    return (int) targets.size();
}
} // namespace fc::duck
