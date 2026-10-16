#include "Conform.h"

namespace fc::conform
{
namespace
{
using Range = std::pair<double, double>;
constexpr double kEps = 0.0005;

std::vector<Range> mergeRanges (std::vector<Range> v)
{
    std::sort (v.begin(), v.end());
    std::vector<Range> out;
    for (auto& r : v)
    {
        if (r.second - r.first < kEps) continue;
        if (! out.empty() && r.first <= out.back().second + kEps) out.back().second = juce::jmax (out.back().second, r.second);
        else out.push_back (r);
    }
    return out;
}

/** Le parti di 'whole' non coperte da 'covered' (entrambi ordinati e uniti). */
std::vector<Range> subtract (const std::vector<Range>& whole, const std::vector<Range>& covered)
{
    std::vector<Range> out;
    for (auto w : whole)
    {
        double pos = w.first;
        for (auto& c : covered)
        {
            if (c.second <= pos || c.first >= w.second) continue;
            if (c.first > pos + kEps) out.emplace_back (pos, c.first);
            pos = juce::jmax (pos, c.second);
        }
        if (w.second > pos + kEps) out.emplace_back (pos, w.second);
    }
    return out;
}

bool isVideoChannel (const juce::String& ch)
{
    const auto u = ch.toUpperCase();
    return u == "V" || u == "B" || u == "VA" || u == "AA/V" || (u.startsWith ("V") && ! u.startsWith ("VO"));
}
} // namespace

Parsed parseEdl (const juce::String& text, int tcFormatIndex)
{
    Parsed r;
    tcFormatIndex = juce::jlimit (0, numTcFormats - 1, tcFormatIndex);
    const auto lines = juce::StringArray::fromLines (text);
    for (auto& l : lines)
        if (l.trim().toUpperCase().startsWith ("FCM:") && l.toUpperCase().contains ("DROP") && ! l.toUpperCase().contains ("NON"))
            r.formatIndex = 4;
    const int fi = r.formatIndex >= 0 ? r.formatIndex : tcFormatIndex;
    const auto& fmt = tcFormats()[fi];

    std::vector<Event> all, video;
    int badTc = 0;
    bool dissolve = false, retime = false, anyVideoChannel = false;
    Event* last = nullptr;
    std::vector<bool> isVideo;
    for (auto& raw : lines)
    {
        const auto l = raw.trim();
        if (l.isEmpty()) continue;
        if (l.startsWithChar ('*'))
        {
            const auto body = l.substring (1).trim();
            if (last != nullptr && body.toUpperCase().startsWith ("FROM CLIP NAME:")) last->clipName = body.fromFirstOccurrenceOf (":", false, false).trim();
            continue;
        }
        auto toks = juce::StringArray::fromTokens (l, " \t", "");
        toks.removeEmptyStrings();
        if (toks.size() >= 2 && toks[0].toUpperCase() == "M2")
        {
            if (last != nullptr) { last->retimed = true; retime = true; }
            continue;
        }
        if (toks.size() < 8 || ! toks[0].containsOnly ("0123456789")) continue;

        // numero, bobina, canale, transizione, [durata], srcIn srcOut recIn recOut
        double t[4] = {};
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k) ok = timecodeToSeconds (toks[toks.size() - 4 + k], fmt, t[k]);
        if (! ok) { ++badTc; continue; }
        Event e;
        e.number = toks[0].getIntValue();
        e.reel = toks[1];
        e.transition = toks[3];
        e.srcIn = t[0]; e.srcOut = t[1]; e.recIn = t[2]; e.recOut = t[3];
        if (e.recOut <= e.recIn || e.srcOut <= e.srcIn) continue;
        if (e.transition.toUpperCase().startsWith ("D") || e.transition.toUpperCase().startsWith ("W")) dissolve = true;
        const bool vid = isVideoChannel (toks[2]);
        anyVideoChannel = anyVideoChannel || vid;
        all.push_back (e);
        isVideo.push_back (vid);
        last = &all.back();
    }
    for (size_t i = 0; i < all.size(); ++i)
        if (isVideo[i] || ! anyVideoChannel) video.push_back (all[i]);
    if (! anyVideoChannel && ! all.empty()) r.warnings.add ("L'EDL non ha eventi video (canale V): uso tutti gli eventi.");
    if (badTc > 0)
        r.warnings.add (juce::String (badTc) + " eventi saltati: timecode non valido per il formato " + juce::String (fmt.name) + " (i fotogrammi del file superano quelli del formato?).");
    if (dissolve) r.warnings.add ("L'EDL contiene dissolvenze o wipe: sono trattate come tagli netti all'inizio dell'evento.");
    if (retime) r.warnings.add ("Alcuni eventi hanno un cambio di velocita' (M2): non e' supportato, quegli eventi non vengono seguiti.");
    r.events = std::move (video);
    return r;
}

