#include "TimelineView.h"
#include "TrackPresets.h"
#include "AdrImport.h"
#include "VideoSupport.h"

namespace fc
{
using namespace theme;

TimelineView::TimelineView (Project& p, AudioEngine& e) : project (p), engine (e)
{
    setWantsKeyboardFocus (true);
    addAndMakeVisible (hBar);
    addAndMakeVisible (vBar);
    hBar.addListener (this);
    vBar.addListener (this);
    hBar.setAutoHide (false);
    vBar.setAutoHide (false);
    project.addChangeListener (this);
    startTimerHz (30);
}

TimelineView::~TimelineView()
{
    project.removeChangeListener (this);
}

//==============================================================================
const TimelineView::LaneInfo& TimelineView::lanesOf (int i) const
{
    static const LaneInfo single;
    const auto& tracks = project.getSession().tracks;
    if (i < 0 || i >= (int) tracks.size() || ! tracks[(size_t) i].showLanes) return single;
    const auto& t = tracks[(size_t) i];
    size_t sig = t.clips.size();
    for (auto& c : t.clips) sig = sig * 1000003u + (size_t) c.start + 31u * (size_t) c.length + (size_t) c.id;
    auto& li = laneCache[t.id];
    if (li.sig == sig && sig != 0) return li;
    li = {};
    li.sig = sig;
    std::vector<const Clip*> order;
    for (auto& c : t.clips) order.push_back (&c);
    std::stable_sort (order.begin(), order.end(), [] (const Clip* a, const Clip* b) { return a->start < b->start; });
    std::vector<int64_t> laneEnd;
    for (auto* c : order)
    {
        size_t l = 0;
        while (l < laneEnd.size() && laneEnd[l] > c->start) ++l;   // la corsia e' ancora occupata
        if (l == laneEnd.size()) laneEnd.push_back (0);
        laneEnd[l] = c->end();
        li.index[c->id] = (int) l;
    }
    li.count = juce::jmax (1, (int) laneEnd.size());
    return li;
}

int TimelineView::laneHeight (int i) const
{
    const auto& t = project.getSession().tracks;
    if (i < 0 || i >= (int) t.size()) return 96;
    return juce::jlimit (44, 140, juce::jlimit (minTrackH, maxTrackH, t[(size_t) i].live->height.load()));
}

int TimelineView::heightOf (int i) const
{
    const auto& t = project.getSession().tracks;
    if (i < 0 || i >= (int) t.size()) return 96;
    if (t[(size_t) i].hidden) return 0;
    if (const auto* g = project.findGroup (t[(size_t) i].groupId); g != nullptr && g->collapsed) return 0;
    const int base = juce::jlimit (minTrackH, maxTrackH, t[(size_t) i].live->height.load());
    if (t[(size_t) i].showLanes) return juce::jmax (base, laneHeight (i) * lanesOf (i).count);
    return base;
}

int TimelineView::bandOf (int i) const
{
    const auto& t = project.getSession().tracks;
    if (i < 0 || i >= (int) t.size() || t[(size_t) i].groupId == 0 || t[(size_t) i].hidden) return 0;
    int j = i - 1;
    while (j >= 0 && t[(size_t) j].hidden) --j;   // la barra sta sopra la prima traccia visibile del gruppo
    return j < 0 || t[(size_t) j].groupId != t[(size_t) i].groupId ? groupBandH : 0;
}

int TimelineView::contentTop (int idx) const
{
    int y = 0;
    for (int i = 0; i < idx; ++i) y += bandOf (i) + heightOf (i);
    return y + bandOf (idx);   // l'inizio della corsia, sotto l'eventuale barra del gruppo
}

int TimelineView::contentHeight() const
{
    return contentTop ((int) project.getSession().tracks.size()) + 40;
}

int TimelineView::getTrackHeight() const
{
    const int n = (int) project.getSession().tracks.size();
    return n == 0 ? 96 : (contentTop (n)) / n;
}

void TimelineView::setAllTrackHeights (int h)
{
    h = juce::jlimit (minTrackH, maxTrackH, h);
    for (auto& t : project.getSession().tracks) t.live->height = h;
    project.liveChanged();
    updateScrollBars();
    repaint();
}

void TimelineView::fitTrackHeights()
{
    const int n = juce::jmax (1, (int) project.getSession().tracks.size());
    scrollY = 0;
    setAllTrackHeights (juce::jmin (160, laneArea().getHeight() / n));   // con poche tracce non diventano enormi
}

juce::Rectangle<float> TimelineView::clipRect (int ti, const Clip& c) const
{
    const auto& tracks = project.getSession().tracks;
    if (ti >= 0 && ti < (int) tracks.size() && tracks[(size_t) ti].showLanes)
    {
        const auto& li = lanesOf (ti);
        const auto it = li.index.find (c.id);
        const int lane = it != li.index.end() ? it->second : 0, lh = laneHeight (ti);
        return { sampleToX (c.start), (float) (laneTop (ti) + lane * lh) + 4.0f, (float) ((double) c.length / spp), (float) lh - 8.0f };
    }
    return { sampleToX (c.start), (float) laneTop (ti) + 4.0f,
             (float) ((double) c.length / spp), (float) heightOf (ti) - 8.0f };
}

int TimelineView::trackIndexAtY (int y) const noexcept
{
    const int cy = y - lanesTop() + scrollY;
    if (cy < 0) return -1;
    const int n = (int) project.getSession().tracks.size();
    int top = 0;
    for (int i = 0; i < n; ++i)
    {
        const int h = bandOf (i) + heightOf (i);
        if (cy < top + h) return i;
        top += h;
    }
    return n;
}

double TimelineView::totalSamples() const
{
    const double rate = project.getSampleRate();
    return juce::jmax (300.0 * rate, (double) project.contentEnd() + 60.0 * rate);
}

void TimelineView::clampScroll()
{
    const auto lanes = laneArea();
    const double visible = lanes.getWidth() * spp;
    scrollX = juce::jlimit (0.0, juce::jmax (0.0, totalSamples() - visible), scrollX);
    scrollY = juce::jlimit (0, juce::jmax (0, contentHeight() - lanes.getHeight()), scrollY);
}

void TimelineView::updateScrollBars()
{
    clampScroll();
    const auto lanes = laneArea();
    hBar.setBounds (headerW, getHeight() - sbSize, lanes.getWidth(), sbSize);
    vBar.setBounds (getWidth() - sbSize, lanesTop(), sbSize, lanes.getHeight());
    hBar.setRangeLimits (0.0, totalSamples());
    hBar.setCurrentRange (scrollX, lanes.getWidth() * spp, juce::dontSendNotification);
    vBar.setRangeLimits (0.0, (double) juce::jmax (contentHeight(), lanes.getHeight()));
    vBar.setCurrentRange ((double) scrollY, (double) lanes.getHeight(), juce::dontSendNotification);
}

void TimelineView::resized() { updateScrollBars(); }

void TimelineView::scrollBarMoved (juce::ScrollBar* bar, double newStart)
{
    if (bar == &hBar) scrollX = newStart; else scrollY = (int) newStart;
    clampScroll();
    repaint();
}

void TimelineView::zoomBy (double factor, float anchorX)
{
    if (anchorX < 0.0f) anchorX = (float) headerW + (float) laneArea().getWidth() * 0.5f;
    const double anchorSample = xToSampleD (anchorX);
    spp = juce::jlimit (0.05, 400000.0, spp / factor);
    scrollX = anchorSample - ((double) anchorX - headerW) * spp;
    updateScrollBars();
    repaint();
    if (onZoomChanged) onZoomChanged();
}

void TimelineView::showRange (int64_t from, int64_t to)
{
    const double w = juce::jmax (100, laneArea().getWidth());
    const double a = (double) juce::jmax<int64_t> (0, from), b = (double) juce::jmax (to, from + 1);
    const double margin = juce::jmax ((b - a) * 0.08, 20.0 * spp);
    const double visible = w * spp;
    const bool fits = scrollX <= a - margin * 0.5 && scrollX + visible >= b + margin * 0.5;
    if (fits) { repaint(); return; }

    if (b - a + 2.0 * margin > visible) spp = juce::jlimit (0.05, 400000.0, (b - a + 2.0 * margin) / w);   // l'intervallo e' piu' largo della vista: si riduce lo zoom
    scrollX = juce::jmax (0.0, a - margin);
    updateScrollBars();
    repaint();
    if (onZoomChanged) onZoomChanged();
}

void TimelineView::setSpp (double newSpp)
{
    zoomBy (spp / juce::jlimit (0.05, 400000.0, newSpp));
}

void TimelineView::zoomToFit()
{
    const double rate = project.getSampleRate();
    double end = (double) project.contentEnd();
    if (auto v = project.getSession().video) end = juce::jmax (end, (v->duration + juce::jmax (0.0, v->offsetSec)) * rate);
    const double len = juce::jmax (end, 5.0 * rate) * 1.05;
    spp = juce::jmax (0.05, len / juce::jmax (100, laneArea().getWidth()));
    scrollX = 0.0;
    updateScrollBars();
    repaint();
    if (onZoomChanged) onZoomChanged();
}

void TimelineView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (stretchHoldId != 0 && project.getBusyCount() == 0) { stretchHoldId = stretchId = 0; stretchImg = {}; }   // il ricalcolo e' finito: il clip e' quello nuovo
    updateScrollBars();
    repaint();
}

void TimelineView::timerCallback()
{
    updateHeaderMeters();
    const int64_t pos = engine.getPosition();
    if (engine.isRecording()) repaint (laneArea());
    if (pos == lastPos) return;
    lastPos = pos;

    if (followEnabled && engine.isPlaying() && drag == Drag::None)
    {
        const float x = sampleToX (pos);
        const auto lanes = laneArea();
        if (x > (float) lanes.getRight() - 30.0f || x < (float) headerW)
        {
            scrollX = (double) pos - lanes.getWidth() * spp * 0.1;
            updateScrollBars();
            repaint();
            lastPlayheadX = -100.0f;
            return;
        }
    }

    const float nx = sampleToX (pos);
    repaint ((int) lastPlayheadX - 10, 0, 20, getHeight());
    repaint ((int) nx - 10, 0, 20, getHeight());
    lastPlayheadX = nx;
}

//==============================================================================
juce::String TimelineView::formatTime (double seconds, double step) const
{
    const bool neg = seconds < 0;
    seconds = std::abs (seconds);
    const int h = (int) (seconds / 3600.0);
    const int m = (int) ((seconds - h * 3600.0) / 60.0);
    const double s = seconds - h * 3600.0 - m * 60.0;
    juce::String str;
    if (h > 0) str << h << ":" << juce::String (m).paddedLeft ('0', 2) << ":";
    else str << m << ":";
    if (step >= 1.0) str << juce::String ((int) s).paddedLeft ('0', 2);
    else
    {
        const int dec = step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3);
        str << juce::String (s, dec).paddedLeft ('0', 3 + dec);
    }
    return neg ? "-" + str : str;
}

double TimelineView::rulerStep() const
{
    const double pxPerSec = project.getSampleRate() / spp;
    static const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600 };
    for (double s : steps) if (s * pxPerSec >= 90.0) return s;
    return 3600.0;
}

void TimelineView::drawExportBar (juce::Graphics& g)
{
    const auto lanes = laneArea();
    const juce::Rectangle<int> bar (headerW, 0, lanes.getWidth(), exportH);
    const juce::Colour orange (0xffff9f43);
    g.setColour (well);
    g.fillRect (bar);
    g.setColour (border);
    g.drawHorizontalLine (exportH - 1, (float) headerW, (float) bar.getRight());
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (bar);
    g.setFont (font (9.0f, true));
    if (project.exportOut > project.exportIn && project.exportIn >= 0)
    {
        const double rate = project.getSampleRate();
        const float x0 = sampleToX (project.exportIn), x1 = sampleToX (project.exportOut);
        const juce::Rectangle<float> r (x0, 1.0f, juce::jmax (3.0f, x1 - x0), (float) exportH - 3.0f);
        g.setColour (orange.withAlpha (0.88f));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (orange.brighter (0.6f));
        g.fillRect (x0 - 1.0f, 0.0f, 3.0f, (float) exportH - 1.0f);
        g.fillRect (x1 - 1.0f, 0.0f, 3.0f, (float) exportH - 1.0f);
        if (r.getWidth() > 190.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.drawText (TRANS ("ESPORTA   ") + project.formatTc ((double) project.exportIn / rate) + "  -  " + project.formatTc ((double) project.exportOut / rate)
                            + "   (" + juce::String ((double) (project.exportOut - project.exportIn) / rate, 1) + " s)",
                        r.toNearestInt().reduced (6, 0), juce::Justification::centredLeft, true);
        }
    }
    else
    {
        g.setColour (dim.withAlpha (0.8f));
        g.drawText (TRANS ("INTERVALLO DI ESPORTAZIONE: trascina qui per sceglierlo (clic destro per le opzioni)"), bar.reduced (8, 0), juce::Justification::centredLeft, true);
    }
}

void TimelineView::drawRuler (juce::Graphics& g)
{
    drawExportBar (g);
    juce::Graphics::ScopedSaveState rulerState (g);
    g.setOrigin (0, exportH);
    const int rulerH = TimelineView::rulerH - exportH;   // dentro il righello le misure sono quelle di prima, sotto la barra di esportazione
    const auto lanes = laneArea();
    const double rate = project.getSampleRate();
    juce::Rectangle<int> r (headerW, 0, lanes.getWidth(), rulerH);

    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, 0.0f, panel, 0.0f, (float) rulerH, false));
    g.fillRect (r);
    g.setColour (border);
    g.drawHorizontalLine (rulerH - 1, (float) headerW, (float) r.getRight());

    // regione di loop
    if (engine.getLoopEnd() > engine.getLoopStart())
    {
        const float x0 = sampleToX (engine.getLoopStart()), x1 = sampleToX (engine.getLoopEnd());
        auto lr = juce::Rectangle<float> (x0, 3.0f, x1 - x0, (float) rulerH - 6.0f).getIntersection (r.toFloat());
        g.setColour (blue.withAlpha (engine.isLooping() ? 0.45f : 0.18f));
        g.fillRoundedRectangle (lr, 3.0f);
        if (engine.isLooping())
        {
            g.setColour (blue);
            g.drawRoundedRectangle (lr, 3.0f, 1.0f);
        }
    }

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (r);

    const double step = rulerStep();
    const double t0 = std::floor (xToSampleD ((double) headerW) / rate / step) * step;
    const double t1 = xToSampleD ((double) r.getRight()) / rate;
    const double minor = step / 5.0;
    g.setFont (font (10.5f));
    for (double t = t0; t <= t1 + step; t += minor)
    {
        const float x = sampleToX ((int64_t) std::llround (t * rate));
        const bool major = std::abs (std::fmod (t / step + 1.0e-6, 1.0)) < 1.0e-3;
        g.setColour (major ? dim.withAlpha (0.8f) : grid.brighter (0.15f));
        g.drawVerticalLine ((int) x, major ? 17.0f : 24.0f, (float) rulerH - 1.0f);
        if (major && t >= -1.0e-9)
        {
            g.setColour (text.withAlpha (0.8f));
            g.drawText (project.rulerTimecode ? project.formatTc (t) : formatTime (t, step), (int) x + 4, 2, 90, 14, juce::Justification::centredLeft, false);
        }
    }

    // marker: bandierina e nome
    const auto& marks = project.getSession().markers;
    for (size_t i = 0; i < marks.size(); ++i)
    {
        const auto& m = marks[i];
        const float x = sampleToX (m.pos);
        if (x < (float) headerW - 12.0f || x > (float) r.getRight() + 4.0f) continue;
        juce::Path flag;
        flag.startNewSubPath (x, 17.0f);
        flag.lineTo (x + 9.0f, 17.0f);
        flag.lineTo (x + 9.0f, 25.0f);
        flag.lineTo (x + 4.5f, 29.0f);
        flag.lineTo (x, 25.0f);
        flag.closeSubPath();
        g.setColour (m.colour);
        g.fillPath (flag);
        g.fillRect (x - 0.5f, 17.0f, 1.0f, (float) rulerH - 18.0f);

        float maxW = 160.0f;
        for (size_t k = 0; k < marks.size(); ++k)
            if (marks[k].pos > m.pos) { maxW = juce::jmin (maxW, sampleToX (marks[k].pos) - x - 14.0f); break; }
        if (maxW > 22.0f)
        {
            g.setColour (m.colour.brighter (0.3f));
            g.setFont (font (10.5f, true));
            g.drawText (m.name, juce::Rectangle<float> (x + 12.0f, 16.0f, maxW, 14.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }
    }
}

void TimelineView::updateHeaderMeters()
{
    const auto& tracks = project.getSession().tracks;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        auto& m = headerMeters[tracks[i].id];
        auto& live = *tracks[i].live;
        const float l = juce::Decibels::gainToDecibels (live.meterL.exchange (0.0f), -100.0f), r = juce::Decibels::gainToDecibels (live.meterR.exchange (0.0f), -100.0f);
        const auto before = m;
        m.dbL = juce::jmax (l, m.dbL - 2.0f);   // caduta di circa 60 dB al secondo
        m.dbR = juce::jmax (r, m.dbR - 2.0f);
        const float top = juce::jmax (l, r);
        if (top > m.holdL) { m.holdL = top; m.age = 0; }
        else if (++m.age > 45) m.holdL = juce::jmax (-100.0f, m.holdL - 1.0f);
        if (std::abs (m.dbL - before.dbL) > 0.2f || std::abs (m.dbR - before.dbR) > 0.2f || std::abs (m.holdL - before.holdL) > 0.2f)
            repaint (meterRect ((int) i).expanded (2, 2));
    }
    if (headerMeters.size() > tracks.size() + 8) headerMeters.clear();
}

