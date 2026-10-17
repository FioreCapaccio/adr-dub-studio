#pragma once

#include <map>

#include "AudioEngine.h"
#include "Commands.h"
#include "Theme.h"

namespace fc
{
class TimelineView : public juce::Component,
                     public juce::ScrollBar::Listener,
                     public juce::Timer,
                     public juce::ChangeListener,
                     public juce::FileDragAndDropTarget
{
public:
    static inline int headerW = 264;   // larghezza dell'intestazione delle tracce: cambia con il layout (Vista > Layout)
    static constexpr int knobW = 50, cueStripH = 14, groupBandH = 24, exportH = 12, rulerH = 44, sbSize = 12, videoH = 66, minTrackH = 48, maxTrackH = 400;

    TimelineView (Project&, AudioEngine&);
    ~TimelineView() override;

    // vista
    bool snapEnabled = true, followEnabled = true;
    void zoomBy (double factor, float anchorX = -1.0f);
    double getSpp() const noexcept { return spp; }
    void setSpp (double newSpp);
    /** Porta nella vista l'intervallo (campioni): scorre, e se serve riduce lo zoom, solo quando non e' gia' tutto visibile. */
    void showRange (int64_t from, int64_t to);   // zoom attorno al centro della vista
    std::function<void()> onZoomChanged;
    void zoomToFit();
    void setAllTrackHeights (int h);
    void fitTrackHeights();
    int getTrackHeight() const;

    void setTool (Tool t);
    Tool getTool() const noexcept { return tool; }
    std::function<void (Tool)> onToolChanged;

    std::function<void()> onImportRequested;
    std::function<void()> onLoopChanged;
    std::function<void()> onVideoOffsetChanged;
    std::function<void (const juce::File&)> onVideoDropped;
    std::function<void (bool toCue)> onStretchRequest;
    std::function<void (int64_t clipId, double ratio, int64_t newStart)> onStretchCommit;   // al rilascio dello strumento Stretch
    std::function<void()> onExportRequested;   // dal menu della barra di esportazione   // time-stretch dei clip selezionati (toCue: durata del cue ADR)

    void renameTrackDialog (int trackId);
    void renameMarkerDialog (int markerId);

    // Component
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

    // altri
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHighlight = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHighlight = false; repaint(); }

private:
    enum class Drag { None, Move, TrimL, TrimR, FadeIn, FadeOut, Marquee, RulerSeek, RulerLoop, ExportRange, Stretch, Trim, AutoSegment, TrackResize, VideoMove,
                      Slip, Pan, ZoomRect, TrackReorder, MarkerMove, AutoPoint, CueIn, CueOut, CueMove };
    enum class HitType { None, ExportBar, Ruler, Marker, Video, Header, HeaderGrip, HeaderAuto, HeaderAutoParam, HeaderTrim, HeaderMute, HeaderSolo, HeaderArm, HeaderInput, TrackResize,
                         Clip, ClipTrimL, ClipTrimR, ClipFadeIn, ClipFadeOut, Lane, GroupBar };
    struct Hit { HitType type = HitType::None; int track = -1; int trackIndex = -1; int64_t clip = 0; int markerId = 0; int group = 0, groupPart = 0; /* 0 barra, 1 muto, 2 solo */ };
    struct Origin { int64_t id, start, offset, length, fadeIn, fadeOut; int track; };

    // geometria
    bool hasVideo() const noexcept { return project.getSession().video != nullptr; }
    int lanesTop() const noexcept { return rulerH + (hasVideo() ? videoH : 0); }
    juce::Rectangle<int> laneArea() const { return { headerW, lanesTop(), juce::jmax (0, getWidth() - headerW - sbSize), juce::jmax (0, getHeight() - lanesTop() - sbSize) }; }
    int heightOf (int trackIdx) const;       // 0 se la traccia e' in un gruppo compresso
    int bandOf (int trackIdx) const;         // barra del gruppo sopra la traccia, se e' la prima di una serie di tracce dello stesso gruppo
    int contentTop (int trackIdx) const;     // somma delle altezze precedenti
    int contentHeight() const;
    int laneTop (int trackIdx) const { return lanesTop() + contentTop (trackIdx) - scrollY; }
    double xToSampleD (double x) const noexcept { return scrollX + (x - headerW) * spp; }
    int64_t xToSample (double x) const noexcept { return (int64_t) std::llround (xToSampleD (x)); }
    float sampleToX (int64_t s) const noexcept { return (float) (headerW + ((double) s - scrollX) / spp); }
    juce::Rectangle<float> clipRect (int trackIdx, const Clip&) const;
    juce::Rectangle<int> armRect (int ti) const { return { headerW - knobW - 98, laneTop (ti) + 9, 26, 20 }; }
    juce::Rectangle<int> muteRect (int ti) const { return { headerW - knobW - 70, laneTop (ti) + 9, 26, 20 }; }
    juce::Rectangle<int> soloRect (int ti) const { return { headerW - knobW - 42, laneTop (ti) + 9, 26, 20 }; }
    juce::Rectangle<int> inputRect (int ti) const { return { headerW - knobW - 70, laneTop (ti) + 36, 56, 18 }; }
    juce::Rectangle<int> trimRect (int ti) const { return { headerW - knobW + 4, laneTop (ti) + 4, knobW - 10, 64 }; }   // manopola del guadagno in ingresso
    // vu-meter orizzontale in basso nell'intestazione: con la scala (traccia alta) o ridotto (traccia bassa)
    juce::Rectangle<int> meterRect (int ti) const   // sotto i pulsanti, con un margine dal bordo
    {
        const int h = heightOf (ti), y = laneTop (ti), w = headerW - knobW - 50;
        if (h >= 88) return { 42, y + 60, w, 20 };
        if (h >= 76) return { 42, y + 60, w, 8 };
        if (h >= 66) return { 42, y + h - 16, w, 8 };
        return { 42, y + 36, w, 8 };
    }
    juce::Rectangle<int> autoRect (int ti) const { return { 42, laneTop (ti) + 36, 46, 18 }; }
    int trackIndexAtY (int y) const noexcept;