Map buildMap (const std::vector<Event>& oldE, const std::vector<Event>& newE)
{
    Map m;
    std::vector<Range> oldRanges, newRanges, mappedOld, mappedNew;
    for (auto& o : oldE) oldRanges.emplace_back (o.recIn, o.recOut);
    for (auto& n : newE) newRanges.emplace_back (n.recIn, n.recOut);
    m.coverage = mergeRanges (oldRanges);
    const auto newMerged = mergeRanges (newRanges);

    struct Cand { double os, oe, ns; };
    std::vector<Cand> cands;
    for (auto& o : oldE)
    {
        if (o.retimed) continue;
        for (auto& n : newE)
        {
            if (n.retimed) continue;
            if (! o.reel.equalsIgnoreCase (n.reel)) continue;
            if (o.clipName.isNotEmpty() && n.clipName.isNotEmpty() && ! o.clipName.equalsIgnoreCase (n.clipName)) continue;
            const double a = juce::jmax (o.srcIn, n.srcIn), b = juce::jmin (o.srcOut, n.srcOut);
            if (b - a <= kEps) continue;
            cands.push_back ({ o.recIn + (a - o.srcIn), o.recIn + (b - o.srcIn), n.recIn + (a - n.srcIn) });
        }
    }
    std::sort (cands.begin(), cands.end(), [] (const Cand& a, const Cand& b) { return a.os != b.os ? a.os < b.os : a.ns < b.ns; });

    // lo stesso materiale usato due volte nel nuovo montaggio: si segue solo la prima occorrenza
    bool duplicates = false;
    double covered = -1.0e18;
    for (auto& c : cands)
    {
        double os = juce::jmax (c.os, covered);
        if (os >= c.oe - kEps) { duplicates = true; continue; }
        if (os > c.os + kEps) duplicates = true;
        m.segments.push_back ({ os, c.oe, c.ns + (os - c.os) });
        covered = c.oe;
    }
    if (duplicates) m.warnings.add ("Parte del materiale compare piu' volte nel nuovo montaggio: l'audio segue solo la prima occorrenza.");

    // unione dei segmenti contigui
    std::vector<Map::Seg> merged;
    for (auto& s : m.segments)
    {
        if (! merged.empty() && std::abs (merged.back().oldEnd - s.oldStart) < kEps && std::abs (merged.back().newStart + (merged.back().oldEnd - merged.back().oldStart) - s.newStart) < kEps)
            merged.back().oldEnd = s.oldEnd;
        else merged.push_back (s);
    }
    m.segments = merged;

    for (auto& s : m.segments) { mappedOld.emplace_back (s.oldStart, s.oldEnd); mappedNew.emplace_back (s.newStart, s.newStart + (s.oldEnd - s.oldStart)); }
    m.removed = subtract (m.coverage, mergeRanges (mappedOld));
    m.inserted = subtract (newMerged, mergeRanges (mappedNew));
    if (m.segments.empty()) m.warnings.add ("Nessun materiale in comune fra i due montaggi: controlla i nomi delle bobine e che gli EDL siano della stessa sessione.");
    return m;
}

ConformPlan makePlan (const Map& m, double tcStart, double rate, bool markInserted)
{
    ConformPlan p;
    auto smp = [&] (double tc) { return (int64_t) std::llround ((tc - tcStart) * rate); };
    for (auto& s : m.segments) p.segments.push_back ({ smp (s.oldStart), smp (s.oldEnd), smp (s.newStart) });
    for (auto& c : m.coverage) p.coverage.emplace_back (smp (c.first), smp (c.second));
    for (auto& i : m.inserted) p.inserted.emplace_back (smp (i.first), smp (i.second));
    p.markInserted = markInserted;
    return p;
}

juce::String describe (const Map& m, const ConformStats& st, double rate)
{
    (void) rate;
    double removed = 0.0, inserted = 0.0, moved = 0.0;
    for (auto& r : m.removed) removed += r.second - r.first;
    for (auto& r : m.inserted) inserted += r.second - r.first;
    for (auto& s : m.segments) if (std::abs (s.newStart - s.oldStart) > kEps) moved += s.oldEnd - s.oldStart;
    juce::String t;
    t << m.segments.size() << " pezzi in comune (" << juce::String (moved, 1) << " s spostati)\n"
      << "Materiale tolto: " << m.removed.size() << " zone, " << juce::String (removed, 1) << " s\n"
      << "Materiale nuovo (senza audio): " << m.inserted.size() << " zone, " << juce::String (inserted, 1) << " s\n\n"
      << "Clip toccati: " << st.clipsTouched << " (" << st.clipPieces << " pezzi, " << st.clipsDropped << " eliminati del tutto, " << juce::String (st.droppedSeconds, 1) << " s di audio tolto)\n"
      << "Cue spostati: " << st.cuesMoved << ", cue fuori dal nuovo montaggio (segnati Da rifare): " << st.cuesOut << "\n"
      << "Marker spostati: " << st.markersMoved << ", marker sul materiale nuovo: " << st.insertedMarkers;
    return t;
}
} // namespace fc::conform