void TimelineView::drawHeaderMeter (juce::Graphics& g, int ti)
{
    const auto& t = project.getSession().tracks[(size_t) ti];
    const auto r = meterRect (ti).toFloat();
    const bool scale = heightOf (ti) >= 88;
    const auto it = headerMeters.find (t.id);
    const HeaderMeter m = it != headerMeters.end() ? it->second : HeaderMeter();
    auto xOf = [&] (float db) { return r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    const float barH = scale ? 5.0f : 3.5f;

    g.setColour (well);
    g.fillRoundedRectangle (r.getX() - 1.0f, r.getY() - 1.0f, r.getWidth() + 2.0f, barH * 2.0f + 3.0f, 2.5f);
    const float ys[2] = { r.getY(), r.getY() + barH + 1.0f };
    const float dbs[2] = { m.dbL, m.dbR };
    juce::ColourGradient cg (juce::Colour (0xff3fbf6a), r.getX(), 0.0f, juce::Colour (0xffe8473c), r.getRight(), 0.0f, false);   // verde fino a -18, giallo a -6, rosso a 0 dB
    cg.addColour (0.70, juce::Colour (0xff3fbf6a));
    cg.addColour (0.90, juce::Colour (0xffffc94d));
    g.setGradientFill (cg);
    for (int c = 0; c < 2; ++c)
        if (dbs[c] > -59.5f) g.fillRect (r.getX(), ys[c], xOf (dbs[c]) - r.getX(), barH);

    if (m.holdL > -59.0f)   // picco mantenuto e indicatore di clip
    {
        g.setColour (m.holdL >= -0.5f ? accent : juce::Colours::white.withAlpha (0.9f));
        g.fillRect (xOf (m.holdL) - 1.0f, r.getY() - 1.0f, 1.5f, barH * 2.0f + 3.0f);
    }
    if (m.holdL >= -0.5f) { g.setColour (accent); g.fillRect (r.getRight() - 3.0f, r.getY() - 1.0f, 4.0f, barH * 2.0f + 3.0f); }

    if (scale)   // graduazione in dBFS sotto le barre
    {
        g.setFont (mono (7.5f));
        static const int marks[] = { 0, -6, -12, -18, -24, -36, -48 };
        const float ty = r.getY() + barH * 2.0f + 3.0f;
        float lastLabel = 1.0e9f;
        for (int k = 0; k < 7; ++k)
        {
            const float x = xOf ((float) marks[k]);
            g.setColour (dim.withAlpha (0.75f));
            g.drawVerticalLine ((int) x, ty, ty + 3.0f);
            if (std::abs (x - lastLabel) >= 15.0f || k == 0)
            {
                g.drawText (juce::String (marks[k]), juce::Rectangle<float> (x - 9.0f, ty + 3.0f, 18.0f, 9.0f).toNearestInt(), juce::Justification::centred);
                lastLabel = x;
            }
        }
    }
}

void TimelineView::drawHeader (juce::Graphics& g, int ti)
{
    const auto& t = project.getSession().tracks[(size_t) ti];
    const int y = laneTop (ti), h = heightOf (ti);
    juce::Rectangle<int> r (0, y, headerW, h);
    const bool sel = project.isTrackSelected (t.id);
    const bool armed = t.live->armed.load();

    g.setGradientFill (juce::ColourGradient (sel ? panelHi : panelTop, 0.0f, (float) y, sel ? panelTop : panel, (float) headerW, (float) y, false));
    g.fillRect (r);
    g.setColour (border);
    g.drawHorizontalLine (y + h - 1, 0.0f, (float) headerW);
    g.drawVerticalLine (headerW - 1, (float) y, (float) (y + h));

    // barra colore
    g.setColour (t.colour.withAlpha (sel ? 1.0f : 0.75f));
    g.fillRect (0, y, sel ? 6 : 4, h - 1);

    // maniglia di spostamento (sei puntini)
    g.setColour (dim.withAlpha (sel ? 0.9f : 0.55f));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            g.fillEllipse (8.0f + (float) col * 3.5f, (float) y + 12.0f + (float) row * 4.5f, 2.0f, 2.0f);

    // numero
    const auto badge = juce::Rectangle<float> (16.0f, (float) y + 10.0f, 18.0f, 18.0f);
    g.setColour (t.colour.darker (0.35f));
    g.fillEllipse (badge);
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.setFont (font (10.5f, true));
    g.drawText (juce::String (ti + 1), badge.toNearestInt(), juce::Justification::centred);

    g.setColour (text);
    g.setFont (font (13.0f, true));
    {
        int right = headerW - knobW - 8;   // il nome arriva fino al primo pulsante visibile
        if (shows (CtlSolo)) right = lockRect (ti).getX() > 0 ? juce::jmin (right, soloRect (ti).getX() - 4) : right;
        if (shows (CtlMute)) right = juce::jmin (right, muteRect (ti).getX() - 4);
        if (shows (CtlRec)) right = juce::jmin (right, armRect (ti).getX() - 4);
        if (shows (CtlLock)) right = juce::jmin (right, lockRect (ti).getX() - 4);
        g.drawText (t.name, 42, y + 9, juce::jmax (30, right - 42), 20, juce::Justification::centredLeft, true);
    }

    auto drawBtn = [&] (juce::Rectangle<int> b, const char* label, bool on, juce::Colour onCol)
    {
        const auto body = draw3dButton (g, b.toFloat().expanded (0.0f, 1.0f), on ? onCol : raisedHi, 4.0f, on, false, onCol.brighter (0.4f));
        g.setColour (on ? juce::Colours::black.withAlpha (0.85f) : dim);
        g.setFont (font (11.5f, true));
        g.drawText (label, body.toNearestInt(), juce::Justification::centred);
    };
    if (shows (CtlLock))
    {
        if (t.frozen != nullptr) drawBtn (lockRect (ti), "F", true, juce::Colour (0xff4ad0ff));   // congelata: l'audio e' quello gia' elaborato
        else drawBtn (lockRect (ti), "L", t.locked, juce::Colour (0xffffc94d));
    }
    if (shows (CtlRec)) drawBtn (armRect (ti), "R", armed, accent);
    if (shows (CtlMute)) drawBtn (muteRect (ti), "M", t.live->mute.load(), muteCol);
    if (shows (CtlSolo)) drawBtn (soloRect (ti), "S", t.live->solo.load(), soloCol);

    if (shows (CtlMeter)) drawHeaderMeter (g, ti);

    if (h >= 52 && shows (CtlTrim))
    {
        // manopola graduata del guadagno in ingresso (-24..+24 dB): trascina in alto/in basso, Maiusc = fine, doppio clic = 0 dB
        const auto kr = trimRect (ti);
        const float cx = (float) kr.getCentreX(), cy = (float) kr.getY() + 21.0f;
        const float trim = t.live->trimDb.load();
        const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
        auto angleOf = [&] (float db) { return a0 + (a1 - a0) * (db + 24.0f) / 48.0f; };
        auto pt = [&] (float ang, float rad) { return juce::Point<float> (cx + rad * std::sin (ang), cy - rad * std::cos (ang)); };
        for (int k = -24; k <= 24; k += 6)   // graduazione ogni 6 dB, il tratto dello zero piu' lungo
        {
            const float ang = angleOf ((float) k);
            const bool zero = k == 0;
            g.setColour (zero ? text.withAlpha (0.95f) : dim.withAlpha (0.8f));
            g.drawLine (juce::Line<float> (pt (ang, zero ? 15.0f : 16.5f), pt (ang, 20.5f)), zero ? 1.8f : 1.2f);
        }
        juce::Path arc;   // arco colorato dallo zero al valore attuale
        arc.addCentredArc (cx, cy, 18.5f, 18.5f, 0.0f, angleOf (0.0f), angleOf (trim), true);
        g.setColour ((trim > 0.05f ? juce::Colour (0xffff9f43) : (trim < -0.05f ? blue : dim)).withAlpha (0.85f));
        g.strokePath (arc, juce::PathStrokeType (2.2f));
        g.setGradientFill (juce::ColourGradient (scrollThumb, cx, cy - 12.0f, raised, cx, cy + 12.0f, false));
        g.fillEllipse (cx - 12.0f, cy - 12.0f, 24.0f, 24.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (cx - 12.0f, cy - 12.0f, 24.0f, 24.0f, 1.0f);
        g.setColour (juce::Colours::white);
        g.drawLine (juce::Line<float> (pt (angleOf (trim), 4.0f), pt (angleOf (trim), 11.0f)), 2.0f);
        if (h >= 72)
        {
            g.setColour (dim);
            g.setFont (mono (9.5f));
            g.drawText ((trim > 0.05f ? "+" : "") + juce::String (trim, 1) + TRANS (" dB"), juce::Rectangle<int> (kr.getX() - 4, (int) cy + 22, kr.getWidth() + 8, 12), juce::Justification::centred);
        }
    }

    if (h >= 66)
    {
        // parametro di automazione mostrato sulla traccia: clic per passare a VOL / PAN / SEND A / SEND B
        if (shows (CtlAutomation))
        {
            const auto pr = paramRect (ti).toFloat();
            const int mode = t.live->autoMode.load();
            g.setColour (mode == 0 ? raised : (mode == 2 ? accent : green).withAlpha (0.28f));
            g.fillRoundedRectangle (pr, 9.0f);
            g.setColour (mode == 0 ? border : (mode == 2 ? accent : green).withAlpha (0.9f));
            g.drawRoundedRectangle (pr.reduced (0.5f), 9.0f, 1.0f);
            g.setColour (mode == 0 ? dim : text);
            g.setFont (font (10.0f, true));
            g.drawText (autoParamName (t.live->autoParam.load()), pr.toNearestInt(), juce::Justification::centred);
        }

        // modalita' di automazione: spenta / lettura / scrittura
        if (shows (CtlAutomation))
        {
            const int mode = t.live->autoMode.load();
            const auto ar = autoRect (ti).toFloat();
            const juce::Colour ac = mode == 2 ? accent : (mode == 1 ? green : raised);
            g.setColour (mode == 0 ? ac : ac.withAlpha (0.28f));
            g.fillRoundedRectangle (ar, 9.0f);
            g.setColour (mode == 0 ? border : ac.withAlpha (0.9f));
            g.drawRoundedRectangle (ar.reduced (0.5f), 9.0f, 1.0f);
            g.setColour (mode == 0 ? dim : text);
            g.setFont (font (10.0f, true));
            g.drawText (mode == 2 ? TRANS ("SCRIVI") : (mode == 1 ? "LEGGI" : "AUTO"), ar.toNearestInt(), juce::Justification::centred);
        }

        static const char* inputNames[] = { "IN 1", "IN 2", "IN 1+2" };
        if (shows (CtlInput))
        {
        const auto ir = inputRect (ti).toFloat();
        g.setColour (armed ? accent.withAlpha (0.22f) : raised);
        g.fillRoundedRectangle (ir, 9.0f);
        g.setColour (armed ? accent.withAlpha (0.7f) : border);
        g.drawRoundedRectangle (ir.reduced (0.5f), 9.0f, 1.0f);
        g.setColour (armed ? text : dim);
        g.setFont (font (10.5f, true));
        g.drawText (inputNames[juce::jlimit (0, 2, t.live->inputMode.load())], ir.toNearestInt(), juce::Justification::centred);
        }
    }

    // maniglia di ridimensionamento
    if (hoverResize == ti || (drag == Drag::TrackResize && resizeIndex == ti))
    {
        g.setColour (blue.withAlpha (0.9f));
        g.fillRect (0, y + h - 3, headerW, 3);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        for (int i = -1; i <= 1; ++i) g.fillEllipse ((float) headerW * 0.5f + (float) i * 7.0f - 1.5f, (float) (y + h) - 2.5f, 3.0f, 3.0f);
    }
}

void TimelineView::drawVideoLane (juce::Graphics& g)
{
    const auto v = project.getSession().video;
    if (v == nullptr) return;
    const auto lanes = laneArea();
    const double rate = project.getSampleRate();

    g.setColour (well);
    g.fillRect (headerW, rulerH, lanes.getWidth() + sbSize, videoH);
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (juce::Rectangle<int> (headerW, rulerH, lanes.getWidth(), videoH));

        const double off = v->offsetSec;
        const float x0 = sampleToX ((int64_t) std::llround (off * rate)), x1 = sampleToX ((int64_t) std::llround ((off + v->duration) * rate));
        const juce::Rectangle<float> block (x0, (float) rulerH + 4.0f, x1 - x0, (float) videoH - 8.0f);
        g.setColour (raised.darker (0.2f));
        g.fillRoundedRectangle (block, 4.0f);

        if (! v->thumbs.empty() && v->thumbs.front().isValid())
        {
            juce::Graphics::ScopedSaveState ss2 (g);
            juce::Path clipPath;
            clipPath.addRoundedRectangle (block, 4.0f);
            g.reduceClipRegion (clipPath);

            const auto& first = v->thumbs.front();
            const float th = block.getHeight() - 2.0f;
            const float tileW = juce::jmax (8.0f, th * (float) first.getWidth() / (float) first.getHeight());
            const double tileSec = (double) tileW * spp / rate;
            const int n = (int) v->thumbs.size();
            const double tlLeft = xToSampleD (headerW) / rate - off, tlRight = xToSampleD (lanes.getRight()) / rate - off;
            for (int k = juce::jmax (0, (int) std::floor (tlLeft / tileSec)); (double) k * tileSec < juce::jmin (v->duration, tlRight) && k < 100000; ++k)
            {
                const int idx = juce::jlimit (0, n - 1, (int) (((double) k + 0.5) * tileSec / v->thumbInterval));
                const auto& img = v->thumbs[(size_t) idx];
                if (! img.isValid()) continue;
                const float x = sampleToX ((int64_t) std::llround ((off + (double) k * tileSec) * rate));
                g.drawImage (img, juce::Rectangle<float> (x, block.getY() + 1.0f, tileW, th), juce::RectanglePlacement::stretchToFit);
            }
            // velo sfumato per leggibilita' dei bordi
            g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), 0.0f, block.getY(),
                                                     juce::Colours::black.withAlpha (0.35f), 0.0f, block.getBottom(), false));
            g.fillRect (block);
        }
        g.setColour (hilite.withAlpha (0.8f));
        g.drawRoundedRectangle (block.reduced (0.5f), 4.0f, 1.0f);

        if (std::abs (off) > 0.0004)
        {
            const auto label = (off > 0 ? "+" : "") + juce::String ((int) std::lround (off * 1000.0)) + " ms";
            const float w = 8.0f + 6.6f * (float) label.length();
            juce::Rectangle<float> pill (juce::jmax (block.getX(), (float) headerW) + 6.0f, block.getY() + 5.0f, w, 16.0f);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.fillRoundedRectangle (pill, 8.0f);
            g.setColour (hilite);
            g.setFont (font (10.5f, true));
            g.drawText (label, pill.toNearestInt(), juce::Justification::centred);
        }
    }

    // intestazione
    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, (float) rulerH, panel, (float) headerW, (float) rulerH, false));
    g.fillRect (0, rulerH, headerW, videoH);
    g.setColour (border);
    g.drawHorizontalLine (rulerH + videoH - 1, 0.0f, (float) (getWidth() - sbSize));
    g.drawVerticalLine (headerW - 1, (float) rulerH, (float) (rulerH + videoH));
    g.setColour (hilite);
    g.fillRect (0, rulerH, 5, videoH - 1);
    g.setColour (text);
    g.setFont (font (13.0f, true));
    g.drawText (TRANS ("VIDEO"), 16, rulerH + 8, headerW - 24, 18, juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (font (11.0f));
    g.drawText (v->file.getFileName(), 16, rulerH + 27, headerW - 24, 16, juce::Justification::centredLeft, true);
    g.drawText (juce::String (v->width) + "x" + juce::String (v->height) + "  " + juce::String (v->fps, 3) + TRANS (" fps  ")
                    + juce::String (v->duration, 1) + " s",
                16, rulerH + 42, headerW - 24, 16, juce::Justification::centredLeft, true);
}

