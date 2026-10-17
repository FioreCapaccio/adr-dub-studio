#pragma once

#include "Model.h"
#include "Theme.h"
#if ADR_FFMPEG_VIDEO
 #include "FfmpegFramePlayer.h"
#endif

namespace fc
{
#if ADR_FFMPEG_VIDEO
using VideoWidget = FfmpegFramePlayer;   // Windows: ffmpeg
#else
using VideoWidget = juce::VideoComponent;   // macOS: AVFoundation
#endif

/** Mostra il video (muto) seguendo il trasporto audio: la posizione dell'audio e' sempre il riferimento. */
class VideoViewer : public juce::Component
{
public:
    VideoViewer();
    ~VideoViewer() override;

    std::function<void()> onDetachToggle, onFullscreenToggle;
    std::function<void (double)> onOffsetChanged;   // secondi
    std::function<juce::String (double)> tcFormatter;   // timecode della sessione per un tempo della timeline

    void setVideo (std::shared_ptr<VideoAsset>);
    bool hasVideo() const noexcept { return asset != nullptr; }
    void setDetached (bool d);
    void refreshOffset();   // rilegge l'offset dal VideoAsset

    /** Striscia ADR sotto l'immagine: cue, dialogo e streamer (il video nativo non si puo' coprire con disegni). */
    struct AdrInfo { bool active = false, recording = false, streamer = true; int countdown = 3; juce::String title, dialogue; double inSec = 0.0, outSec = 0.0; int dialogueSize = 30; };
    void setAdr (const AdrInfo& a) { adr = a; resized(); repaint(); }

    /** Da chiamare a ~30 Hz con la posizione dell'audio (timeline) in secondi. */
    void sync (double timeSeconds, bool playing);
    int getSeekCount() const noexcept { return seekCount; }   // quante volte il video e' stato riposizionato mentre suonava (diagnostica)
    double getVideoPosition() const { return video.getPlayPosition(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static juce::String formatTimecode (double seconds, double fps);

private:
    static constexpr int topH = 34, bottomH = 58;
    int adrH() const { return adr.active ? 44 + (int) std::round ((float) adr.dialogueSize * 1.3f * 2.0f) : 0; }   // spazio per due righe di dialogo
    AdrInfo adr;
    double curT = 0.0;
    double startLead = 0.09, displayLead = 0.0, lastSpeed = 1.0;   // anticipo alla partenza (si impara), e velocita' corrente
    bool measuredStart = false;
    int seekCount = 0;
    juce::TextLayout dialogueLayout;   // dialogo gia' impaginato (si ricalcola solo se cambiano testo, dimensione o spazio)
    juce::String dialogueKey;
    juce::Rectangle<int> adrStrip() const { return getLocalBounds().withTrimmedBottom (bottomH).removeFromBottom (adrH()); }

    juce::Rectangle<int> pictureArea() const;
    void nudge (double seconds);
    void commitLabel();

    VideoWidget video { false };
    std::shared_ptr<VideoAsset> asset;
    bool loaded = false, running = false, detached = false;
    double lastSetPos = -1.0, holdoffUntil = 0.0, lastShown = -1.0;
    juce::String timecodeText;

    juce::TextButton detachBtn { TRANS ("STACCA") }, fullBtn { TRANS ("TUTTO SCHERMO") };
    juce::TextButton minusFrame { "-F" }, minusMs { "-" }, plusMs { "+" }, plusFrame { "+F" }, resetBtn { "0" };
    juce::Label offsetLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoViewer)
};

/** Finestra separata che ospita il viewer (spostabile su un altro schermo, a tutto schermo). */
class VideoWindow : public juce::DocumentWindow
{
public:
    explicit VideoWindow (std::function<void()> closeCallback)
        : juce::DocumentWindow ("Video - ADR Dub Studio", juce::Colour (0xff08090b), juce::DocumentWindow::allButtons),
          onClose (std::move (closeCallback))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setResizeLimits (480, 360, 8000, 6000);
    }

    void closeButtonPressed() override { if (onClose) onClose(); }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey && isFullScreen()) { setFullScreen (false); return true; }
        return false;
    }

private:
    std::function<void()> onClose;
};

} // namespace fc