    Hit hitTest (juce::Point<int>) const;
    /** Maniglie dei cue ADR: nella fascia in alto delle corsie (bordo = ingresso/uscita, corpo = sposta) e sul bordo nelle corsie vuote. */
    struct CueHit { int id = 0; Drag kind = Drag::None; };
    CueHit cueHitTest (juce::Point<int>, bool onEmptyLane) const;
    void updateScrollBars();
    void clampScroll();
    double totalSamples() const;
    juce::String formatTime (double seconds, double step) const;
    double rulerStep() const;

    // pittura
    void drawRuler (juce::Graphics&);
    void drawVideoLane (juce::Graphics&);
    void drawHeader (juce::Graphics&, int trackIdx);
    void drawClip (juce::Graphics&, const Track&, int trackIdx, const Clip&, juce::Rectangle<int> lanes);
    void drawOfflineClip (juce::Graphics&, const Clip&, juce::Rectangle<float>);
    void drawWaveform (juce::Graphics&, const Clip&, juce::Rectangle<float> wf, juce::Rectangle<int> lanes);

    // interazione
    bool snapTo (int64_t s, int64_t& out) const;
    void buildSnapPoints (bool excludeSelected);
    bool handleToolMouseDown (const juce::MouseEvent&, const Hit&);
    void beginDragEdit();
    void showTrackMenu (int trackId);
    void showGroupMenu (int groupId);
    void renameGroupDialog (int groupId);
    void newGroupDialog (std::vector<int> trackIds);
    void drawGroupBars (juce::Graphics&);
    bool groupAll (int groupId, bool solo) const;   // tutte le tracce del gruppo mute / in solo
    void showClipMenu (int64_t clipId);
    void showMarkerMenu (int markerId);
    void showExportBarMenu();
    void drawExportBar (juce::Graphics&);
    // automazione del volume
    float autoYForValue (int ti, int param, float v) const;
    float autoValueForY (int ti, int param, int y) const;
    juce::Rectangle<int> paramRect (int ti) const { return { 92, laneTop (ti) + 36, 48, 18 }; }
    bool findAutoPointAt (juce::Point<int> p, int& trackIndex, int& pointId) const;
    bool findAutoSegmentAt (juce::Point<int> p, int& trackIndex, int& idA, int& idB) const;   // linea tra due punti
    void drawWaveformChannel (juce::Graphics&, const Clip&, juce::Rectangle<float>, juce::Rectangle<int>, int channel);   // -1 = tutti i canali sovrapposti

    Project& project;
    AudioEngine& engine;
    juce::ScrollBar hBar { false }, vBar { true };

    double spp = 480.0;   // campioni per pixel
    double scrollX = 0.0; // campione al bordo sinistro
    int scrollY = 0;

    Drag drag = Drag::None;
    bool dragStarted = false, dropHighlight = false;
    int64_t primaryId = 0;
    std::vector<Origin> origins;
    std::vector<int64_t> snapPoints;
    std::set<int64_t> baseSelection;
    juce::Point<int> downPos;
    juce::Rectangle<int> marquee;
    struct HeaderMeter { float dbL = -100.0f, dbR = -100.0f, holdL = -100.0f, holdR = -100.0f; int age = 0; };
    std::map<int, HeaderMeter> headerMeters;   // per id di traccia
    void updateHeaderMeters();
    void drawHeaderMeter (juce::Graphics&, int ti);
    int64_t loopAnchor = 0;
    int trimTrackId = 0, segTrackId = 0, segIdA = 0, segIdB = 0, segDownY = 0;
    float trimStartDb = 0.0f;
    int trimDownY = 0;
    int64_t segPosA = 0, segPosB = 0;
    float segValA = 0.0f, segValB = 0.0f;
    // intervallo di esportazione (barra sopra il righello)
    int exportMode = 0;                  // 0 nuovo, 1 sposta l'inizio, 2 sposta la fine, 3 sposta tutto
    int64_t exportAnchor = 0, exportGrab = 0;
    // stretch visivo di un clip: l'onda viene ridisegnata scalata mentre si trascina, l'audio si ricalcola al rilascio
    int64_t stretchId = 0, stretchHoldId = 0, stretchOrigStart = 0, stretchOrigLen = 0, stretchStart = 0, stretchLen = 0;
    bool stretchLeft = false;
    juce::Image stretchImg;
    int resizeIndex = -1, resizeOrigH = 0, hoverResize = -1;
    Tool tool = Tool::Select;
    double panScrollX = 0.0;
    int panScrollY = 0, reorderFrom = -1, reorderTo = -1;
    juce::Rectangle<int> razorGuide;
    double videoOrigOffset = 0.0;
    int markerDragId = 0, autoTrackId = 0, autoPointId = 0;
    int cueDragId = 0;
    int64_t cueOrigIn = 0, cueOrigOut = 0;

    int64_t lastPos = -1;
    float lastPlayheadX = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};

} // namespace fc