/** Clip il cui file non e' stato trovato: righe oblique rosse e il nome del file. */
void TimelineView::drawOfflineClip (juce::Graphics& g, const Clip& c, juce::Rectangle<float> wf)
{
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (wf.toNearestInt());
    g.setColour (accent.withAlpha (0.16f));
    g.fillRect (wf);
    g.setColour (accent.withAlpha (0.45f));
    for (float x = wf.getX() - wf.getHeight(); x < wf.getRight(); x += 10.0f)
        g.drawLine (x, wf.getBottom(), x + wf.getHeight(), wf.getY(), 1.0f);
    g.setColour (accent.brighter (0.4f));
    g.setFont (font (11.0f, true));
    g.drawText (TRANS ("OFFLINE - ") + c.asset->file.getFileName() + TRANS (" (File > Ricollega i media)"), wf.toNearestInt().reduced (6, 0).withX (juce::jmax (wf.toNearestInt().getX() + 6, 0)), juce::Justification::centredLeft, true);
}

void TimelineView::drawWaveformChannel (juce::Graphics& g, const Clip& c, juce::Rectangle<float> wf, juce::Rectangle<int> lanes, int channel)
{
    const auto& a = *c.asset;
    const float mid = wf.getCentreY(), half = wf.getHeight() * 0.5f - 1.0f;
    const float gain = dbToGain (c.gainDb);
    const int px0 = (int) std::floor (juce::jmax (wf.getX(), (float) lanes.getX()));
    const int px1 = (int) std::ceil (juce::jmin (wf.getRight(), (float) lanes.getRight()));
    const double clipEndAsset = (double) (c.offset + c.length);

    auto column = [&] (int px, float lo, float hi)
    {
        const float y0 = mid - juce::jlimit (-1.0f, 1.0f, hi * gain) * half;
        float y1 = mid - juce::jlimit (-1.0f, 1.0f, lo * gain) * half;
        if (y1 - y0 < 1.0f) y1 = y0 + 1.0f;
        g.fillRect (juce::Rectangle<float> ((float) px, y0, 1.0f, y1 - y0));
    };

    if (spp < 64.0)
    {
        const int nch = a.numChannels();
        const int chFrom = channel < 0 ? 0 : channel, chTo = channel < 0 ? nch : channel + 1;
        for (int px = px0; px < px1; ++px)
        {
            double sa = (double) c.offset + (xToSampleD (px) - (double) c.start);
            double sb = sa + spp;
            sa = juce::jmax (sa, (double) c.offset);
            sb = juce::jmin (sb, clipEndAsset);
            if (sb <= sa) continue;
            const int i0 = (int) std::floor (sa);
            const int i1 = juce::jmin ((int) a.length() - 1, juce::jmax (i0 + 1, (int) std::ceil (sb)));
            float lo = 0.0f, hi = 0.0f;
            for (int ch = chFrom; ch < chTo && ch < nch; ++ch)
            {
                const float* d = a.audio.getReadPointer (ch);
                for (int i = i0; i <= i1; ++i) { lo = juce::jmin (lo, d[i]); hi = juce::jmax (hi, d[i]); }
            }
            column (px, lo, hi);
        }
        return;
    }

    const PeakLevel* L = &a.levels.front();
    for (auto& lv : a.levels) if ((double) lv.block <= spp) L = &lv;
    const int nb = (int) L->mn.size();

    for (int px = px0; px < px1; ++px)
    {
        double sa = (double) c.offset + (xToSampleD (px) - (double) c.start);
        double sb = sa + spp;
        sa = juce::jmax (sa, (double) c.offset);
        sb = juce::jmin (sb, clipEndAsset);
        if (sb <= sa) continue;
        const int b0 = juce::jlimit (0, nb - 1, (int) (sa / L->block));
        const int b1 = juce::jlimit (b0, nb - 1, (int) ((sb - 1.0) / L->block));
        float lo = 0.0f, hi = 0.0f;
        for (int b = b0; b <= b1; ++b)
        {
            if (channel != 1) { lo = juce::jmin (lo, L->mn[(size_t) b]); hi = juce::jmax (hi, L->mx[(size_t) b]); }
            if ((channel == 1 || channel < 0) && ! L->mn1.empty()) { lo = juce::jmin (lo, L->mn1[(size_t) b]); hi = juce::jmax (hi, L->mx1[(size_t) b]); }
        }
        column (px, lo, hi);
    }
}

void TimelineView::drawWaveform (juce::Graphics& g, const Clip& c, juce::Rectangle<float> wf, juce::Rectangle<int> lanes)
{
    // audio stereo: i due canali separati (sinistro sopra, destro sotto); audio mono: una sola onda
    if (c.asset->numChannels() >= 2 && wf.getHeight() >= 40.0f)
    {
        const float h2 = (wf.getHeight() - 2.0f) * 0.5f;
        drawWaveformChannel (g, c, wf.withHeight (h2), lanes, 0);
        drawWaveformChannel (g, c, wf.withTrimmedTop (h2 + 2.0f), lanes, 1);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (wf.getX(), wf.getY() + h2, wf.getWidth(), 2.0f);
    }
    else
        drawWaveformChannel (g, c, wf, lanes, -1);
}

void TimelineView::drawClip (juce::Graphics& g, const Track& t, int ti, const Clip& c, juce::Rectangle<int> lanes)
{
    const bool preview = c.id == stretchId && (drag == Drag::Stretch || stretchHoldId == c.id) && stretchImg.isValid();
    if (preview)
    {
        // ombra dell'originale, tratteggiata, per vedere di quanto si sta allungando
        const auto orig = clipRect (ti, c);
        juce::Path o;
        o.addRectangle (orig);
        juce::Path dashed;
        const float dl[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, o, dl, 2);
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.fillPath (dashed);
    }
    Clip shown = c;
    if (preview) { shown.start = stretchStart; shown.length = stretchLen; }
    const auto r = clipRect (ti, shown);
    if (r.getRight() < (float) lanes.getX() || r.getX() > (float) lanes.getRight()) return;
    if (r.getBottom() < (float) lanes.getY() || r.getY() > (float) lanes.getBottom()) return;

    const bool sel = project.selection.count (c.id) > 0;
    const auto col = t.colour;
    const float rad = 5.0f;

    // ombra
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillRoundedRectangle (r.translated (0.0f, 1.5f), rad);

    juce::Graphics::ScopedSaveState ss (g);
    juce::Path shape;
    shape.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), rad);
    g.reduceClipRegion (lanes);
    g.reduceClipRegion (shape);

    // corpo
    g.setGradientFill (juce::ColourGradient (col.darker (0.62f), 0.0f, r.getY(), col.darker (0.86f), 0.0f, r.getBottom(), false));
    g.fillRect (r);

    // intestazione
    const juce::Rectangle<float> head = r.withHeight (17.0f);
    g.setGradientFill (juce::ColourGradient (sel ? col.brighter (0.15f) : col.withMultipliedBrightness (0.92f), 0.0f, head.getY(),
                                             sel ? col : col.withMultipliedBrightness (0.72f), 0.0f, head.getBottom(), false));
    g.fillRect (head);
    g.setColour (juce::Colours::black.withAlpha (0.82f));
    g.setFont (font (11.0f, true));
    const juce::String label = (c.cueId != 0 ? "T" + juce::String (c.takeNo) + "  " : juce::String()) + c.asset->name + (std::abs (c.gainDb) > 0.05f ? "   " + juce::String (c.gainDb, 1) + " dB" : juce::String())
                             + (preview ? "     STRETCH " + juce::String (100.0 * (double) stretchLen / (double) juce::jmax<int64_t> (1, stretchOrigLen), 1) + " %   " + juce::String ((double) stretchLen / project.getSampleRate(), 2) + " s" : juce::String());
    auto labelBox = head.toNearestInt().withTrimmedLeft (7).withTrimmedRight (4);
    if (labelBox.getX() < lanes.getX() + 4) labelBox.setLeft (juce::jmin (labelBox.getRight(), lanes.getX() + 4));
    g.drawText (label, labelBox, juce::Justification::centredLeft, true);

    // forma d'onda
    auto wf = r.withTrimmedTop (18.0f).reduced (0.0f, 3.0f);
    g.setColour (col.brighter (0.8f).withAlpha (0.92f));
    if (preview) g.drawImage (stretchImg, wf, juce::RectanglePlacement::stretchToFit);   // anteprima in tempo reale: l'onda scalata
    else if (c.asset->offline) drawOfflineClip (g, c, wf);
    else drawWaveform (g, c, wf, lanes);

    g.setColour (col.brighter (0.4f).withAlpha (0.22f));
    g.drawHorizontalLine ((int) wf.getCentreY(), r.getX(), r.getRight());

    // dissolvenze
    const auto shade = juce::Colours::black.withAlpha (0.58f);
    if (c.fadeIn > 0)
    {
        const float fx = r.getX() + (float) ((double) c.fadeIn / spp);
        juce::Path p;
        p.addTriangle (r.getX(), wf.getY(), fx, wf.getY(), r.getX(), wf.getBottom());
        g.setColour (shade);
        g.fillPath (p);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawLine (r.getX(), wf.getBottom(), fx, wf.getY(), 1.2f);
    }
    if (c.fadeOut > 0)
    {
        const float fx = r.getRight() - (float) ((double) c.fadeOut / spp);
        juce::Path p;
        p.addTriangle (r.getRight(), wf.getY(), fx, wf.getY(), r.getRight(), wf.getBottom());
        g.setColour (shade);
        g.fillPath (p);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawLine (fx, wf.getY(), r.getRight(), wf.getBottom(), 1.2f);
    }

    if (c.muted)   // take ADR non attivo: suona solo quello scelto
    {
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRect (r.withTrimmedTop (17.0f));
    }

    if (sel && r.getWidth() > 30.0f)
    {
        const float fi = r.getX() + (float) ((double) c.fadeIn / spp);
        const float fo = r.getRight() - (float) ((double) c.fadeOut / spp);
        for (float hx : { fi, fo })
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (hx - 5.0f, r.getY() + 19.5f, 10.0f, 10.0f);
            g.setColour (juce::Colours::white);
            g.fillEllipse (hx - 4.0f, r.getY() + 20.0f, 8.0f, 8.0f);
        }
    }

    g.restoreState();
    g.saveState();
    g.reduceClipRegion (lanes);
    if (sel)
    {
        g.setColour (col.withAlpha (0.35f));
        g.drawRoundedRectangle (r.expanded (1.5f), rad + 1.0f, 3.0f);
    }
    g.setColour (sel ? juce::Colours::white : juce::Colours::black.withAlpha (0.65f));
    g.drawRoundedRectangle (r.reduced (0.5f), rad, sel ? 1.6f : 1.0f);
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    const auto& s = project.getSession();
    const auto lanes = laneArea();
    const int nTracks = (int) s.tracks.size();

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (lanes);

        for (int i = 0; i < nTracks; ++i)
        {
            const int y = laneTop (i), h = heightOf (i);
            if (h == 0 || y + h < lanes.getY() || y > lanes.getBottom()) continue;
            g.setColour (i % 2 == 0 ? lane : laneAlt);
            g.fillRect (lanes.getX(), y, lanes.getWidth(), h);
            if (s.tracks[(size_t) i].editLocked())
            {
                g.setColour ((s.tracks[(size_t) i].frozen != nullptr ? juce::Colour (0xff4ad0ff) : juce::Colour (0xffffc94d)).withAlpha (0.045f));
                g.fillRect (lanes.getX(), y, lanes.getWidth(), h);
            }
            if (project.isTrackSelected (s.tracks[(size_t) i].id))
            {
                g.setColour (juce::Colours::white.withAlpha (0.022f));
                g.fillRect (lanes.getX(), y, lanes.getWidth(), h);
            }
            if (s.tracks[(size_t) i].showLanes)
                for (int k = 1; k < lanesOf (i).count; ++k)
                {
                    g.setColour (border.withAlpha (0.7f));
                    g.drawHorizontalLine (y + k * laneHeight (i), (float) lanes.getX(), (float) lanes.getRight());
                }
            g.setColour (border);
            g.drawHorizontalLine (y + h - 1, (float) lanes.getX(), (float) lanes.getRight());
        }

        // griglia verticale in linea con il righello
        {
            const double rate = project.getSampleRate();
            const double step = rulerStep();
            g.setColour (grid.withAlpha (0.5f));
            for (double t = std::floor (xToSampleD (headerW) / rate / step) * step; t <= xToSampleD (lanes.getRight()) / rate; t += step)
                g.drawVerticalLine ((int) sampleToX ((int64_t) std::llround (t * rate)), (float) lanes.getY(), (float) lanes.getBottom());
        }

        bool anyClip = false;
        for (int i = 0; i < nTracks; ++i)
        {
            const int y = laneTop (i), h = heightOf (i);
            if (h == 0 || y + h < lanes.getY() || y > lanes.getBottom()) { anyClip |= ! s.tracks[(size_t) i].clips.empty(); continue; }
            for (auto& c : s.tracks[(size_t) i].clips)
            {
                anyClip = true;
                drawClip (g, s.tracks[(size_t) i], i, c, lanes);
            }
        }

        // sovrapposizioni tra clip della stessa traccia: crossfade automatico
        for (int i = 0; i < nTracks; ++i)
        {
            const auto& cl = s.tracks[(size_t) i].clips;
            if (heightOf (i) == 0 || s.tracks[(size_t) i].showLanes) continue;   // con le corsie i clip sovrapposti sono su righe diverse
            for (size_t a = 0; a < cl.size(); ++a)
                for (size_t b = 0; b < cl.size(); ++b)
                {
                    if (a == b || ! (cl[a].start < cl[b].start && cl[b].start < cl[a].end() && cl[a].end() <= cl[b].end())) continue;
                    const float x0 = sampleToX (cl[b].start), x1 = sampleToX (cl[a].end());
                    if (x1 < (float) lanes.getX() || x0 > (float) lanes.getRight() || x1 - x0 < 2.0f) continue;
                    const float y0 = (float) laneTop (i) + 4.0f, y1 = y0 + (float) heightOf (i) - 8.0f;
                    g.setColour (juce::Colours::white.withAlpha (0.16f));
                    g.fillRect (x0, y0, x1 - x0, y1 - y0);
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.drawLine (x0, y1, x1, y0, 1.4f);
                    g.drawLine (x0, y0 + 17.0f, x1, y1, 1.4f);
                }
        }

        if (! anyClip)
        {
            g.setColour (dim);
            g.setFont (font (15.0f));
            g.drawText (TRANS ("Trascina qui i file audio o un video oppure usa File > Importa audio (") + juce::String (juce::CharPointer_UTF8 ("\xe2\x8c\x98")) + "I)",
                        lanes, juce::Justification::centred);
        }

        // curve di automazione (volume, pan, send): quella scelta su ogni traccia
        for (int i = 0; i < nTracks; ++i)
        {
            const auto& tr = s.tracks[(size_t) i];
            const int mode = tr.live->autoMode.load();
            const int y = laneTop (i), h = heightOf (i);
            const bool showAlways = tool == Tool::Automation && tr.id == project.selectedTrackId;   // con la traccia selezionata la linea si vede subito, anche a zero
            if ((mode == 0 && ! showAlways) || h == 0 || y + h < lanes.getY() || y > lanes.getBottom()) continue;
            const int param = tr.live->autoParam.load();
            const auto& lane = tr.lane (param);
            const juce::Colour lc = mode == 2 ? accent : (mode == 1 ? green : blue.brighter (0.3f));
            g.setColour (lc.withAlpha (0.85f));
            g.setFont (font (9.5f, true));
            g.drawText (TRANS ("AUTOMAZIONE: ") + juce::String (autoParamName (param)), juce::Rectangle<int> (lanes.getX() + 6, y + 1, 160, 12), juce::Justification::centredLeft);
            if (lane.empty())
            {
                const float cur = param == AutoPan ? tr.live->pan.load() : (param == AutoVolume ? tr.live->volumeDb.load() : tr.live->sendDb[param - AutoSendA].load());
                const float yy = autoYForValue (i, param, cur);
                g.setColour (lc.withAlpha (0.45f));
                g.drawHorizontalLine ((int) yy, (float) lanes.getX(), (float) lanes.getRight());
                continue;
            }
            juce::Path line;
            line.startNewSubPath ((float) lanes.getX(), autoYForValue (i, param, lane.front().db));
            for (auto& ap : lane) line.lineTo (sampleToX (ap.pos), autoYForValue (i, param, ap.db));
            line.lineTo ((float) lanes.getRight(), autoYForValue (i, param, lane.back().db));
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.strokePath (line, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved));
            g.setColour (lc);
            g.strokePath (line, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
            for (auto& ap : lane)
            {
                const float px = sampleToX (ap.pos), py = autoYForValue (i, param, ap.db);
                if (px < (float) lanes.getX() - 6.0f || px > (float) lanes.getRight() + 6.0f) continue;
                const bool hot = drag == Drag::AutoPoint && ap.id == autoPointId;
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.fillEllipse (px - 5.0f, py - 5.0f, 10.0f, 10.0f);
                g.setColour (hot ? juce::Colours::white : lc);
                g.fillEllipse (px - 3.5f, py - 3.5f, 7.0f, 7.0f);
                if (hot)
                {
                    juce::String label;
                    if (param == AutoPan) label = std::abs (ap.db) < 0.01f ? "C" : (ap.db < 0 ? "L" + juce::String ((int) std::round (-ap.db * 100)) : "R" + juce::String ((int) std::round (ap.db * 100)));
                    else label = ap.db <= -79.5f ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) : juce::String (ap.db, 1) + " dB";
                    g.setColour (juce::Colours::white);
                    g.setFont (font (10.0f, true));
                    g.drawText (label, juce::Rectangle<float> (px + 8.0f, py - 8.0f, 60.0f, 14.0f).toNearestInt(), juce::Justification::centredLeft);
                }
            }
        }

        // regione di loop: fascia azzurra su tutte le tracce
        if (engine.getLoopEnd() > engine.getLoopStart())
        {
            const float lx0 = juce::jmax ((float) lanes.getX(), sampleToX (engine.getLoopStart())), lx1 = juce::jmin ((float) lanes.getRight(), sampleToX (engine.getLoopEnd()));
            if (lx1 > lx0)
            {
                g.setColour (blue.withAlpha (engine.isLooping() ? 0.13f : 0.06f));
                g.fillRect (lx0, (float) lanes.getY(), lx1 - lx0, (float) lanes.getHeight());
                g.setColour (blue.withAlpha (engine.isLooping() ? 0.8f : 0.4f));
                if (sampleToX (engine.getLoopStart()) >= (float) lanes.getX()) g.drawVerticalLine ((int) sampleToX (engine.getLoopStart()), (float) lanes.getY(), (float) lanes.getBottom());
                if (sampleToX (engine.getLoopEnd()) <= (float) lanes.getRight()) g.drawVerticalLine ((int) sampleToX (engine.getLoopEnd()), (float) lanes.getY(), (float) lanes.getBottom());
            }
        }

        // intervallo di esportazione: fuori dall'intervallo la timeline e' oscurata
        if (project.exportOut > project.exportIn && project.exportIn >= 0)
        {
            const float ex0 = sampleToX (project.exportIn), ex1 = sampleToX (project.exportOut);
            const float lx = (float) lanes.getX(), rx = (float) lanes.getRight();
            g.setColour (juce::Colours::black.withAlpha (0.34f));
            if (ex0 > lx) g.fillRect (lx, (float) lanes.getY(), juce::jmin (ex0, rx) - lx, (float) lanes.getHeight());
            if (ex1 < rx) g.fillRect (juce::jmax (ex1, lx), (float) lanes.getY(), rx - juce::jmax (ex1, lx), (float) lanes.getHeight());
            g.setColour (juce::Colour (0xffff9f43).withAlpha (0.85f));
            for (float ex : { ex0, ex1 }) if (ex >= lx && ex <= rx) g.drawVerticalLine ((int) ex, (float) lanes.getY(), (float) lanes.getBottom());
        }

        // cue ADR: fascia dall'ingresso all'uscita, colore secondo lo stato
        for (auto& cue : s.cues)
        {
            const float cx0 = sampleToX (cue.inPos), cx1 = sampleToX (cue.outPos);
            if (cx1 < (float) lanes.getX() || cx0 > (float) lanes.getRight()) continue;
            const auto cc = cue.status == 1 ? blue : cue.status == 2 ? green : cue.status == 3 ? accent : juce::Colour (0xffffc94d);
            const bool sel = cue.id == project.selectedCueId;
            const float a = juce::jmax ((float) lanes.getX(), cx0), b = juce::jmin ((float) lanes.getRight(), cx1);
            g.setColour (cc.withAlpha (sel ? 0.16f : 0.08f));
            g.fillRect (a, (float) lanes.getY(), b - a, (float) lanes.getHeight());
            g.setColour (cc.withAlpha (sel ? 0.30f : 0.16f));
            g.fillRect (a, (float) lanes.getY(), b - a, (float) cueStripH);   // fascia da afferrare per spostare il cue
            g.setColour (cc.withAlpha (sel ? 0.9f : 0.5f));
            g.drawVerticalLine ((int) cx0, (float) lanes.getY(), (float) lanes.getBottom());
            g.drawVerticalLine ((int) cx1, (float) lanes.getY(), (float) lanes.getBottom());
            for (float ex : { cx0, cx1 })   // maniglie dei bordi
                if (ex >= (float) lanes.getX() && ex <= (float) lanes.getRight()) g.fillRect (ex - 1.5f, (float) lanes.getY(), 3.0f, (float) cueStripH);
            if (cx1 - cx0 > 30.0f)
            {
                g.setFont (font (10.5f, true));
                g.drawText ("#" + juce::String (cue.number) + " " + cue.character, juce::Rectangle<float> (a + 6.0f, (float) lanes.getY() + 1.0f, b - a - 10.0f, (float) cueStripH - 1.0f).toNearestInt(),
                            juce::Justification::centredLeft, true);
            }
        }

        // strumento Range: intervallo di tempo sulle tracce scelte
        if (project.range.valid())
        {
            const auto& rg = project.range;
            const float rx0 = juce::jmax ((float) lanes.getX(), sampleToX (rg.a)), rx1 = juce::jmin ((float) lanes.getRight(), sampleToX (rg.b));
            if (rx1 > rx0)
                for (int i = rg.first; i <= juce::jmin (rg.last, nTracks - 1); ++i)
                {
                    if (heightOf (i) == 0) continue;
                    g.setColour (blue.withAlpha (0.20f));
                    g.fillRect (rx0, (float) laneTop (i), rx1 - rx0, (float) heightOf (i));
                    g.setColour (blue.withAlpha (0.9f));
                    g.drawRect (rx0, (float) laneTop (i), rx1 - rx0, (float) heightOf (i), 1.0f);
                }
        }
        if (drag == Drag::Comp && compB > compA)
            if (const int ci = project.trackIndexForId (compTrackId); ci >= 0)
            {
                const float cx0 = sampleToX (compA), cx1 = sampleToX (compB);
                g.setColour (green.withAlpha (0.25f));
                g.fillRect (cx0, (float) laneTop (ci), cx1 - cx0, (float) heightOf (ci));
                g.setColour (green);
                g.drawRect (cx0, (float) laneTop (ci), cx1 - cx0, (float) heightOf (ci), 1.5f);
            }

        // linee dei marker
        for (auto& m : s.markers)
        {
            const float mx = sampleToX (m.pos);
            if (mx < (float) lanes.getX() || mx > (float) lanes.getRight()) continue;
            g.setColour (m.colour.withAlpha (0.30f));
            g.drawVerticalLine ((int) mx, (float) lanes.getY(), (float) lanes.getBottom());
        }

        // registrazione in corso: blocco rosso che cresce fino alla testina
        if (engine.isRecording() && engine.getRecordStart() >= 0)
        {
            const float rx0 = sampleToX (engine.getRecordStart()), rx1 = sampleToX (engine.getPosition());
            for (int i = 0; i < nTracks; ++i)
            {
                if (! s.tracks[(size_t) i].live->armed.load() || heightOf (i) == 0) continue;
                juce::Rectangle<float> rr (rx0, (float) laneTop (i) + 4.0f, juce::jmax (2.0f, rx1 - rx0), (float) heightOf (i) - 8.0f);
                g.setColour (accent.withAlpha (0.26f));
                g.fillRoundedRectangle (rr, 5.0f);
                g.setColour (accent);
                g.drawRoundedRectangle (rr, 5.0f, 1.5f);
                g.setFont (font (11.0f, true));
                g.drawText (TRANS ("REC"), rr.toNearestInt().withTrimmedLeft (8), juce::Justification::topLeft);
            }
        }

        // loop (velo)
        if (engine.isLooping())
        {
            const float x0 = sampleToX (engine.getLoopStart()), x1 = sampleToX (engine.getLoopEnd());
            g.setColour (blue.withAlpha (0.07f));
            g.fillRect (x0, (float) lanes.getY(), x1 - x0, (float) lanes.getHeight());
            g.setColour (blue.withAlpha (0.5f));
            g.drawVerticalLine ((int) x0, (float) lanes.getY(), (float) lanes.getBottom());
            g.drawVerticalLine ((int) x1, (float) lanes.getY(), (float) lanes.getBottom());
        }

        if (! marquee.isEmpty())
        {
            g.setColour (blue.withAlpha (0.15f));
            g.fillRect (marquee);
            g.setColour (blue.withAlpha (0.85f));
            g.drawRect (marquee);
        }

        if (dropHighlight)
        {
            g.setColour (blue.withAlpha (0.16f));
            g.fillRect (lanes);
            g.setColour (blue);
            g.drawRect (lanes, 2);
        }

        // bordo di ridimensionamento evidenziato
        if (hoverResize >= 0 || (drag == Drag::TrackResize && resizeIndex >= 0))
        {
            const int idx = drag == Drag::TrackResize ? resizeIndex : hoverResize;
            const int by = laneTop (idx) + heightOf (idx) - 2;
            g.setColour (blue.withAlpha (0.9f));
            g.fillRect (lanes.getX(), by, lanes.getWidth(), 3);
        }
    }

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (0, lanes.getY(), headerW, lanes.getHeight());
        for (int i = 0; i < nTracks; ++i)
            if (heightOf (i) > 0 && laneTop (i) + heightOf (i) >= lanes.getY() && laneTop (i) <= lanes.getBottom()) drawHeader (g, i);
    }
    drawGroupBars (g);

    drawVideoLane (g);
    drawRuler (g);

    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, 0.0f, panel, 0.0f, (float) rulerH, false));
    g.fillRect (0, 0, headerW, rulerH);
    g.setColour (border);
    g.drawHorizontalLine (rulerH - 1, 0.0f, (float) headerW);
    g.drawVerticalLine (headerW - 1, 0.0f, (float) rulerH);
    drawCaption (g, TRANS ("Tracce"), juce::Rectangle<int> (16, 0, headerW - 16 - 104, rulerH));
    {
        const auto& s = project.getSession();
        bool anyHidden = false;
        for (auto& t : s.tracks) anyHidden = anyHidden || t.hidden;
        auto btn = [&] (int part, const char* label, bool on, juce::Colour col)
        {
            const auto body = draw3dButton (g, globalRect (part).toFloat(), on ? col : raisedHi, 4.0f, on, false, col.brighter (0.4f));
            g.setColour (on ? juce::Colours::black.withAlpha (0.85f) : dim);
            g.setFont (font (11.0f, true));
            g.drawText (label, body.toNearestInt(), juce::Justification::centred);
        };
        btn (0, "M", project.anyTrackMuted(), muteCol);       // clic: toglie tutti i mute
        btn (1, "S", project.anyTrackSoloed(), soloCol);      // clic: toglie tutti i solo
        btn (2, "V", anyHidden, blue);                        // clic: tracce visibili / nascoste
    }

    // riordino delle tracce: linea di destinazione
    if (drag == Drag::TrackReorder && reorderTo >= 0 && reorderFrom >= 0 && reorderTo != reorderFrom)
    {
        const int y = laneTop (reorderTo) + (reorderTo > reorderFrom ? heightOf (reorderTo) : 0);
        g.setColour (blue);
        g.fillRect (0, y - 1, getWidth() - sbSize, 3);
    }

    // guida dello strumento Lama
    if (! razorGuide.isEmpty())
    {
        g.setColour (accent.withAlpha (0.95f));
        g.fillRect (razorGuide.getX(), razorGuide.getY(), 1, razorGuide.getHeight());
        g.setColour (accent.withAlpha (0.25f));
        g.fillRect (razorGuide.getX() - 1, razorGuide.getY(), 3, razorGuide.getHeight());
    }

    // testina
    const float px = sampleToX (engine.getPosition());
    if (px >= (float) headerW - 1.0f && px <= (float) lanes.getRight())
    {
        g.setColour (playhead.withAlpha (0.22f));
        g.fillRect (px - 2.0f, 0.0f, 5.0f, (float) lanes.getBottom());
        g.setColour (playhead);
        g.drawVerticalLine ((int) px, 0.0f, (float) lanes.getBottom());
        juce::Path flag;
        flag.addRoundedRectangle (px - 6.5f, 0.0f, 13.0f, 11.0f, 2.5f);
        flag.addTriangle (px - 6.5f, 8.0f, px + 6.5f, 8.0f, px, 15.0f);
        g.fillPath (flag);
    }
}

//==============================================================================
TimelineView::CueHit TimelineView::cueHitTest (juce::Point<int> p, bool onEmptyLane) const
{
    const auto lanes = laneArea();
    if (! lanes.contains (p)) return {};
    const bool strip = p.y < lanes.getY() + cueStripH;
    if (! strip && ! onEmptyLane) return {};
    const int grab = 5;
    CueHit best;
    float bestDist = 1.0e9f;
    for (auto& cue : project.getSession().cues)
    {
        const float x0 = sampleToX (cue.inPos), x1 = sampleToX (cue.outPos);
        const float px = (float) p.x;
        auto offer = [&] (Drag k, float d) { if (d < bestDist) { bestDist = d; best = { cue.id, k }; } };
        if (std::abs (px - x0) <= grab) offer (Drag::CueIn, std::abs (px - x0));
        if (std::abs (px - x1) <= grab) offer (Drag::CueOut, std::abs (px - x1));
        if (strip && px > x0 + grab && px < x1 - grab) offer (Drag::CueMove, 100.0f);   // i bordi hanno la precedenza sul corpo
    }
    return best;
}

TimelineView::Hit TimelineView::hitTest (juce::Point<int> p) const
{
    Hit h;
    const auto lanes = laneArea();
    if (p.y < rulerH)
    {
        if (p.x < headerW)
        {
            if (globalRect (0).contains (p)) h.type = HitType::GlobalMute;
            else if (globalRect (1).contains (p)) h.type = HitType::GlobalSolo;
            else if (globalRect (2).contains (p)) h.type = HitType::GlobalVisibility;
            return h;
        }
        if (p.x >= headerW && p.y < exportH) { h.type = HitType::ExportBar; return h; }
        if (p.x >= headerW)
        {
            h.type = HitType::Ruler;
            if (p.y >= 14 + exportH)
                for (auto& m : project.getSession().markers)
                    if (std::abs ((float) p.x - sampleToX (m.pos)) <= 7.0f) { h.type = HitType::Marker; h.markerId = m.id; break; }
        }
        return h;
    }
    if (p.y < lanesTop())
    {
        if (p.x >= headerW && p.x <= lanes.getRight()) h.type = HitType::Video;
        return h;
    }
    if (p.y > lanes.getBottom() || p.x > lanes.getRight()) return h;

    const auto& tracks = project.getSession().tracks;
    const int n = (int) tracks.size();

    // bordi tra le tracce: ridimensionamento
    for (int i = 0; i < n; ++i)
    {
        const int bottom = laneTop (i) + heightOf (i);
        if (heightOf (i) > 0 && std::abs (p.y - bottom) <= 3 && bottom >= lanes.getY())
        {
            h.type = HitType::TrackResize;
            h.trackIndex = i;
            h.track = tracks[(size_t) i].id;
            return h;
        }
    }

    // barra di un gruppo (sopra la prima traccia): freccia/clic = comprimi, M e S = muto/solo di tutto il gruppo
    for (int i = 0; i < n; ++i)
        if (bandOf (i) > 0 && p.y >= laneTop (i) - groupBandH && p.y < laneTop (i))
        {
            h.type = HitType::GroupBar;
            h.group = tracks[(size_t) i].groupId;
            h.trackIndex = i;
            h.groupPart = p.x >= headerW - 66 && p.x < headerW - 40 ? 1 : (p.x >= headerW - 36 && p.x < headerW - 10 ? 2 : 0);
            return h;
        }

    const int ti = trackIndexAtY (p.y);
    if (ti < 0 || ti >= n)
    {
        h.type = p.x < headerW ? HitType::None : HitType::Lane;
        return h;
    }
    h.track = tracks[(size_t) ti].id;
    h.trackIndex = ti;

    if (p.x < headerW)
    {
        h.type = HitType::Header;
        if (shows (CtlLock) && lockRect (ti).contains (p)) h.type = HitType::HeaderLock;
        else if (shows (CtlRec) && armRect (ti).contains (p)) h.type = HitType::HeaderArm;
        else if (shows (CtlInput) && heightOf (ti) >= 66 && inputRect (ti).contains (p)) h.type = HitType::HeaderInput;
        else if (shows (CtlAutomation) && heightOf (ti) >= 66 && autoRect (ti).contains (p)) h.type = HitType::HeaderAuto;
        else if (shows (CtlAutomation) && heightOf (ti) >= 66 && paramRect (ti).contains (p)) h.type = HitType::HeaderAutoParam;
        else if (shows (CtlTrim) && heightOf (ti) >= 52 && trimRect (ti).contains (p)) h.type = HitType::HeaderTrim;
        else if (shows (CtlMute) && muteRect (ti).contains (p)) h.type = HitType::HeaderMute;
        else if (shows (CtlSolo) && soloRect (ti).contains (p)) h.type = HitType::HeaderSolo;
        else h.type = HitType::HeaderGrip;   // tutta l'intestazione si puo' trascinare per spostare la traccia
        return h;
    }

    h.type = HitType::Lane;
    const auto& clips = tracks[(size_t) ti].clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it)
    {
        const auto r = clipRect (ti, *it);
        if (! r.expanded (4.0f, 0.0f).contains (p.toFloat())) continue;

        h.clip = it->id;
        const bool sel = project.selection.count (it->id) > 0;
        if (sel && r.getWidth() > 30.0f)
        {
            const float fi = r.getX() + (float) ((double) it->fadeIn / spp);
            const float fo = r.getRight() - (float) ((double) it->fadeOut / spp);
            if (std::abs ((float) p.x - fi) < 7.0f && std::abs ((float) p.y - (r.getY() + 24.0f)) < 8.0f) { h.type = HitType::ClipFadeIn; return h; }
            if (std::abs ((float) p.x - fo) < 7.0f && std::abs ((float) p.y - (r.getY() + 24.0f)) < 8.0f) { h.type = HitType::ClipFadeOut; return h; }
        }
        if (r.getWidth() > 20.0f && (float) p.x - r.getX() < 6.0f) h.type = HitType::ClipTrimL;
        else if (r.getWidth() > 20.0f && r.getRight() - (float) p.x < 6.0f) h.type = HitType::ClipTrimR;
        else h.type = HitType::Clip;
        return h;
    }
    return h;
}

// mappa valore <-> altezza. Volume: espande la zona intorno a 0 dB (0 dB sta al 27% dall'alto).
// Pan: lineare, destra in alto. Send: lineare da 0 dB (alto) a -60 dB, in fondo spento.
float TimelineView::autoYForValue (int ti, int param, float v) const
{
    float frac;
    if (param == AutoPan) frac = (1.0f - juce::jlimit (-1.0f, 1.0f, v)) * 0.5f;
    else if (param == AutoVolume) frac = std::pow (juce::jlimit (0.0f, 1.0f, (6.0f - v) / 66.0f), 0.55f);
    else frac = v <= -79.5f ? 1.0f : juce::jlimit (0.0f, 1.0f, -v / 60.0f) * 0.985f;
    return (float) laneTop (ti) + 8.0f + frac * (float) (heightOf (ti) - 16);
}

float TimelineView::autoValueForY (int ti, int param, int y) const
{
    const float frac = juce::jlimit (0.0f, 1.0f, ((float) y - (float) laneTop (ti) - 8.0f) / (float) juce::jmax (1, heightOf (ti) - 16));
    if (param == AutoPan) { const float v = 1.0f - 2.0f * frac; return std::abs (v) < 0.03f ? 0.0f : v; }   // vicino al centro si aggancia a C
    if (frac > 0.985f) return -80.0f;
    if (param == AutoVolume) return 6.0f - 66.0f * std::pow (frac, 1.0f / 0.55f);
    return -60.0f * frac / 0.985f;
}

bool TimelineView::findAutoSegmentAt (juce::Point<int> p, int& trackIndex, int& idA, int& idB) const
{
    const auto& tracks = project.getSession().tracks;
    const int ti = trackIndexAtY (p.y);
    if (ti < 0 || ti >= (int) tracks.size()) return false;
    const int param = tracks[(size_t) ti].live->autoParam.load();
    const auto& lane = tracks[(size_t) ti].lane (param);
    for (size_t i = 0; i + 1 < lane.size(); ++i)
    {
        const float x0 = sampleToX (lane[i].pos), x1 = sampleToX (lane[i + 1].pos);
        if ((float) p.x < x0 || (float) p.x > x1 || x1 - x0 < 1.0f) continue;
        const float y0 = autoYForValue (ti, param, lane[i].db), y1 = autoYForValue (ti, param, lane[i + 1].db);
        const float yl = y0 + ((float) p.x - x0) / (x1 - x0) * (y1 - y0);
        if (std::abs ((float) p.y - yl) <= 6.0f) { trackIndex = ti; idA = lane[i].id; idB = lane[i + 1].id; return true; }
    }
    return false;
}

bool TimelineView::findAutoPointAt (juce::Point<int> p, int& trackIndex, int& pointId) const
{
    const auto& tracks = project.getSession().tracks;
    const int ti = trackIndexAtY (p.y);
    if (ti < 0 || ti >= (int) tracks.size()) return false;
    const int param = tracks[(size_t) ti].live->autoParam.load();
    for (auto& ap : tracks[(size_t) ti].lane (param))
    {
        const float x = sampleToX (ap.pos), y = autoYForValue (ti, param, ap.db);
        if (std::abs ((float) p.x - x) <= 8.0f && std::abs ((float) p.y - y) <= 8.0f) { trackIndex = ti; pointId = ap.id; return true; }
    }
    return false;
}

void TimelineView::setTool (Tool t)
{
    if (tool == t) return;
    tool = t;
    razorGuide = {};
    setMouseCursor (juce::MouseCursor::NormalCursor);
    if (onToolChanged) onToolChanged (t);
    repaint();
}

void TimelineView::buildSnapPoints (bool excludeSelected)
{
    snapPoints = { 0, engine.getPosition() };
    if (engine.getLoopEnd() > engine.getLoopStart()) { snapPoints.push_back (engine.getLoopStart()); snapPoints.push_back (engine.getLoopEnd()); }
    for (auto& cue : project.getSession().cues) { snapPoints.push_back (cue.inPos); snapPoints.push_back (cue.outPos); }
    if (project.exportOut > project.exportIn && project.exportIn >= 0) { snapPoints.push_back (project.exportIn); snapPoints.push_back (project.exportOut); }
    for (auto& t : project.getSession().tracks)
        for (auto& c : t.clips)
            if (! excludeSelected || project.selection.count (c.id) == 0) { snapPoints.push_back (c.start); snapPoints.push_back (c.end()); }
    std::sort (snapPoints.begin(), snapPoints.end());
}

bool TimelineView::handleToolMouseDown (const juce::MouseEvent& e, const Hit& hit)
{
    const bool onClip = hit.clip != 0 && (hit.type == HitType::Clip || hit.type == HitType::ClipTrimL || hit.type == HitType::ClipTrimR
                                          || hit.type == HitType::ClipFadeIn || hit.type == HitType::ClipFadeOut);
    const bool inLanes = onClip || hit.type == HitType::Lane;

    switch (tool)
    {
        case Tool::Razor:
        {
            if (! inLanes) return false;
            buildSnapPoints (false);
            int64_t pos = xToSample (e.x), snapped = 0;
            if (snapTo (pos, snapped)) pos = snapped;
            if (e.mods.isShiftDown()) project.splitAt (pos, true);
            else if (onClip) project.splitClip (hit.clip, pos);
            return true;
        }

        case Tool::Slip:
        {
            if (! onClip) return false;
            if (project.trackLocked (hit.track)) return true;
            project.selectedTrackId = hit.track;
            if (project.selection.count (hit.clip) == 0) { project.selection = { hit.clip }; }
            primaryId = hit.clip;
            origins.clear();
            if (auto* c = project.findClip (hit.clip))
                origins.push_back ({ c->id, c->start, c->offset, c->length, c->fadeIn, c->fadeOut, hit.trackIndex });
            drag = Drag::Slip;
            project.sendChangeMessage();
            return true;
        }

        case Tool::Stretch:
        {
            if (! onClip) return false;
            auto* c = project.findClip (hit.clip);
            if (c == nullptr || c->asset == nullptr || c->asset->offline) return false;
            if (project.trackLocked (hit.track)) return true;
            project.selectedTrackId = hit.track;
            project.selection = { c->id };
            primaryId = c->id;
            stretchId = c->id; stretchHoldId = 0;
            stretchOrigStart = stretchStart = c->start;
            stretchOrigLen = stretchLen = c->length;
            const auto r = clipRect (hit.trackIndex, *c);
            stretchLeft = (float) e.x < r.getCentreX();
            buildSnapPoints (true);

            // l'onda com'e' adesso, su un'immagine che viene scalata mentre si trascina
            const int W = juce::jlimit (16, 16000, (int) std::ceil (r.getWidth())), H = juce::jmax (8, (int) (r.getHeight() - 24.0f));
            stretchImg = juce::Image (juce::Image::ARGB, W, H, true);
            {
                juce::Graphics ig (stretchImg);
                ig.addTransform (juce::AffineTransform::translation (-r.getX(), 0.0f).followedBy (juce::AffineTransform::scale ((float) W / juce::jmax (1.0f, r.getWidth()), 1.0f)));
                ig.setColour (project.getSession().tracks[(size_t) hit.trackIndex].colour.brighter (0.8f).withAlpha (0.92f));
                drawWaveform (ig, *c, juce::Rectangle<float> (r.getX(), 0.0f, r.getWidth(), (float) H),
                              juce::Rectangle<int> ((int) std::floor (r.getX()) - 1, 0, (int) std::ceil (r.getWidth()) + 3, H));
            }
            drag = Drag::Stretch;
            project.sendChangeMessage();
            return true;
        }

        case Tool::Mute:
            if (! onClip) return false;
            project.selection = { hit.clip };
            project.toggleClipMute (hit.clip);
            return true;

        case Tool::Erase:
            if (! onClip) return false;
            project.eraseClip (hit.clip);
            return true;

        case Tool::Range:
        {
            if (! inLanes) return false;
            buildSnapPoints (false);
            int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
            if (snapTo (pos, snapped)) pos = snapped;
            rangeAnchor = pos;
            rangeAnchorRow = juce::jlimit (0, juce::jmax (0, (int) project.getSession().tracks.size() - 1), trackIndexAtY (e.y));
            project.range = { pos, pos, rangeAnchorRow, rangeAnchorRow };
            drag = Drag::Range;
            repaint();
            return true;
        }

        case Tool::Comp:
        {
            if (! onClip) return false;
            if (project.trackLocked (hit.track)) return true;
            if (const auto* tr = project.findTrack (hit.track); tr != nullptr && ! tr->showLanes) { project.setTracksLanes ({ hit.track }, true); updateScrollBars(); repaint(); return true; }   // il primo clic mostra le corsie
            buildSnapPoints (false);
            int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
            if (snapTo (pos, snapped)) pos = snapped;
            compTrackId = hit.track; compClipId = hit.clip; compAnchor = compA = compB = pos;
            drag = Drag::Comp;
            repaint();
            return true;
        }

        case Tool::Hand:
            if (! inLanes && hit.type != HitType::Video) return false;
            drag = Drag::Pan;
            panScrollX = scrollX;
            panScrollY = scrollY;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            return true;

        case Tool::Zoom:
            if (! inLanes) return false;
            drag = Drag::ZoomRect;
            return true;

        case Tool::Automation:
        {
            if (! inLanes || hit.trackIndex < 0) return false;
            project.selectedTrackId = hit.track;
            int ti = -1, pid = 0;
            int sa = 0, sb = 0, sti = -1;
            if (findAutoPointAt (e.getPosition(), ti, pid))
            {
                autoTrackId = project.getSession().tracks[(size_t) ti].id;
                autoPointId = pid;
            }
            else if (! e.mods.isAltDown() && findAutoSegmentAt (e.getPosition(), sti, sa, sb))
            {
                // clic sulla linea tra due punti: trascinando in alto o in basso si sposta tutto il tratto (Alt + clic aggiunge un punto)
                const auto& tr = project.getSession().tracks[(size_t) sti];
                const auto& lane = tr.lane (tr.live->autoParam.load());
                segTrackId = tr.id; segIdA = sa; segIdB = sb; segDownY = e.y;
                for (auto& ap : lane) { if (ap.id == sa) { segPosA = ap.pos; segValA = ap.db; } if (ap.id == sb) { segPosB = ap.pos; segValB = ap.db; } }
                dragStarted = false;
                drag = Drag::AutoSegment;
                return true;
            }
            else
            {
                buildSnapPoints (false);
                int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
                if (snapTo (pos, snapped)) pos = snapped;
                autoTrackId = hit.track;
                autoPointId = project.addAutoPoint (hit.track, pos, autoValueForY (hit.trackIndex, project.findTrack (hit.track)->live->autoParam.load(), e.y));
                dragStarted = true;       // l'inserimento ha gia' creato il passo di undo
            }
            buildSnapPoints (false);
            drag = Drag::AutoPoint;
            return true;
        }

        default: return false;
    }
}

bool TimelineView::snapTo (int64_t s, int64_t& out) const
{
    if (! snapEnabled || snapPoints.empty()) return false;
    const int64_t thr = (int64_t) (10.0 * spp);
    auto it = std::lower_bound (snapPoints.begin(), snapPoints.end(), s);
    int64_t best = 0;
    bool found = false;
    auto consider = [&] (int64_t p)
    {
        if (std::abs (p - s) <= thr && (! found || std::abs (p - s) < std::abs (best - s))) { best = p; found = true; }
    };
    if (it != snapPoints.end()) consider (*it);
    if (it != snapPoints.begin()) consider (*(it - 1));
    if (found) out = best;
    return found;
}

void TimelineView::beginDragEdit()
{
    if (dragStarted) return;
    dragStarted = true;
    project.beginEdit();
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = hitTest (e.getPosition());
    const int newHover = hit.type == HitType::TrackResize ? hit.trackIndex : -1;
    if (newHover != hoverResize) { hoverResize = newHover; repaint(); }

    const bool onClip = hit.clip != 0 && (hit.type == HitType::Clip || hit.type == HitType::ClipTrimL || hit.type == HitType::ClipTrimR
                                          || hit.type == HitType::ClipFadeIn || hit.type == HitType::ClipFadeOut);
    const bool inLanes = onClip || hit.type == HitType::Lane;

    // guida della lama
    juce::Rectangle<int> guide;
    if (tool == Tool::Razor && inLanes && hit.trackIndex >= 0)
    {
        buildSnapPoints (false);
        int64_t snapped = 0;
        double x = e.x;
        if (snapTo (xToSample (e.x), snapped)) x = (double) sampleToX (snapped);
        int y0 = laneTop (hit.trackIndex), y1 = y0 + heightOf (hit.trackIndex);
        if (e.mods.isShiftDown()) { y0 = lanesTop(); y1 = laneArea().getBottom(); }
        guide = { (int) std::round (x), y0, 1, y1 - y0 };
    }
    if (guide != razorGuide)
    {
        repaint (razorGuide.expanded (3, 1));
        razorGuide = guide;
        repaint (razorGuide.expanded (3, 1));
    }

    if (tool == Tool::Select)
        if (const auto ch = cueHitTest (e.getPosition(), hit.type == HitType::Lane); ch.id != 0)
        {
            setMouseCursor (ch.kind == Drag::CueMove ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

    if (hit.type == HitType::TrackResize) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); return; }
    if (hit.type == HitType::HeaderTrim) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); return; }
    if (hit.type == HitType::HeaderGrip) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); return; }

    if ((tool == Tool::Razor || tool == Tool::Range || tool == Tool::Comp) && inLanes) { setMouseCursor (juce::MouseCursor::CrosshairCursor); return; }
    if ((tool == Tool::Mute || tool == Tool::Erase) && onClip) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    if ((tool == Tool::Slip || tool == Tool::Stretch) && onClip) { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
    if (tool == Tool::Hand && (inLanes || hit.type == HitType::Video)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    if (tool == Tool::Zoom && inLanes) { setMouseCursor (juce::MouseCursor::CrosshairCursor); return; }
    if (tool == Tool::Automation && inLanes)
    {
        int t0 = -1, a0 = 0, b0 = 0, pid0 = 0;
        if (! findAutoPointAt (e.getPosition(), t0, pid0) && findAutoSegmentAt (e.getPosition(), t0, a0, b0)) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); return; }
    }
    if (tool == Tool::Automation && inLanes) { setMouseCursor (juce::MouseCursor::CrosshairCursor); return; }

    switch (hit.type)
    {
        case HitType::ClipTrimL:
        case HitType::ClipTrimR:   setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case HitType::ClipFadeIn:
        case HitType::ClipFadeOut: setMouseCursor (juce::MouseCursor::UpDownLeftRightResizeCursor); break;
        case HitType::Clip:        setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
        case HitType::Video:       setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
        default:                   setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void TimelineView::mouseExit (const juce::MouseEvent&)
{
    if (hoverResize != -1) { hoverResize = -1; repaint(); }
    if (! razorGuide.isEmpty()) { repaint (razorGuide.expanded (3, 1)); razorGuide = {}; }
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = Drag::None;
    dragStarted = false;
    marquee = {};
    downPos = e.getPosition();
    const auto hit = hitTest (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        {
            int ti = -1, pid = 0;
            if (findAutoPointAt (e.getPosition(), ti, pid))
            {
                project.removeAutoPoint (project.getSession().tracks[(size_t) ti].id, pid);
                return;
            }
        }
        if (hit.type == HitType::GroupBar) { showGroupMenu (hit.group); return; }
        if (e.x < headerW && e.y < rulerH) { showControlsMenu(); return; }   // angolo in alto a sinistra: scelta dei controlli di traccia
        if (hit.type == HitType::ExportBar) { showExportBarMenu(); return; }
        if (hit.type == HitType::Marker) { showMarkerMenu (hit.markerId); return; }
        if (hit.type == HitType::Ruler) { showMarkerMenu (0); return; }
        if (hit.clip != 0)
        {
            if (project.selection.count (hit.clip) == 0)
            {
                project.selection = { hit.clip };
                project.sendChangeMessage();
            }
            showClipMenu (hit.clip);
        }
        else if (hit.track >= 0) showTrackMenu (hit.track);
        else showTrackMenu (-1);
        return;
    }

    if (hit.type == HitType::GroupBar)
    {
        if (hit.groupPart == 0) { if (const auto* g = project.findGroup (hit.group)) project.setGroupCollapsed (hit.group, ! g->collapsed); }
        else project.setGroupMuteSolo (hit.group, hit.groupPart == 2, ! groupAll (hit.group, hit.groupPart == 2));
        return;
    }

    // cue ADR: bordo = sposta ingresso/uscita, fascia = sposta tutto il cue
    if (tool == Tool::Select && ! e.mods.isCommandDown())
        if (const auto ch = cueHitTest (e.getPosition(), hit.type == HitType::Lane); ch.id != 0)
            if (auto* cue = project.findCue (ch.id))
            {
                drag = ch.kind;
                cueDragId = ch.id;
                cueOrigIn = cue->inPos;
                cueOrigOut = cue->outPos;
                if (project.selectedCueId != ch.id) { project.selectedCueId = ch.id; project.sendChangeMessage(); }
                if (! engine.isRecording()) engine.setPosition (cue->inPos);   // selezionare il cue porta la testina al suo ingresso
                return;
            }

    // Cmd (Ctrl su Windows) + trascina sulla timeline: seleziona la porzione che diventa la regione di loop
    if (e.mods.isCommandDown() && (hit.type == HitType::Lane || hit.clip != 0) && tool == Tool::Select)
    {
        drag = Drag::RulerLoop;
        loopAnchor = juce::jmax<int64_t> (0, xToSample (e.x));
        return;
    }

    if (tool != Tool::Select && handleToolMouseDown (e, hit)) return;

    switch (hit.type)
    {
        case HitType::Marker:
            if (auto* m = project.findMarker (hit.markerId))
            {
                drag = Drag::MarkerMove;
                markerDragId = hit.markerId;
                engine.setPosition (m->pos);
            }
            break;

        case HitType::HeaderGrip:
            if (e.mods.isCommandDown()) { project.toggleTrackSelection (hit.track); project.sendChangeMessage(); break; }   // piu' tracce: niente riordino
            if (e.mods.isShiftDown()) { project.selectTrackRange (hit.track); project.sendChangeMessage(); break; }
            project.selectOnlyTrack (hit.track);
            drag = Drag::TrackReorder;
            reorderFrom = reorderTo = hit.trackIndex;
            project.sendChangeMessage();
            break;

        case HitType::TrackResize:
            drag = Drag::TrackResize;
            resizeIndex = hit.trackIndex;
            resizeOrigH = heightOf (hit.trackIndex);
            break;

        case HitType::ExportBar:
        {
            buildSnapPoints (false);
            const int64_t s = juce::jmax<int64_t> (0, xToSample (e.x));
            const bool has = project.exportOut > project.exportIn && project.exportIn >= 0;
            exportMode = 0;
            if (has)
            {
                const float x0 = sampleToX (project.exportIn), x1 = sampleToX (project.exportOut);
                if (std::abs ((float) e.x - x0) <= 7.0f) exportMode = 1;
                else if (std::abs ((float) e.x - x1) <= 7.0f) exportMode = 2;
                else if ((float) e.x > x0 && (float) e.x < x1) { exportMode = 3; exportGrab = s - project.exportIn; }
            }
            int64_t snapped = 0;
            exportAnchor = snapTo (s, snapped) ? snapped : s;
            drag = Drag::ExportRange;
            break;
        }

        case HitType::Ruler:
            if (e.mods.isShiftDown())
            {
                drag = Drag::RulerLoop;
                loopAnchor = juce::jmax<int64_t> (0, xToSample (e.x));
            }
            else
            {
                drag = Drag::RulerSeek;
                engine.setPosition (xToSample (e.x));
            }
            break;

        case HitType::Video:
            // clic = sposta la testina, trascinamento = cambia l'offset del video
            drag = Drag::VideoMove;
            videoOrigOffset = project.getSession().video != nullptr ? project.getSession().video->offsetSec : 0.0;
            break;

        case HitType::HeaderAuto:
            if (auto* t = project.findTrack (hit.track)) { t->live->autoMode = (t->live->autoMode.load() + 1) % 3; project.liveChanged(); }
            break;
        case HitType::HeaderTrim:
            if (auto* t = project.findTrack (hit.track))
            {
                project.selectedTrackId = hit.track;
                trimTrackId = hit.track;
                trimStartDb = t->live->trimDb.load();
                trimDownY = e.y;
                drag = Drag::Trim;
                project.sendChangeMessage();
            }
            break;
        case HitType::HeaderAutoParam:
            if (auto* t = project.findTrack (hit.track)) { t->live->autoParam = (t->live->autoParam.load() + 1) % kNumAutoParams; project.liveChanged(); }
            break;
        case HitType::HeaderArm:
            if (auto* t = project.findTrack (hit.track))
            {
                t->live->armed = ! t->live->armed.load();
                if (t->live->armed.load()) engine.ensureInput();   // apre l'ingresso subito, cosi' il vu-meter mostra il livello prima di registrare
                project.liveChanged();
            }
            break;
        case HitType::HeaderInput:
            if (auto* t = project.findTrack (hit.track)) { t->live->inputMode = (t->live->inputMode.load() + 1) % 3; project.liveChanged(); }
            break;
        case HitType::HeaderLock:
            if (auto* t = project.findTrack (hit.track))
            {
                const auto targets = project.isTrackSelected (hit.track) ? project.selectedTracks() : std::vector<int> { hit.track };
                project.setTracksLocked (targets, ! t->locked);
            }
            break;
        case HitType::GlobalMute: project.clearAllMute(); break;
        case HitType::GlobalSolo: project.clearAllSolo(); break;
        case HitType::GlobalVisibility: showVisibilityMenu(); break;
        case HitType::HeaderMute:
            if (auto* t = project.findTrack (hit.track)) { t->live->mute = ! t->live->mute.load(); project.liveChanged(); }
            break;
        case HitType::HeaderSolo:
            if (auto* t = project.findTrack (hit.track)) { t->live->solo = ! t->live->solo.load(); project.liveChanged(); }
            break;
        case HitType::Header:
            project.selectedTrackId = hit.track;
            project.sendChangeMessage();
            break;

        case HitType::Clip:
        case HitType::ClipTrimL:
        case HitType::ClipTrimR:
        case HitType::ClipFadeIn:
        case HitType::ClipFadeOut:
        {
            project.selectedTrackId = hit.track;
            if (project.selection.count (hit.clip) == 0)
            {
                if (! e.mods.isShiftDown()) project.selection.clear();
                project.selection.insert (hit.clip);
            }
            else if (e.mods.isShiftDown() && hit.type == HitType::Clip)
            {
                project.selection.erase (hit.clip);
                project.sendChangeMessage();
                return;
            }
            primaryId = hit.clip;
            if (project.trackLocked (hit.track)) { project.sendChangeMessage(); break; }   // traccia bloccata: il clip si seleziona ma non si modifica

            origins.clear();
            for (size_t ti = 0; ti < project.getSession().tracks.size(); ++ti)
                if (! project.getSession().tracks[ti].editLocked())
                for (auto& c : project.getSession().tracks[ti].clips)
                    if (project.selection.count (c.id) > 0 || c.id == primaryId)
                        origins.push_back ({ c.id, c.start, c.offset, c.length, c.fadeIn, c.fadeOut, (int) ti });

            buildSnapPoints (true);

            drag = hit.type == HitType::ClipTrimL ? Drag::TrimL
                 : hit.type == HitType::ClipTrimR ? Drag::TrimR
                 : hit.type == HitType::ClipFadeIn ? Drag::FadeIn
                 : hit.type == HitType::ClipFadeOut ? Drag::FadeOut : Drag::Move;
            project.sendChangeMessage();
            break;
        }

        case HitType::Lane:
            drag = Drag::Marquee;
            baseSelection = e.mods.isShiftDown() ? project.selection : std::set<int64_t>();
            if (hit.track >= 0) project.selectedTrackId = hit.track;
            if (! e.mods.isShiftDown()) project.selection.clear();
            project.sendChangeMessage();
            break;

        default: break;
    }
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::None) return;
    const auto lanes = laneArea();

    if (drag == Drag::TrackResize)
    {
        const int nh = juce::jlimit (minTrackH, maxTrackH, resizeOrigH + e.getDistanceFromDragStartY());
        if (e.mods.isShiftDown()) { for (auto& t : project.getSession().tracks) t.live->height = nh; }
        else if (auto* t = (resizeIndex >= 0 && resizeIndex < (int) project.getSession().tracks.size()) ? &project.getSession().tracks[(size_t) resizeIndex] : nullptr)
            t->live->height = nh;
        updateScrollBars();
        repaint();
        return;
    }

    if (drag == Drag::AutoPoint)
    {
        const int ti = project.trackIndexForId (autoTrackId);
        if (ti < 0) return;
        if (! dragStarted && e.getDistanceFromDragStart() < 3) return;
        beginDragEdit();
        int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
        if (snapTo (pos, snapped)) pos = snapped;
        project.moveAutoPoint (autoTrackId, autoPointId, pos, autoValueForY (ti, project.getSession().tracks[(size_t) ti].live->autoParam.load(), e.y));
        return;
    }
    if (drag == Drag::MarkerMove)
    {
        if (e.getDistanceFromDragStart() < 3 && ! dragStarted) return;
        beginDragEdit();
        if (auto* m = project.findMarker (markerDragId))
        {
            buildSnapPoints (false);
            for (auto& o : project.getSession().markers) if (o.id != markerDragId) snapPoints.push_back (o.pos);
            std::sort (snapPoints.begin(), snapPoints.end());
            int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
            if (snapTo (pos, snapped)) pos = snapped;
            m->pos = pos;
            project.changed();
        }
        return;
    }
    if (drag == Drag::Range || drag == Drag::Comp)
    {
        int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
        if (snapTo (pos, snapped)) pos = snapped;
        if (drag == Drag::Range)
        {
            const int row = juce::jlimit (0, juce::jmax (0, (int) project.getSession().tracks.size() - 1), trackIndexAtY (e.y));
            project.range = { juce::jmin (rangeAnchor, pos), juce::jmax (rangeAnchor, pos), juce::jmin (rangeAnchorRow, row), juce::jmax (rangeAnchorRow, row) };
        }
        else { compA = juce::jmin (compAnchor, pos); compB = juce::jmax (compAnchor, pos); }
        repaint();
        return;
    }
    if (drag == Drag::CueIn || drag == Drag::CueOut || drag == Drag::CueMove)
    {
        if (e.getDistanceFromDragStart() < 3 && ! dragStarted) return;
        beginDragEdit();
        if (auto* cue = project.findCue (cueDragId))
        {
            buildSnapPoints (false);
            snapPoints.erase (std::remove_if (snapPoints.begin(), snapPoints.end(), [this] (int64_t v) { return v == cueOrigIn || v == cueOrigOut; }), snapPoints.end());
            const int64_t minLen = (int64_t) (project.getSampleRate() * 0.1);
            int64_t snapped = 0;
            if (drag == Drag::CueMove)
            {
                int64_t delta = juce::jmax ((int64_t) std::llround ((double) e.getDistanceFromDragStartX() * spp), -cueOrigIn);
                int64_t sa = 0, sb = 0;
                const bool snapA = snapTo (cueOrigIn + delta, sa), snapB = snapTo (cueOrigOut + delta, sb);
                if (snapA && (! snapB || std::abs (sa - (cueOrigIn + delta)) <= std::abs (sb - (cueOrigOut + delta)))) delta = sa - cueOrigIn;
                else if (snapB) delta = sb - cueOrigOut;
                delta = juce::jmax (delta, -cueOrigIn);
                cue->inPos = cueOrigIn + delta;
                cue->outPos = cueOrigOut + delta;
            }
            else
            {
                int64_t pos = juce::jmax<int64_t> (0, xToSample (e.x));
                if (snapTo (pos, snapped)) pos = snapped;
                if (drag == Drag::CueIn) cue->inPos = juce::jlimit<int64_t> (0, cue->outPos - minLen, pos);
                else cue->outPos = juce::jmax (cue->inPos + minLen, pos);
            }
            project.changed();
        }
        return;
    }
    if (drag == Drag::Pan)
    {
        scrollX = panScrollX - (double) e.getDistanceFromDragStartX() * spp;
        scrollY = panScrollY - e.getDistanceFromDragStartY();
        updateScrollBars();
        repaint();
        return;
    }
    if (drag == Drag::ZoomRect)
    {
        if (e.getDistanceFromDragStart() < 3) return;
        dragStarted = true;
        marquee = juce::Rectangle<int> (downPos, e.getPosition()).getIntersection (lanes);
        repaint();
        return;
    }
    if (drag == Drag::TrackReorder)
    {
        const int n = (int) project.getSession().tracks.size();
        const int t = juce::jlimit (0, n - 1, trackIndexAtY (e.y));
        if (t != reorderTo) { reorderTo = t; repaint(); }
        return;
    }
    if (drag == Drag::Slip)
    {
        if (e.getDistanceFromDragStart() < 3 && ! dragStarted) return;
        beginDragEdit();
        if (auto* c = project.findClip (primaryId); c != nullptr && ! origins.empty())
        {
            const int64_t dx = (int64_t) std::llround ((double) e.getDistanceFromDragStartX() * spp);
            const auto& o = origins.front();
            c->offset = juce::jlimit<int64_t> (0, c->asset->length() - c->length, o.offset - dx);
            project.changed();
        }
        return;
    }

    if (drag == Drag::VideoMove)
    {
        if (e.getDistanceFromDragStart() < 4 && ! dragStarted) return;
        dragStarted = true;
        const auto v = project.getSession().video;
        if (v == nullptr) return;
        const double rate = project.getSampleRate();
        double off = videoOrigOffset + (double) e.getDistanceFromDragStartX() * spp / rate;
        if (snapEnabled)
        {
            if (std::abs (off) * rate < 8.0 * spp) off = 0.0;                              // aggancio allo zero
            else off = std::round (off * v->fps) / v->fps;                                 // altrimenti ai fotogrammi
        }
        project.setVideoOffset (off);
        if (onVideoOffsetChanged) onVideoOffsetChanged();
        repaint();
        return;
    }

    // scorrimento automatico ai bordi durante il trascinamento
    if (drag == Drag::Move || drag == Drag::Marquee || drag == Drag::RulerSeek || drag == Drag::RulerLoop || drag == Drag::ExportRange)
    {
        if (e.x > lanes.getRight() - 12) { scrollX += 20.0 * spp; updateScrollBars(); }
        else if (e.x < headerW + 12) { scrollX -= 20.0 * spp; updateScrollBars(); }
    }

    if (drag == Drag::RulerSeek)
    {
        engine.setPosition (juce::jmax<int64_t> (0, xToSample (e.x)));
        repaint();
        return;
    }
    if (drag == Drag::Trim)
    {
        if (auto* t = project.findTrack (trimTrackId))
        {
            const float step = e.mods.isShiftDown() ? 0.04f : 0.25f;   // dB per pixel
            t->live->trimDb = juce::jlimit (-24.0f, 24.0f, std::round ((trimStartDb + (float) (trimDownY - e.y) * step) * 10.0f) / 10.0f);
            project.liveChanged();
            repaint (0, laneTop (project.trackIndexForId (trimTrackId)), headerW, 80);
        }
        return;
    }
    if (drag == Drag::AutoSegment)
    {
        const int ti = project.trackIndexForId (segTrackId);
        if (ti < 0) return;
        const int dy = e.y - segDownY;
        if (! dragStarted && std::abs (dy) < 2) return;
        beginDragEdit();
        const int param = project.getSession().tracks[(size_t) ti].live->autoParam.load();
        // la linea si sposta in verticale: i due punti mantengono la loro posizione e si muovono dello stesso tratto sullo schermo
        project.moveAutoPoint (segTrackId, segIdA, segPosA, autoValueForY (ti, param, (int) (autoYForValue (ti, param, segValA) + (float) dy)));
        project.moveAutoPoint (segTrackId, segIdB, segPosB, autoValueForY (ti, param, (int) (autoYForValue (ti, param, segValB) + (float) dy)));
        return;
    }
    if (drag == Drag::ExportRange)
    {
        int64_t cur = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
        if (exportMode != 3 && snapTo (cur, snapped)) cur = snapped;
        if (exportMode == 3)
        {
            const int64_t len = project.exportOut - project.exportIn;
            int64_t in = juce::jmax<int64_t> (0, cur - exportGrab);
            if (snapTo (in, snapped)) in = snapped;
            project.exportIn = in; project.exportOut = in + len;
        }
        else if (exportMode == 1) project.exportIn = juce::jmin (cur, project.exportOut - 1);
        else if (exportMode == 2) project.exportOut = juce::jmax (cur, project.exportIn + 1);
        else { project.exportIn = juce::jmin (exportAnchor, cur); project.exportOut = juce::jmax (exportAnchor, cur); }
        repaint();
        return;
    }
    if (drag == Drag::Stretch)
    {
        const int64_t origEnd = stretchOrigStart + stretchOrigLen;
        const int64_t minLen = juce::jmax<int64_t> (64, stretchOrigLen / 4), maxLen = stretchOrigLen * 4;
        int64_t cur = juce::jmax<int64_t> (0, xToSample (e.x)), snapped = 0;
        if (snapTo (cur, snapped)) cur = snapped;
        if (stretchLeft)
        {
            stretchStart = juce::jlimit (origEnd - maxLen, origEnd - minLen, cur);
            stretchLen = origEnd - stretchStart;
        }
        else
        {
            stretchStart = stretchOrigStart;
            stretchLen = juce::jlimit (minLen, maxLen, cur - stretchOrigStart);
        }
        repaint();
        return;
    }
    if (drag == Drag::RulerLoop)
    {
        const int64_t cur = juce::jmax<int64_t> (0, xToSample (e.x));
        engine.setLoop (true, juce::jmin (loopAnchor, cur), juce::jmax (loopAnchor, cur));
        if (onLoopChanged) onLoopChanged();
        repaint();
        return;
    }

    if (drag == Drag::Marquee)
    {
        if (e.getDistanceFromDragStart() < 3) return;
        dragStarted = true;
        marquee = juce::Rectangle<int> (downPos, e.getPosition()).getIntersection (lanes);
        auto sel = baseSelection;
        const auto& tracks = project.getSession().tracks;
        for (size_t ti = 0; ti < tracks.size(); ++ti)
            for (auto& c : tracks[ti].clips)
                if (clipRect ((int) ti, c).toNearestInt().intersects (marquee)) sel.insert (c.id);
        project.selection = sel;
        repaint();
        return;
    }

    if (e.getDistanceFromDragStart() < 3 && ! dragStarted) return;

    const auto* primary = project.findClip (primaryId);
    if (primary == nullptr) return;
    Origin po {};
    for (auto& o : origins) if (o.id == primaryId) po = o;
    const int64_t dx = (int64_t) std::llround ((double) e.getDistanceFromDragStartX() * spp);

    beginDragEdit();

    switch (drag)
    {
        case Drag::Move:
        {
            int64_t minStart = std::numeric_limits<int64_t>::max();
            for (auto& o : origins) minStart = juce::jmin (minStart, o.start);
            int64_t delta = juce::jmax (dx, -minStart);

            int64_t sa = 0, sb = 0;
            const bool snapA = snapTo (po.start + delta, sa);
            const bool snapB = snapTo (po.start + po.length + delta, sb);
            if (snapA && (! snapB || std::abs (sa - (po.start + delta)) <= std::abs (sb - (po.start + po.length + delta))))
                delta = sa - po.start;
            else if (snapB)
                delta = sb - (po.start + po.length);
            delta = juce::jmax (delta, -minStart);

            const int nTracks = (int) project.getSession().tracks.size();
            int rowDelta = juce::jlimit (-1000, 1000, trackIndexAtY (e.y) - trackIndexAtY (downPos.y));
            for (auto& o : origins)
                rowDelta = juce::jlimit (-o.track, nTracks - 1 - o.track, rowDelta);

            for (auto& o : origins)
                project.moveClipTo (o.id, o.track + rowDelta, o.start + delta);
            break;
        }
        case Drag::TrimL:
        {
            if (auto* c = project.findClip (primaryId))
            {
                int64_t ns = po.start + dx, snapped = 0;
                if (snapTo (ns, snapped)) ns = snapped;
                ns = juce::jlimit<int64_t> (juce::jmax<int64_t> (0, po.start - po.offset), po.start + po.length - 64, ns);
                const int64_t d = ns - po.start;
                c->start = ns;
                c->offset = po.offset + d;
                c->length = po.length - d;
                c->fadeIn = juce::jmin (po.fadeIn, c->length);
                c->fadeOut = juce::jmin (po.fadeOut, c->length);
            }
            break;
        }
        case Drag::TrimR:
        {
            if (auto* c = project.findClip (primaryId))
            {
                int64_t ne = po.start + po.length + dx, snapped = 0;
                if (snapTo (ne, snapped)) ne = snapped;
                ne = juce::jlimit<int64_t> (po.start + 64, po.start + (c->asset->length() - po.offset), ne);
                c->length = ne - po.start;
                c->fadeOut = juce::jmin (po.fadeOut, c->length);
                c->fadeIn = juce::jmin (po.fadeIn, c->length);
            }
            break;
        }
        case Drag::FadeIn:
        {
            if (auto* c = project.findClip (primaryId))
                c->fadeIn = juce::jlimit<int64_t> (0, c->length - c->fadeOut, xToSample (e.x) - c->start);
            break;
        }
        case Drag::FadeOut:
        {
            if (auto* c = project.findClip (primaryId))
                c->fadeOut = juce::jlimit<int64_t> (0, c->length - c->fadeIn, c->end() - xToSample (e.x));
            break;
        }
        default: break;
    }
    project.changed();
}

void TimelineView::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::Marquee && ! dragStarted && ! e.mods.isPopupMenu())
    {
        const auto pos = juce::jmax<int64_t> (0, xToSample (e.x));
        engine.setPosition (pos);
        // un clic dentro un cue lo seleziona (la finestra ADR lo mostra)
        for (auto& cue : project.getSession().cues)
            if (pos >= cue.inPos && pos < cue.outPos) { project.selectedCueId = cue.id; break; }
    }
    else if (drag == Drag::Range)
    {
        if (project.range.b - project.range.a < (int64_t) (spp * 4.0)) project.range = {};   // un semplice clic toglie la selezione
        repaint();
    }
    else if (drag == Drag::Comp)
    {
        if (compB - compA >= (int64_t) (spp * 4.0)) project.compRange (compTrackId, compClipId, compA, compB);
        compTrackId = 0;
    }
    else if (drag == Drag::Move && dragStarted && project.editMode == Project::EditMode::Shuffle)
        project.shuffleResolve();
    else if (drag == Drag::VideoMove && ! dragStarted)
        engine.setPosition (juce::jmax<int64_t> (0, xToSample (e.x)));
    else if (drag == Drag::TrackResize)
        project.liveChanged();
    else if (drag == Drag::ExportRange)
    {
        if (project.exportOut - project.exportIn < (int64_t) (spp * 4.0)) project.exportIn = project.exportOut = -1;   // un semplice clic non crea un intervallo
        project.markDirty();
        repaint();
    }
    else if (drag == Drag::Stretch)
    {
        const double ratio = (double) stretchLen / (double) juce::jmax<int64_t> (1, stretchOrigLen);
        if (std::abs (ratio - 1.0) > 0.002 && onStretchCommit)
        {
            stretchHoldId = stretchId;   // l'anteprima resta finche' l'audio non e' stato ricalcolato
            onStretchCommit (stretchId, ratio, stretchStart);
            if (project.getBusyCount() == 0) { stretchHoldId = stretchId = 0; stretchImg = {}; }
        }
        else { stretchId = 0; stretchImg = {}; }
        repaint();
    }
    else if (drag == Drag::AutoPoint)
        autoPointId = 0;
    else if (drag == Drag::MarkerMove)
    {
        if (dragStarted)
        {
            auto& mk = project.edit().markers;
            std::sort (mk.begin(), mk.end(), [] (const Marker& a, const Marker& b) { return a.pos < b.pos; });
            project.changed();
        }
        markerDragId = 0;
    }
    else if (drag == Drag::CueIn || drag == Drag::CueOut || drag == Drag::CueMove)
    {
        if (dragStarted)
        {
            auto& cues = project.edit().cues;
            std::sort (cues.begin(), cues.end(), [] (const AdrCue& a, const AdrCue& b) { return a.inPos < b.inPos; });
            project.changed();
        }
        cueDragId = 0;
    }
    else if (drag == Drag::TrackReorder)
    {
        if (reorderFrom >= 0 && reorderTo >= 0 && reorderFrom != reorderTo) project.moveTrackTo (reorderFrom, reorderTo);
        reorderFrom = reorderTo = -1;
    }
    else if (drag == Drag::ZoomRect)
    {
        const auto lanes = laneArea();
        if (dragStarted && marquee.getWidth() >= 8)
        {
            const double s0 = xToSampleD (marquee.getX()), s1 = xToSampleD (marquee.getRight());
            spp = juce::jlimit (0.05, 400000.0, (s1 - s0) / juce::jmax (50, lanes.getWidth()));
            scrollX = s0;
            updateScrollBars();
            if (onZoomChanged) onZoomChanged();
        }
        else
            zoomBy (e.mods.isAltDown() ? 0.5 : 2.0, (float) e.x);
    }
    else if (drag == Drag::Pan)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);

    drag = Drag::None;
    resizeIndex = -1;
    marquee = {};
    origins.clear();
    snapPoints.clear();
    project.sendChangeMessage();
    repaint();
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = hitTest (e.getPosition());
    if (hit.type == HitType::Marker) { renameMarkerDialog (hit.markerId); return; }
    if (hit.type == HitType::GroupBar) { renameGroupDialog (hit.group); return; }
    if (hit.type == HitType::TrackResize && hit.trackIndex >= 0)
    {
        // doppio clic sul bordo: altezza predefinita
        if (auto* t = project.findTrack (hit.track)) { t->live->height = 96; project.liveChanged(); updateScrollBars(); repaint(); }
    }
    else if (hit.type == HitType::HeaderTrim)
    {
        if (auto* t = project.findTrack (hit.track)) { t->live->trimDb = 0.0f; project.liveChanged(); repaint(); }
    }
    else if ((hit.type == HitType::Header || hit.type == HitType::HeaderGrip) && hit.track >= 0 && e.x > 30)
        renameTrackDialog (hit.track);
    else if (hit.type == HitType::ExportBar) { project.exportIn = project.exportOut = -1; project.markDirty(); repaint(); }
    else if (hit.type == HitType::Ruler)
    {
        engine.setLoop (false, 0, 0);
        if (onLoopChanged) onLoopChanged();
        repaint();
    }
    else if (hit.type == HitType::Video && project.getSession().video != nullptr)
    {
        project.setVideoOffset (0.0);
        if (onVideoOffsetChanged) onVideoOffsetChanged();
        repaint();
    }
    else if (hit.type == HitType::Lane && hit.clip == 0 && hit.track < 0 && onImportRequested)
        onImportRequested();
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    {
        const auto hit = hitTest (e.getPosition());
        if (hit.type == HitType::HeaderTrim)
            if (auto* t = project.findTrack (hit.track))
            {
                t->live->trimDb = juce::jlimit (-24.0f, 24.0f, std::round ((t->live->trimDb.load() + w.deltaY * 8.0f) * 10.0f) / 10.0f);
                project.liveChanged();
                repaint();
                return;
            }
    }
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomBy (std::exp ((double) w.deltaY * 1.6), (float) e.x);
        return;
    }
    if (e.mods.isAltDown())
    {
        setAllTrackHeights (getTrackHeight() + (int) (w.deltaY * 120.0f));
        return;
    }
    const bool horiz = e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY);
    const float dx = e.mods.isShiftDown() && w.deltaX == 0.0f ? w.deltaY : w.deltaX;
    if (horiz) scrollX -= (double) dx * 700.0 * spp;
    else scrollY -= (int) (w.deltaY * 600.0f);
    updateScrollBars();
    repaint();
}

void TimelineView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    zoomBy ((double) scaleFactor, (float) e.x);
}

//==============================================================================
void TimelineView::renameTrackDialog (int trackId)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr) return;
    auto* aw = new juce::AlertWindow (TRANS ("Rinomina traccia"), "", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", t->name);
    aw->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TimelineView> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, trackId] (int r)
    {
        if (r == 1 && safe != nullptr) safe->project.renameTrack (trackId, aw->getTextEditorContents ("name"));
    }), true);
}

bool TimelineView::groupAll (int groupId, bool solo) const
{
    bool any = false;
    for (auto& t : project.getSession().tracks)
        if (t.groupId == groupId)
        {
            any = true;
            if (! (solo ? t.live->solo : t.live->mute).load()) return false;
        }
    return any;
}

void TimelineView::drawGroupBars (juce::Graphics& g)
{
    const auto lanes = laneArea();
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (0, lanes.getY(), lanes.getRight(), lanes.getHeight());
    const auto& tracks = project.getSession().tracks;
    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        if (bandOf (i) == 0) continue;
        const int y = laneTop (i) - groupBandH;
        if (y + groupBandH < lanes.getY() || y > lanes.getBottom()) continue;
        const auto* grp = project.findGroup (tracks[(size_t) i].groupId);
        if (grp == nullptr) continue;
        int members = 0;
        for (auto& t : tracks) if (t.groupId == grp->id) ++members;

        g.setGradientFill (juce::ColourGradient (panelHi, 0.0f, (float) y, panelTop, 0.0f, (float) (y + groupBandH), false));
        g.fillRect (0, y, lanes.getRight(), groupBandH);
        g.setColour (blue.withAlpha (0.85f));
        g.fillRect (0, y, 4, groupBandH);
        g.setColour (border);
        g.drawHorizontalLine (y + groupBandH - 1, 0.0f, (float) lanes.getRight());

        juce::Path arrow;   // ▶ compresso, ▼ aperto
        const float ax = 14.0f, ay = (float) y + groupBandH * 0.5f;
        if (grp->collapsed) arrow.addTriangle (ax - 3.0f, ay - 5.0f, ax - 3.0f, ay + 5.0f, ax + 4.0f, ay);
        else arrow.addTriangle (ax - 5.0f, ay - 3.0f, ax + 5.0f, ay - 3.0f, ax, ay + 4.0f);
        g.setColour (text);
        g.fillPath (arrow);
        g.setFont (font (11.5f, true));
        g.drawText (grp->name.toUpperCase() + "   (" + juce::String (members) + ")", juce::Rectangle<int> (26, y, headerW - 100, groupBandH), juce::Justification::centredLeft, true);

        for (int part = 1; part <= 2; ++part)
        {
            const bool on = groupAll (grp->id, part == 2);
            const juce::Rectangle<int> r (headerW - (part == 1 ? 66 : 36), y + 3, 26, groupBandH - 6);
            g.setColour (on ? (part == 1 ? accent : juce::Colour (0xffffc94d)) : panel);
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
            g.setColour (on ? juce::Colours::black : dim);
            g.setFont (font (10.5f, true));
            g.drawText (part == 1 ? "M" : "S", r, juce::Justification::centred);
        }
    }
}

void TimelineView::showControlsMenu()
{
    struct Item { Control c; const char* label; };
    static const Item items[] = {
        { CtlLock, TRX ("Lock / Freeze (L)") }, { CtlRec, TRX ("Registra (R)") }, { CtlMute, TRX ("Mute (M)") }, { CtlSolo, TRX ("Solo (S)") },
        { CtlTrim, TRX ("Manopola del guadagno in ingresso") }, { CtlMeter, TRX ("Misuratore di livello") },
        { CtlAutomation, TRX ("Automazione (parametro e modo)") }, { CtlInput, TRX ("Ingresso (IN 1 / IN 2)") } };
    juce::PopupMenu m;
    m.addSectionHeader (TRANS ("Controlli delle tracce"));
    for (auto& it : items) m.addItem (100 + (int) it.c, juce::translate (it.label), true, shows (it.c));
    m.addSeparator();
    m.addItem (1, TRANS ("Mostra tutti i controlli"));
    m.addItem (2, TRANS ("Solo il nome (vista compatta)"));
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1) safe->setControlMask (CtlAll);
        else if (r == 2) safe->setControlMask (0);
        else if (r >= 100) safe->setControlMask (safe->controlMask ^ (r - 100));
    });
}

void TimelineView::showVisibilityMenu()
{
    juce::PopupMenu m;
    m.addItem (1, TRANS ("Mostra tutte le tracce"));
    m.addItem (2, TRANS ("Nascondi le tracce vuote"));
    m.addSeparator();
    const auto& tracks = project.getSession().tracks;
    for (size_t i = 0; i < tracks.size(); ++i)
        m.addItem (100 + (int) i, juce::String ((int) i + 1) + "  " + tracks[i].name + (tracks[i].locked ? "  (Lock)" : ""), true, ! tracks[i].hidden);
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& p = safe->project;
        if (r == 1) p.showAllTracks();
        else if (r == 2) p.hideEmptyTracks();
        else if (r >= 100 && r - 100 < (int) p.getSession().tracks.size())
        {
            const auto& t = p.getSession().tracks[(size_t) (r - 100)];
            p.setTracksHidden ({ t.id }, ! t.hidden);
        }
        safe->updateScrollBars();
        safe->repaint();
    });
}

void TimelineView::showGroupMenu (int groupId)
{
    const auto* grp = project.findGroup (groupId);
    if (grp == nullptr) return;
    juce::PopupMenu m;
    m.addItem (1, grp->collapsed ? TRANS ("Espandi il gruppo") : TRANS ("Comprimi il gruppo"));
    m.addItem (2, TRANS ("Rinomina il gruppo..."));
    m.addItem (3, TRANS ("Sciogli il gruppo (le tracce restano)"));
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, groupId] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& p = safe->project;
        if (r == 1) { if (const auto* g = p.findGroup (groupId)) p.setGroupCollapsed (groupId, ! g->collapsed); }
        else if (r == 2) safe->renameGroupDialog (groupId);
        else if (r == 3) p.removeGroup (groupId);
    });
}

void TimelineView::renameGroupDialog (int groupId)
{
    const auto* grp = project.findGroup (groupId);
    if (grp == nullptr) return;
    auto* aw = new juce::AlertWindow (TRANS ("Rinomina il gruppo"), "", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", grp->name);
    aw->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TimelineView> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, groupId] (int r)
    {
        if (r == 1 && safe != nullptr) safe->project.renameGroup (groupId, aw->getTextEditorContents ("name"));
    }), true);
}

void TimelineView::newGroupDialog (std::vector<int> trackIds)
{
    auto* aw = new juce::AlertWindow (TRANS ("Nuovo gruppo"), TRANS ("Nome del gruppo (per esempio Dialoghi)"), juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", TRANS ("Dialoghi"));
    aw->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TimelineView> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, trackIds] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        auto& p = safe->project;
        p.setTracksGroup (trackIds, p.addGroup (aw->getTextEditorContents ("name")));
    }), true);
}

void TimelineView::renameMarkerDialog (int markerId)
{
    auto* m = project.findMarker (markerId);
    if (m == nullptr) return;
    auto* aw = new juce::AlertWindow (TRANS ("Rinomina marker"), "", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", m->name);
    aw->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TimelineView> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, markerId] (int r)
    {
        if (r == 1 && safe != nullptr) safe->project.renameMarker (markerId, aw->getTextEditorContents ("name"));
    }), true);
}

void TimelineView::showExportBarMenu()
{
    const bool has = project.exportOut > project.exportIn && project.exportIn >= 0;
    const bool hasLoop = engine.getLoopEnd() > engine.getLoopStart();
    const auto* cue = project.findCue (project.selectedCueId);
    juce::PopupMenu m;
    m.addItem (1, TRANS ("Esporta l'intervallo..."), has);
    m.addSeparator();
    m.addItem (2, TRANS ("Imposta dalla regione di loop"), hasLoop);
    m.addItem (3, TRANS ("Imposta dal cue ADR selezionato"), cue != nullptr);
    m.addItem (4, TRANS ("Cancella l'intervallo"), has);
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, cue] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& p = safe->project;
        if (r == 1) { if (safe->onExportRequested) safe->onExportRequested(); return; }
        if (r == 2) { p.exportIn = safe->engine.getLoopStart(); p.exportOut = safe->engine.getLoopEnd(); }
        else if (r == 3 && cue != nullptr) { p.exportIn = cue->inPos; p.exportOut = cue->outPos; }
        else if (r == 4) p.exportIn = p.exportOut = -1;
        p.markDirty();
        safe->repaint();
    });
}

void TimelineView::showMarkerMenu (int markerId)
{
    juce::PopupMenu m;
    const int64_t pos = engine.getPosition();
    m.addItem (1, TRANS ("Aggiungi marker alla testina"));
    if (markerId != 0)
    {
        m.addSeparator();
        m.addItem (2, TRANS ("Rinomina..."));
        juce::PopupMenu colours;
        for (int i = 0; i < numPaletteColours; ++i) colours.addColouredItem (100 + i, "Colore " + juce::String (i + 1), trackPalette[i]);
        m.addSubMenu (TRANS ("Colore"), colours);
        m.addItem (3, TRANS ("Elimina marker"));
    }
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, markerId, pos] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& p = safe->project;
        if (r == 1) p.addMarker (pos);
        else if (r == 2) safe->renameMarkerDialog (markerId);
        else if (r == 3) p.removeMarker (markerId);
        else if (r >= 100) p.setMarkerColour (markerId, trackPalette[r - 100]);
    });
}

void TimelineView::showTrackMenu (int trackId)
{
    juce::PopupMenu m;
    if (trackId >= 0)
    {
        m.addItem (1, TRANS ("Rinomina..."));
        juce::PopupMenu colours;
        for (int i = 0; i < numPaletteColours; ++i)
            colours.addColouredItem (100 + i, "Colore " + juce::String (i + 1), trackPalette[i]);
        m.addSubMenu (TRANS ("Colore"), colours);
        m.addItem (2, TRANS ("Elimina traccia"));
        m.addItem (13, TRANS ("Blocca la traccia (Lock)"), true, project.trackLocked (trackId));
        m.addItem (14, TRANS ("Nascondi la traccia"));
        m.addItem (16, TRANS ("Mostra le corsie dei take (Lanes)"), true, project.findTrack (trackId) != nullptr && project.findTrack (trackId)->showLanes);
        m.addItem (15, project.findTrack (trackId) != nullptr && project.findTrack (trackId)->frozen != nullptr ? TRANS ("Scongela la traccia (Unfreeze)") : TRANS ("Congela la traccia (Freeze: rende EQ, compressore e plug-in)"));
        {
            juce::PopupMenu groups;
            const auto* tr = project.findTrack (trackId);
            for (auto& g : project.getSession().groups) groups.addItem (400 + g.id, g.name, true, tr != nullptr && tr->groupId == g.id);
            if (! project.getSession().groups.empty()) groups.addSeparator();
            groups.addItem (398, project.isTrackSelected (trackId) && project.selectedTracks().size() > 1
                                     ? TRANS ("Nuovo gruppo con le ") + juce::String ((int) project.selectedTracks().size()) + TRANS (" tracce selezionate...")
                                     : TRANS ("Nuovo gruppo con questa traccia..."));
            groups.addItem (397, TRANS ("Gruppo Dialoghi con tutte le tracce Dialoghi"));
            groups.addItem (399, TRANS ("Fuori dai gruppi"), tr != nullptr && tr->groupId != 0);
            m.addSubMenu (TRANS ("Gruppo"), groups);
        }
        {
            // tipo di traccia: applica la catena consigliata (EQ, compressore, limiter, livello)
            juce::PopupMenu types;
            const int cur = project.findTrack (trackId) != nullptr ? project.findTrack (trackId)->live->trackType.load() : 0;
            for (int t = 0; t < kNumTrackTypes; ++t) types.addItem (200 + t, trackTypeName (t), true, t == cur);
            m.addSubMenu (TRANS ("Tipo di traccia (preset)"), types);
        }
        m.addSeparator();
    }
    m.addItem (3, TRANS ("Nuova traccia"));
    m.addItem (4, TRANS ("Importa audio..."));
    m.addSeparator();
    m.addItem (5, TRANS ("Adatta l'altezza delle tracce"));
    if (trackId >= 0) m.addItem (6, TRANS ("Cancella l'automazione: ") + juce::String (project.findTrack (trackId) != nullptr ? autoParamName (project.findTrack (trackId)->live->autoParam.load()) : "VOL"));

    juce::Component::SafePointer<TimelineView> safe (this);
    const auto targets = project.isTrackSelected (trackId) ? project.selectedTracks() : std::vector<int> { trackId };   // il menu agisce su tutte le tracce selezionate
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, trackId, targets] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& p = safe->project;
        if (r == 1) safe->renameTrackDialog (trackId);
        else if (r == 2) p.removeTrack (trackId);
        else if (r == 3) p.addTrack();
        else if (r == 4 && safe->onImportRequested) safe->onImportRequested();
        else if (r == 5) safe->fitTrackHeights();
        else if (r == 6) p.clearAutomation (trackId);
        else if (r == 13) p.setTracksLocked (targets, ! p.trackLocked (trackId));
        else if (r == 14) p.setTracksHidden (targets, true);
        else if (r == 16) { const auto* tr = p.findTrack (trackId); p.setTracksLanes (targets, tr != nullptr && ! tr->showLanes); safe->updateScrollBars(); safe->repaint(); }
        else if (r == 15 && safe->onFreezeRequest) safe->onFreezeRequest (targets);
        else if (r == 399) p.setTracksGroup (targets, 0);
        else if (r == 398) safe->newGroupDialog (targets);
        else if (r == 397)
        {
            // tracce Dialoghi e tracce che portano il nome di un personaggio dei cue
            std::vector<int> ids;
            for (auto& t : p.getSession().tracks)
            {
                bool isCharacter = t.live->trackType.load() == TrackDialogue;
                for (auto& c : p.getSession().cues) isCharacter = isCharacter || (c.character.trim().isNotEmpty() && t.name.trim().equalsIgnoreCase (c.character));
                if (isCharacter) ids.push_back (t.id);
            }
            p.setTracksGroup (ids, p.addGroup (TRANS ("Dialoghi")));
        }
        else if (r >= 400) p.setTracksGroup (targets, r - 400);
        else if (r >= 200 && r < 200 + kNumTrackTypes) p.setTrackType (trackId, r - 200);
        else if (r >= 100) p.setTrackColour (trackId, trackPalette[r - 100]);
    });
}

void TimelineView::showClipMenu (int64_t clipId)
{
    juce::PopupMenu m;
    m.addItem (1, TRANS ("Dividi alla testina"));
    m.addItem (2, TRANS ("Duplica"));
    m.addItem (3, TRANS ("Elimina"));
    m.addSeparator();
    m.addItem (4, TRANS ("Normalizza a -1 dBFS"));
    m.addItem (5, TRANS ("Guadagno +3 dB"));
    m.addItem (6, TRANS ("Guadagno -3 dB"));
    m.addItem (7, TRANS ("Azzera guadagno"));
    if (auto* cl = project.findClip (clipId); cl != nullptr && cl->cueId != 0)
    {
        m.addSeparator();
        m.addItem (8, TRANS ("ADR: attiva il take ") + juce::String (cl->takeNo), cl->muted);
        m.addItem (9, TRANS ("ADR: elimina il take ") + juce::String (cl->takeNo));
        m.addItem (11, TRANS ("ADR: adatta alla durata del cue (time-stretch)"));
    }
    m.addSeparator();
    m.addItem (12, TRANS ("Unisci i clip selezionati (JOIN)"), project.joinableGroups (project.selection) > 0);
    m.addItem (10, TRANS ("Time-stretch..."));

    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, clipId] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& p = safe->project;
        auto* c = p.findClip (clipId);
        switch (r)
        {
            case 1: p.splitAt (safe->engine.getPosition()); break;
            case 2: p.duplicateSelected(); break;
            case 3: p.deleteSelected(); break;
            case 4: p.normalizeClip (clipId); break;
            case 5: if (c != nullptr) p.setClipGain (clipId, c->gainDb + 3.0f); break;
            case 6: if (c != nullptr) p.setClipGain (clipId, c->gainDb - 3.0f); break;
            case 7: p.setClipGain (clipId, 0.0f); break;
            case 8: if (c != nullptr) p.setActiveTake (c->cueId, c->takeNo); break;
            case 9: if (c != nullptr) p.deleteTake (c->cueId, c->takeNo); break;
            case 12: if (safe->onJoinRequest) safe->onJoinRequest(); break;
            case 10: case 11:
                if (p.selection.count (clipId) == 0) p.selection = { clipId };
                if (safe->onStretchRequest) safe->onStretchRequest (r == 11);
                break;
            default: break;
        }
    });
}

//==============================================================================
bool TimelineView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (video::isVideoFile (juce::File (f))) return true;
    for (auto& f : files)
        if (project.getFormats().findFormatForFileExtension (juce::File (f).getFileExtension()) != nullptr) return true;
    for (auto& f : files)
        if (onAdrFileDropped && adr::isCueFile (juce::File (f))) return true;
    return false;
}

void TimelineView::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropHighlight = false;
    repaint();
    juce::Array<juce::File> list;
    for (auto& f : files)
        if (video::isVideoFile (juce::File (f)) && onVideoDropped)
        {
            onVideoDropped (juce::File (f));
            return;
        }
    for (auto& f : files)
    {
        juce::File file (f);
        if (project.getFormats().findFormatForFileExtension (file.getFileExtension()) != nullptr) list.add (file);
    }
    for (auto& f : files)   // elenco dei cue ADR: va all'importazione (dialogo del formato e delle tracce)
        if (onAdrFileDropped && list.isEmpty() && adr::isCueFile (juce::File (f))) { onAdrFileDropped (juce::File (f)); return; }
    if (list.isEmpty()) return;

    const int ti = trackIndexAtY (y);
    const auto& tracks = project.getSession().tracks;
    const int trackId = (ti >= 0 && ti < (int) tracks.size()) ? tracks[(size_t) ti].id : -1;
    project.importFiles (list, trackId, juce::jmax<int64_t> (0, xToSample (x)), [] (juce::StringArray errs)
    {
        if (errs.size() > 0)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Importazione"), errs.joinIntoString ("\n"));
    });
}

} // namespace fc
