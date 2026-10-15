#include "MainComponent.h"
#include "VideoSupport.h"
#include "Commands.h"
#include "ExportDialog.h"
#include "Settings.h"
#include "SessionSettings.h"
#include "AdrImport.h"

namespace fc
{
using namespace theme;

namespace
{


juce::File settingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("ArotronStudio").getChildFile ("audio.xml");
}

juce::File appDataFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support").getChildFile ("ArotronStudio");
}
juce::File autosaveFile() { return appDataFolder().getChildFile ("autosave.arostudio"); }
juce::File autosaveInfo() { return appDataFolder().getChildFile ("autosave.info"); }
juce::File runningFlag() { return appDataFolder().getChildFile ("running.flag"); }

juce::File recordingsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ADR Dub Studio").getChildFile ("Registrazioni");
}
} // namespace

//==============================================================================
/** Esporta l'audio della sessione (mixdown o stem) con barra di avanzamento. */
class ExportJob : public juce::ThreadWithProgressWindow
{
public:
    ExportJob (Session s, double r, ExportOptions o, juce::File t, juce::String name, std::function<void (ExportResult)> d)
        : juce::ThreadWithProgressWindow ("Esportazione in corso...", true, true),
          session (std::move (s)), rate (r), options (std::move (o)), target (std::move (t)), baseName (std::move (name)), done (std::move (d)) {}

    void run() override
    {
        result = exportSession (session, rate, options, target, baseName, [this] (double p) { setProgress (p); return ! threadShouldExit(); });
    }

    void threadComplete (bool) override { if (done) done (result); }

private:
    Session session;
    double rate;
    ExportOptions options;
    juce::File target;
    juce::String baseName;
    std::function<void (ExportResult)> done;
    ExportResult result;
};

/** Esporta un .mov con il video originale (non ricodificato) e il mix della sessione come audio. */
class VideoExportJob : public juce::ThreadWithProgressWindow
{
public:
    /** from/to: intervallo della timeline in campioni (to <= from = tutto il video). */
    VideoExportJob (Session s, double r, juce::File v, double off, juce::File o, int64_t from, int64_t to, std::function<void (bool, juce::String)> d)
        : juce::ThreadWithProgressWindow ("Esportazione video con mix...", true, true),
          session (std::move (s)), rate (r), videoFile (std::move (v)), offset (off), outFile (std::move (o)), rangeFrom (from), rangeTo (to), done (std::move (d)) {}

    void run() override
    {
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("arotron_mix", ".wav");
        juce::String msg;
        ok = renderMixdown (session, rate, tmp, [this] (double p) { setProgress (p * 0.4); return ! threadShouldExit(); }, msg, rangeFrom, rangeTo);
        if (ok)
            ok = video::muxAudio (videoFile, tmp, outFile, offset, [this] (double p) { setProgress (0.4 + 0.6 * p); return ! threadShouldExit(); }, msg,
                                  rangeTo > rangeFrom ? (double) rangeFrom / rate : 0.0, rangeTo > rangeFrom ? (double) rangeTo / rate : -1.0);
        tmp.deleteFile();
        message = ok ? "Video esportato in " + outFile.getFullPathName() : msg;
    }

    void threadComplete (bool) override { if (done) done (ok, message); }

private:
    Session session;
    double rate;
    juce::File videoFile;
    double offset = 0.0;
    juce::File outFile;
    int64_t rangeFrom = 0, rangeTo = -1;
    std::function<void (bool, juce::String)> done;
    bool ok = false;
    juce::String message;
};

//==============================================================================
MainComponent::MainComponent()
{
    formats.registerBasicFormats();

    std::unique_ptr<juce::XmlElement> saved = juce::XmlDocument::parse (settingsFile());
    engine.initialise (saved.get());

    pluginHost = std::make_unique<PluginHost> (engine);
    project = std::make_unique<Project> (formats, engine.getSampleRate());
    project->onPublish = [this] (std::shared_ptr<const Session> s) { engine.publish (std::move (s)); };
    project->onSessionLoaded = [this] { pluginHost->restoreAll (project->getSession()); };
    project->isPlayingFn = [this] { return engine.isPlaying(); };
    project->positionFn = [this] { return engine.getPosition(); };
    pluginHost->onChanged = [this] { project->liveChanged(); };
    pluginHost->onError = [this] (const juce::String& m) { showError (m); };
    project->newSession();

    engine.onSampleRateChanged = [this] (double rate)
    {
        const double old = project->getSampleRate();
        if (std::abs (rate - old) < 0.5) return;
        const double ratio = rate / old;
        project->setSampleRate (rate);
        pluginHost->reprepareAll (project->getSession());
        engine.setPosition ((int64_t) ((double) engine.getPosition() * ratio));
        engine.setLoop (engine.isLooping(), (int64_t) ((double) engine.getLoopStart() * ratio),
                        (int64_t) ((double) engine.getLoopEnd() * ratio));
    };

    timeline = std::make_unique<TimelineView> (*project, engine);
    editBar = std::make_unique<EditBar> (commandManager);
    addAndMakeVisible (*editBar);
    addAndMakeVisible (mixerResizer);
    editBar->onToolSelected = [this] (Tool t) { timeline->setTool (t); };
    editBar->onHeightChanged = [this] (int h) { timeline->setAllTrackHeights (h); };
    editBar->onFitHeights = [this] { timeline->fitTrackHeights(); };
    editBar->onZoomChanged = [this] (double spp) { timeline->setSpp (spp); };
    timeline->onZoomChanged = [this] { editBar->setZoomValue (timeline->getSpp()); };
    editBar->setZoomValue (timeline->getSpp());
    timeline->onToolChanged = [this] (Tool t) { editBar->setTool (t); commandManager.commandStatusChanged(); };
    mixerResizer.onStart = [this] { mixerHeightAtDrag = mixerHeight; };
    mixerResizer.onDrag = [this] (int dy) { mixerUserSized = true; mixerHeight = juce::jlimit (240, 700, mixerHeightAtDrag - dy); layoutAll(); };
    mixer = std::make_unique<MixerView> (*project, *pluginHost, engine.loudness,
                                         [this] (float* dest, int n) { engine.readSpectrumSamples (dest, n); }, [this] { return engine.getSampleRate(); });
    addAndMakeVisible (*timeline);
    addAndMakeVisible (*mixer);
    timeline->onImportRequested = [this] { importDialog(); };
    timeline->onLoopChanged = [this] { syncToggles(); commandManager.commandStatusChanged(); };
    timeline->onVideoDropped = [this] (const juce::File& f) { loadVideo (f); };
    timeline->onStretchRequest = [this] (bool toCue) { timeStretchSelected (toCue); };
    timeline->onStretchCommit = [this] (int64_t id, double ratio, int64_t newStart)
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ADR Dub Studio").getChildFile ("Elaborati");
        juce::Component::SafePointer<MainComponent> safe (this);
        project->stretchClip (id, ratio, true, folder, [safe] (juce::String err) { if (safe != nullptr && err.isNotEmpty()) safe->showError (err); }, newStart);
    };
    timeline->onExportRequested = [this] { exportDialog(); };
    timeline->onVideoOffsetChanged = [this] { viewer.refreshOffset(); };
    addChildComponent (viewer);
    viewer.onDetachToggle = [this] { setViewerDetached (videoWindow == nullptr); };
    viewer.onFullscreenToggle = [this] { toggleVideoFullscreen(); };
    viewer.onOffsetChanged = [this] (double s) { project->setVideoOffset (s); timeline->repaint(); };
    viewer.tcFormatter = [this] (double s) { return project->formatTc (s); };
    addAndMakeVisible (inMeter);

    for (juce::Button* b : std::initializer_list<juce::Button*> { &rewindBtn, &playBtn, &stopBtn, &recBtn, &loopBtn, &snapBtn, &followBtn, &monBtn, &zoomOutBtn, &zoomInBtn,
                     &fitBtn, &importBtn, &exportBtn, &addTrackBtn, &videoBtn, &mixerBtn })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    for (auto* b : { &loopBtn, &snapBtn, &followBtn, &monBtn, &videoBtn, &mixerBtn })
        b->setClickingTogglesState (true);
    recBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5a2420));
    recBtn.setColour (juce::TextButton::buttonOnColourId, accent);
    recBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffff8a7d));
    playBtn.setColour (juce::TextButton::buttonOnColourId, green.darker (0.3f));

    rewindBtn.onClick = [this] { engine.setPosition (0); };
    playBtn.onClick = [this] { toggleTransport(); };
    stopBtn.onClick = [this] { stopAll(); };
    recBtn.onClick = [this] { if (engine.isRecording()) finishRecording(); else startRecording(); };
    monBtn.onClick = [this] { engine.monitorInput = monBtn.getToggleState(); };
    loopBtn.onClick = [this] { toggleLoop(); };
    snapBtn.onClick = [this] { timeline->snapEnabled = snapBtn.getToggleState(); };
    followBtn.onClick = [this] { timeline->followEnabled = followBtn.getToggleState(); };
    zoomOutBtn.onClick = [this] { timeline->zoomBy (1.0 / 1.5); };
    zoomInBtn.onClick = [this] { timeline->zoomBy (1.5); };
    fitBtn.onClick = [this] { timeline->zoomToFit(); };
    importBtn.onClick = [this] { importDialog(); };
    exportBtn.onClick = [this] { exportDialog(); };
    addTrackBtn.onClick = [this] { project->addTrack(); };
    videoBtn.onClick = [this] { viewerVisible = videoBtn.getToggleState(); layoutAll(); };
    mixerBtn.onClick = [this] { mixerVisible = mixerBtn.getToggleState(); mixer->setVisible (mixerVisible); resized(); };

    rewindBtn.setTooltip ("Vai all'inizio (Home)");
    playBtn.setTooltip ("Play / Stop (barra spaziatrice)");
    stopBtn.setTooltip ("Stop");
    recBtn.setTooltip ("Registra sulle tracce armate (R). Se nessuna e' armata usa la traccia selezionata");
    loopBtn.setTooltip ("Loop (L). Maiusc + trascina sul righello per definire la regione");
    snapBtn.setTooltip ("Aggancio ai bordi dei clip, alla testina e ai fotogrammi (N)");
    followBtn.setTooltip ("La vista segue la testina durante la riproduzione (F)");
    monBtn.setTooltip ("Ascolta l'ingresso durante la registrazione (M). Usa le cuffie per evitare il feedback");
    zoomOutBtn.setTooltip ("Zoom indietro (-)");
    zoomInBtn.setTooltip ("Zoom avanti (+)");
    fitBtn.setTooltip ("Adatta il progetto alla finestra (Maiusc + Z)");
    addTrackBtn.setTooltip ("Aggiunge una traccia (Maiusc + Cmd + T)");
    importBtn.setTooltip ("Importa file audio (Cmd + I)");
    exportBtn.setTooltip ("Esporta il mixdown in WAV, AIFF o FLAC (Cmd + E)");
    videoBtn.setTooltip ("Mostra o nasconde il video (Maiusc + V)");
    mixerBtn.setTooltip ("Mostra o nasconde il mixer (Maiusc + M)");
    inMeter.setTooltip ("Livello dell'ingresso audio");

    snapBtn.setToggleState (true, juce::dontSendNotification);
    followBtn.setToggleState (true, juce::dontSendNotification);
    mixerBtn.setToggleState (true, juce::dontSendNotification);
    videoBtn.setToggleState (true, juce::dontSendNotification);

    commandManager.registerAllCommandsForTarget (this);
    commandManager.setFirstCommandTarget (this);
    setApplicationCommandManagerToWatch (&commandManager);
    addKeyListener (commandManager.getKeyMappings());
    timeline->addKeyListener (commandManager.getKeyMappings());
    setWantsKeyboardFocus (true);

    project->addChangeListener (this);
    setSize (1440, 900);
    startTimerHz (30);
    timeline->zoomToFit();
    updateTitle();
    checkRecovery();
}

MainComponent::~MainComponent()
{
    stopTimer();
    project->removeChangeListener (this);
    engine.onSampleRateChanged = nullptr;
    if (engine.isRecording()) { engine.stop(); engine.stopRecording(); }
    pluginHost->closeAllWindows();
    runningFlag().deleteFile();      // chiusura regolare: niente recupero al prossimo avvio
    autosaveFile().deleteFile();
    autosaveInfo().deleteFile();
    if (videoWindow != nullptr) { videoWindow->clearContentComponent(); videoWindow.reset(); }
    adrWindow.reset();

    if (auto xml = engine.deviceManager.createStateXml())
    {
        auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        xml->writeTo (f);
    }
    engine.shutdown();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    // barra degli strumenti
    g.fillAll (bg);
    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, 0.0f, panel, 0.0f, (float) toolbarH, false));
    g.fillRect (0, 0, getWidth(), toolbarH);
    g.setColour (border);
    g.drawHorizontalLine (toolbarH - 1, 0.0f, (float) getWidth());

    for (auto& c : capsules)
    {
        auto f = c.toFloat();
        g.setColour (juce::Colour (0xff111316));
        g.fillRoundedRectangle (f, 10.0f);
        g.setColour (border);
        g.drawRoundedRectangle (f.reduced (0.5f), 10.0f, 1.0f);
    }

    // display del timecode
    {
        const bool rec = engine.isRecording();
        auto f = timeBox.toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff050607), 0.0f, f.getY(), juce::Colour (0xff0d0f11), 0.0f, f.getBottom(), false));
        g.fillRoundedRectangle (f, 10.0f);
        g.setColour (rec ? accent : border);
        g.drawRoundedRectangle (f.reduced (0.5f), 10.0f, rec ? 2.0f : 1.0f);

        const int64_t pos = engine.getPosition();
        const double rate = project->getSampleRate();
        const auto tc = project->formatTc ((double) pos / rate);
        auto digits = timeBox.withTrimmedTop (14).withTrimmedBottom (13);
        g.setFont (mono (27.0f));
        g.setColour (playhead.withAlpha (0.18f));
        g.drawText (tc, digits.translated (0, 1), juce::Justification::centred);   // alone luminoso
        g.setColour (rec ? juce::Colours::white : playhead);
        g.drawText (tc, digits, juce::Justification::centred);

        drawCaption (g, rec ? "REC" : "Timecode", timeBox.withTrimmedLeft (12).withHeight (12).translated (0, 3), juce::Justification::centredLeft, rec ? accent : dim);
        drawCaption (g, juce::String (project->tcFmt().name), timeBox.withTrimmedRight (12).withHeight (12).translated (0, 3), juce::Justification::centredRight);
        g.setColour (dim);
        g.setFont (mono (10.5f));
        g.drawText (juce::String ((double) pos / rate, 3) + " s", timeBox.withTrimmedTop (timeBox.getHeight() - 15).translated (0, -1), juce::Justification::centred);
    }

    // barra di stato
    g.setColour (panel);
    g.fillRect (statusBox);
    g.setColour (border);
    g.drawHorizontalLine (statusBox.getY(), 0.0f, (float) getWidth());
    g.setFont (font (11.0f));
    juce::String info;
    if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
    {
        info << "Uscita: " << dev->getName() << "   " << juce::String (dev->getCurrentSampleRate() / 1000.0, 1) << " kHz   "
             << dev->getCurrentBufferSizeSamples() << " spl";
        if (engine.getNumInputChannels() > 0) info << "   Ingressi: " << engine.getNumInputChannels();
    }
    else
        info << "Nessun dispositivo audio";
    if (engine.getPdcLatency() > 0)
        info << "   Latenza plug-in " << juce::String (engine.getPdcLatency() * 1000.0 / project->getSampleRate(), 1) << " ms"
             << (engine.pdcOn.load() ? " (compensata)" : " (NON compensata)");
    info << "   CPU " << juce::String ((int) std::round (engine.deviceManager.getCpuUsage() * 100.0)) << "%";
    g.setColour (dim);
    g.drawText (info, statusBox.reduced (10, 0), juce::Justification::centredRight, true);

    juce::String left;
    juce::Colour leftCol = dim;
    if (engine.isRecording())
    {
        left = "REGISTRAZIONE IN CORSO";
        leftCol = accent;
        if (engine.getDroppedBlocks() > 0) left << "  -  attenzione: blocchi persi";
    }
    else if (project->getBusyCount() > 0) { left = "Caricamento..."; leftCol = yellow; }
    else if (project->getSession().video != nullptr) left = "Video: " + project->getSession().video->file.getFileName();
    g.setColour (leftCol);
    g.drawText (left, statusBox.reduced (10, 0), juce::Justification::centredLeft, true);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    const float p = project->getTaskProgress();
    if (p < 0.0f) return;
    const auto label = project->getTaskLabel();
    const auto box = juce::Rectangle<int> (0, 0, 460, 92).withCentre (getLocalBounds().getCentre());
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRect (getLocalBounds());
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (box.toFloat().translated (0.0f, 4.0f), 12.0f);
    g.setColour (panelHi);
    g.fillRoundedRectangle (box.toFloat(), 12.0f);
    g.setColour (border);
    g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 12.0f, 1.0f);
    g.setColour (text);
    g.setFont (font (14.0f, true));
    g.drawText (label.isEmpty() ? juce::String ("Caricamento in corso...") : label, box.reduced (22, 12).removeFromTop (24), juce::Justification::centredLeft, true);
    auto bar = box.reduced (22, 0).withTrimmedTop (44).withHeight (14).toFloat();
    g.setColour (juce::Colour (0xff0a0b0d));
    g.fillRoundedRectangle (bar, 7.0f);
    g.setGradientFill (juce::ColourGradient (blue.brighter (0.2f), bar.getX(), 0.0f, green, bar.getRight(), 0.0f, false));
    g.fillRoundedRectangle (bar.withWidth (juce::jmax (14.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, p))), 7.0f);
    g.setColour (dim);
    g.setFont (mono (12.0f));
    g.drawText (juce::String ((int) std::round (p * 100.0f)) + " %", box.reduced (22, 8).removeFromBottom (22), juce::Justification::centredRight);
}

void MainComponent::layoutAll()
{
    auto r = getLocalBounds();
    r.removeFromTop (toolbarH);
    statusBox = r.removeFromBottom (24);
    mixerResizer.setVisible (mixerVisible);
    if (mixerVisible)
    {
        // finche' l'utente non lo ridimensiona, il mixer occupa circa il 45% della finestra: cosi' i fader hanno spazio per la scala
        if (! mixerUserSized) mixerHeight = juce::jlimit (370, 500, (int) (getHeight() * 0.47));
        mixer->setBounds (r.removeFromBottom (mixerHeight));
        mixerResizer.setBounds (r.removeFromBottom (7));
    }

    const bool hasVideo = project->getSession().video != nullptr;
    const bool detached = videoWindow != nullptr;
    const bool showDocked = viewerVisible && hasVideo && ! detached;
    viewer.setVisible (detached ? true : showDocked);
    if (detached) videoWindow->setVisible (viewerVisible && hasVideo);
    if (showDocked) viewer.setBounds (r.removeFromRight (juce::jlimit (360, 620, (int) (r.getWidth() * 0.36))));
    editBar->setBounds (r.removeFromTop (40));
    timeline->setBounds (r);
    if (pendingFit && timeline->getHeight() > 120)   // all'avvio (e con un nuovo video) le tracce si adattano alla finestra: si vedono tutte
    {
        pendingFit = false;
        timeline->fitTrackHeights();
    }
    repaint();
}

void MainComponent::resized()
{
    capsules.clear();
    auto area = getLocalBounds().removeFromTop (toolbarH).reduced (12, 10);

    // ogni gruppo e' una "capsula" con i suoi controlli
    auto group = [&] (int contentW) -> juce::Rectangle<int>
    {
        auto c = area.removeFromLeft (contentW + 16);
        area.removeFromLeft (10);
        capsules.push_back (c);
        return c.reduced (8, 6);
    };
    auto put = [] (juce::Rectangle<int>& r, juce::Component& c, int w, int gap = 6)
    {
        c.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (gap);
    };

    auto g1 = group (40 + 6 + 48 + 6 + 40 + 6 + 48);
    put (g1, rewindBtn, 40); put (g1, playBtn, 48); put (g1, stopBtn, 40); put (g1, recBtn, 48, 0);

    timeBox = area.removeFromLeft (232);
    area.removeFromLeft (10);

    auto g3 = group (56 + 6 + 56 + 6 + 62);
    put (g3, loopBtn, 56); put (g3, snapBtn, 56); put (g3, followBtn, 62, 0);

    auto g4 = group (16 + 8 + 48);
    inMeter.setBounds (g4.removeFromLeft (16).reduced (0, 1));
    g4.removeFromLeft (8);
    put (g4, monBtn, 48, 0);

    auto g5 = group (32 + 6 + 32 + 6 + 66);
    put (g5, zoomOutBtn, 32); put (g5, zoomInBtn, 32); put (g5, fitBtn, 66, 0);

    auto g6 = group (96 + 6 + 74 + 6 + 74 + 6 + 62 + 6 + 66);
    put (g6, addTrackBtn, 96); put (g6, importBtn, 74); put (g6, exportBtn, 74); put (g6, videoBtn, 62); put (g6, mixerBtn, 66, 0);

    layoutAll();
    repaint (0, 0, getWidth(), toolbarH);
}

void MainComponent::setViewerDetached (bool detach)
{
    if (detach == (videoWindow != nullptr)) return;

    if (detach)
    {
        removeChildComponent (&viewer);
        videoWindow = std::make_unique<VideoWindow> ([this] { setViewerDetached (false); });
        videoWindow->setContentNonOwned (&viewer, false);
        videoWindow->addKeyListener (commandManager.getKeyMappings());
        videoWindow->setSize (960, 640);
        videoWindow->centreAroundComponent (this, 960, 640);
        viewer.setDetached (true);
        viewer.setVisible (true);
        videoWindow->setVisible (true);
    }
    else
    {
        videoWindow->setFullScreen (false);
        videoWindow->clearContentComponent();
        videoWindow.reset();
        addChildComponent (viewer);
        viewer.setDetached (false);
    }
    layoutAll();
    commandManager.commandStatusChanged();
}

void MainComponent::toggleVideoFullscreen()
{
    if (project->getSession().video == nullptr) return;
    if (videoWindow == nullptr) setViewerDetached (true);
    videoWindow->setFullScreen (! videoWindow->isFullScreen());
}

void MainComponent::timerCallback()
{
    const int64_t pos = engine.getPosition();
    if (pos != lastShownPos)
    {
        lastShownPos = pos;
        repaint (timeBox);
    }
    const bool playing = engine.isPlaying();

    viewer.sync ((double) (pos - (playing ? engine.getPdcLatency() : 0)) / project->getSampleRate(), playing);   // il video segue cio' che si sente
    inMeter.setLevels (engine.inPeakL.exchange (0.0f), engine.inPeakR.exchange (0.0f));

    if (engine.isRecording() && ! playing) { finishRecording(); return; }
    if (adrRecCue != 0 && engine.isRecording() && pos >= adrRecStop) { finishRecording(); return; }
    if (engine.getAuditionClip() != 0 && (! playing || pos >= auditionStop)) endAudition();
    if (recBtn.getToggleState() != engine.isRecording())
    {
        recBtn.setClickingTogglesState (false);
        recBtn.setToggleState (engine.isRecording(), juce::dontSendNotification);
        repaint (0, 0, getWidth(), toolbarH);
        repaint (statusBox);
    }
    if (playBtn.getToggleState() != playing)
    {
        playBtn.setClickingTogglesState (false);
        playBtn.setToggleState (playing, juce::dontSendNotification);
    }
    {
        const float tp = project->getTaskProgress();
        const int pct = tp < 0.0f ? -1 : (int) std::round (tp * 100.0f);
        if (pct != lastTaskPct) { lastTaskPct = pct; repaint(); }
    }
    if (++secondTick >= 30)
    {
        secondTick = 0;
        if (++secondsSinceAutosave >= 120) { secondsSinceAutosave = 0; autosaveTick(); }
    }
    if (++infoTick >= 15)
    {
        infoTick = 0;
        repaint (statusBox);
    }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (shownVideo != project->getSession().video)
    {
        shownVideo = project->getSession().video;
        viewer.setVideo (shownVideo);
        if (shownVideo == nullptr && videoWindow != nullptr) setViewerDetached (false);
        pendingFit = true;
        layoutAll();
    }
    viewer.refreshOffset();
    updateAdrViewer();
    pluginHost->pruneWindows (project->getSession());
    editBar->setHeightValue (timeline->getTrackHeight());
    updateTitle();
    commandManager.commandStatusChanged();
    repaint (statusBox);
}

void MainComponent::updateTitle()
{
    if (onTitleChanged)
    {
        auto name = project->getFile() == juce::File() ? juce::String ("Senza titolo") : project->getFile().getFileNameWithoutExtension();
        onTitleChanged (name + (project->isDirty() ? " *" : "") + " - ADR Dub Studio");
    }
}

void MainComponent::syncToggles()
{
    loopBtn.setToggleState (engine.isLooping(), juce::dontSendNotification);
}

void MainComponent::showError (const juce::String& msg)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "ADR Dub Studio", msg);
}

//==============================================================================
void MainComponent::toggleTransport()
{
    if (engine.isRecording()) { finishRecording(); return; }
    if (engine.isPlaying()) { engine.stop(); return; }
    mixer->resetPeaks();
    engine.play();
}

void MainComponent::stopAll()
{
    if (engine.isRecording()) finishRecording(); else engine.stop();
}

void MainComponent::armSelectedTrack()
{
    if (auto* t = project->findTrack (project->selectedTrackId))
    {
        t->live->armed = ! t->live->armed.load();
        if (t->live->armed.load()) engine.ensureInput();   // il vu-meter della traccia funziona anche prima di registrare
        project->liveChanged();
    }
}

void MainComponent::startRecording()
{
    if (engine.isRecording()) return;

    bool any = false;
    for (auto& t : project->getSession().tracks) any |= t.live->armed.load();
    if (! any) armSelectedTrack();   // nessuna traccia armata: usa quella selezionata

    if (! engine.ensureInput())
    {
        showError ("Non riesco ad aprire un ingresso audio.\n\nControlla che il microfono sia consentito in Impostazioni di Sistema > Privacy e sicurezza > Microfono, "
                   "e scegli l'ingresso in File > Impostazioni audio.");
        return;
    }

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const auto session = project->getFile() == juce::File() ? juce::String ("Sessione") : project->getFile().getFileNameWithoutExtension();
    std::vector<TakeSpec> specs;
    for (auto& t : project->getSession().tracks)
        if (t.live->armed.load())
            specs.push_back ({ t.id,
                               recordingsFolder().getChildFile (juce::File::createLegalFileName (session + "_" + t.name + "_" + stamp) + ".wav"),
                               t.live->inputMode.load() });

    const auto err = engine.startRecording (specs);
    if (err.isNotEmpty()) { showError (err); return; }
    engine.play();
    repaint();
}

void MainComponent::finishRecording()
{
    engine.stop();
    engine.clearPunch();
    engine.clearBeeps();
    const int drops = engine.getDroppedBlocks();
    const auto results = engine.stopRecording();
    const int cueId = adrRecCue;
    adrRecCue = 0;
    updateAdrViewer();
    repaint();
    if (results.empty()) return;

    juce::String takeName;
    if (const auto* cue = project->findCue (cueId))
        takeName = juce::File::createLegalFileName ((cue->character.isEmpty() ? juce::String ("ADR") : cue->character) + "_" + cue->scene + "_" + juce::String (cue->number));
    std::vector<Project::RecItem> items;
    for (auto& r : results) items.push_back ({ r.trackId, r.file, r.startPos, r.latency, cueId, takeName });

    juce::Component::SafePointer<MainComponent> safe (this);
    project->addRecordings (items, [safe, drops] (juce::StringArray errs)
    {
        if (safe == nullptr) return;
        if (drops > 0) errs.add ("Durante la registrazione sono stati persi alcuni blocchi audio: il disco o il computer non reggevano il ritmo.");
        if (errs.size() > 0) safe->showError (errs.joinIntoString ("\n"));
    });
}

void MainComponent::toggleLoop()
{
    if (engine.isLooping())
        engine.setLoop (false, engine.getLoopStart(), engine.getLoopEnd());
    else
    {
        if (engine.getLoopEnd() <= engine.getLoopStart())
            setLoopFromSelection();
        else
            engine.setLoop (true, engine.getLoopStart(), engine.getLoopEnd());
    }
    syncToggles();
    timeline->repaint();
}

void MainComponent::setLoopFromSelection()
{
    int64_t a = std::numeric_limits<int64_t>::max(), b = 0;
    for (auto& t : project->getSession().tracks)
        for (auto& c : t.clips)
            if (project->selection.count (c.id) > 0) { a = juce::jmin (a, c.start); b = juce::jmax (b, c.end()); }
    if (b <= a) { a = 0; b = project->contentEnd(); }
    if (b > a) engine.setLoop (true, a, b);
    syncToggles();
    timeline->repaint();
}

void MainComponent::confirmDiscard (std::function<void()> proceed)
{
    if (! project->isDirty()) { proceed(); return; }
    auto opts = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, "Modifiche non salvate",
                                                                 "Vuoi salvare le modifiche alla sessione?", "Salva", "Non salvare", "Annulla");
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::NativeMessageBox::showAsync (opts, [safe, proceed] (int r)
    {
        if (safe == nullptr) return;
        // NativeMessageBox restituisce l'indice del pulsante (da 0): 0 = Salva, 1 = Non salvare, 2 = Annulla
        if (r == 0)
        {
            if (safe->project->getFile() != juce::File())
            {
                juce::String err;
                if (safe->project->save (safe->project->getFile(), err)) proceed(); else safe->showError (err);
            }
            else
            {
                safe->chooser = std::make_unique<juce::FileChooser> ("Salva sessione", juce::File(), "*.arostudio");
                safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                                | juce::FileBrowserComponent::warnAboutOverwriting,
                                            [safe, proceed] (const juce::FileChooser& fc)
                {
                    auto f = fc.getResult();
                    if (safe == nullptr || f == juce::File()) return;
                    juce::String err;
                    if (safe->project->save (f.withFileExtension ("arostudio"), err)) proceed(); else safe->showError (err);
                });
            }
        }
        else if (r == 1) proceed();
    });
}

void MainComponent::autosaveTick()
{
    if (! project->isDirty() || project->getBusyCount() > 0 || engine.isRecording()) return;
    juce::String err;
    if (project->saveCopy (autosaveFile(), err))
        autosaveInfo().replaceWithText (project->getFile() == juce::File() ? juce::String() : project->getFile().getFullPathName());
}

void MainComponent::checkRecovery()
{
    const bool crashed = runningFlag().existsAsFile() && autosaveFile().existsAsFile();
    appDataFolder().createDirectory();
    runningFlag().replaceWithText (juce::Time::getCurrentTime().toString (true, true));
    if (! crashed) return;

    const auto when = autosaveFile().getLastModificationTime().toString (true, true);
    auto opts = juce::MessageBoxOptions::makeOptionsYesNo (juce::MessageBoxIconType::QuestionIcon, "Recupero della sessione",
                                                          "ADR Dub Studio non e' stato chiuso correttamente.\n\nVuoi ripristinare la copia di sicurezza del " + when + "?",
                                                          "Ripristina", "Ignora");
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::NativeMessageBox::showAsync (opts, [safe] (int r)
    {
        if (safe == nullptr || r != 0) return;   // 0 = Ripristina
        const auto original = autosaveInfo().loadFileAsString().trim();
        safe->project->loadSession (autosaveFile(), [safe, original] (juce::String errs)
        {
            if (safe == nullptr) return;
            safe->project->setFile (original.isNotEmpty() ? juce::File (original) : juce::File());
            safe->project->markDirty();
            safe->timeline->zoomToFit();
            if (errs.isNotEmpty()) safe->showError (errs);
        });
    });
}

void MainComponent::addRecent (const juce::File& f)
{
    auto& st = settings();
    juce::StringArray list = juce::StringArray::fromLines (st.getValue ("recent", ""));
    list.removeString (f.getFullPathName());
    list.insert (0, f.getFullPathName());
    while (list.size() > 10) list.remove (list.size() - 1);
    st.setValue ("recent", list.joinIntoString ("\n"));
    st.saveIfNeeded();
}

void MainComponent::openFile (const juce::File& f)
{
    if (! f.existsAsFile())
    {
        showError ("Il file non esiste piu': " + f.getFullPathName());
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    confirmDiscard ([safe, f]
    {
        if (safe == nullptr) return;
        safe->engine.stop();
        safe->engine.setPosition (0);
        safe->project->loadSession (f, [safe, f] (juce::String errs)
        {
            if (safe == nullptr) return;
            safe->addRecent (f);
            safe->timeline->zoomToFit();
            if (errs.isNotEmpty()) safe->showError (errs);
        });
    });
}

void MainComponent::showSessionSettings()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* dlg = new SessionSettingsDialog (*project, [safe] (const SessionSettingsDialog::Values& v)
    {
        if (safe == nullptr) return;
        auto& p = *safe->project;
        p.tcFormat = v.tcFormat;
        p.tcStartSec = v.tcStart;
        p.rulerTimecode = v.rulerTc;
        p.adrPreRoll = v.preRoll;
        p.adrPostRoll = v.postRoll;
        p.adrBeeps = v.beeps;
        p.adrStreamer = v.streamer;
        p.adrDialogueSize = v.dialogueSize;
        safe->adrViewerKey.clear();
        p.markDirty();
        safe->repaint();
        safe->timeline->repaint();
    });
    dlg->setSize (520, 510);
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (dlg);
    lo.dialogTitle = "Impostazioni sessione: timecode e ADR";
    lo.dialogBackgroundColour = panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    lo.launchAsync();
}

void MainComponent::showShortcuts()
{
    juce::String t;
    t << "SCORCIATOIE DA TASTIERA\n\n";
    juce::StringArray cats = commandManager.getCommandCategories();
    for (auto& cat : cats)
    {
        t << cat.toUpperCase() << "\n";
        for (auto id : commandManager.getCommandsInCategory (cat))
        {
            if (auto* info = commandManager.getCommandForID (id))
            {
                juce::String keys;
                for (auto& kp : commandManager.getKeyMappings()->getKeyPressesAssignedToCommand (id))
                    keys << (keys.isEmpty() ? "" : "  /  ") << kp.getTextDescriptionWithIcons();
                t << "  " << info->shortName.paddedRight (' ', 52) << keys << "\n";
            }
        }
        t << "\n";
    }
    t << "MOUSE\n"
         "  Trascina il bordo inferiore di una traccia      altezza traccia (Maiusc: tutte)\n"
         "  Doppio clic sul nome della traccia              rinomina\n"
         "  Clic sul righello                               sposta la testina\n"
         "  Maiusc + trascina sul righello                  regione di loop\n"
         "  Doppio clic sul righello                        elimina la regione di loop\n"
         "  Cmd + rotella / pinch                           zoom orizzontale\n"
         "  Alt + rotella                                   altezza delle tracce\n"
         "  Clic destro su clip, traccia, marker            menu contestuale\n";

    auto* ed = new juce::TextEditor();
    ed->setMultiLine (true);
    ed->setReadOnly (true);
    ed->setFont (mono (12.0f));
    ed->setColour (juce::TextEditor::backgroundColourId, bg);
    ed->setText (t, false);
    ed->setCaretPosition (0);
    ed->setSize (700, 640);

    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (ed);
    lo.dialogTitle = "Guida rapida e scorciatoie";
    lo.dialogBackgroundColour = bg;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = true;
    lo.launchAsync();
}

/** Copia i file audio e video usati dalla sessione in una cartella accanto al file di sessione. */
class CollectJob : public juce::ThreadWithProgressWindow
{
public:
    struct Item { std::shared_ptr<Asset> asset; std::shared_ptr<VideoAsset> video; juce::File source, dest; };

    CollectJob (std::vector<Item> i, std::function<void (std::vector<Item>, juce::String)> d)
        : juce::ThreadWithProgressWindow ("Raccolta dei media...", true, true), items (std::move (i)), done (std::move (d)) {}

    void run() override
    {
        for (size_t k = 0; k < items.size() && ! threadShouldExit(); ++k)
        {
            setProgress ((double) k / (double) items.size());
            setStatusMessage (items[k].source.getFileName());
            if (! items[k].source.copyFileTo (items[k].dest)) { error = "Impossibile copiare " + items[k].source.getFileName(); return; }
        }
        ok = ! threadShouldExit();
    }
    void threadComplete (bool) override { if (done) done (ok ? items : std::vector<Item>(), ok ? juce::String() : error); }

private:
    std::vector<Item> items;
    std::function<void (std::vector<Item>, juce::String)> done;
    bool ok = false;
    juce::String error;
};

void MainComponent::collectMedia()
{
    if (project->getFile() == juce::File())
    {
        showError ("Salva prima la sessione: i media vengono raccolti in una cartella accanto al file di sessione.");
        return;
    }
    const auto folder = project->getFile().getParentDirectory().getChildFile (project->getFile().getFileNameWithoutExtension() + "_media");
    folder.createDirectory();

    std::vector<CollectJob::Item> items;
    std::set<juce::String> seenPaths, usedNames;
    auto plan = [&] (const juce::File& src, std::shared_ptr<Asset> a, std::shared_ptr<VideoAsset> v)
    {
        if (src.isAChildOf (folder) || ! seenPaths.insert (src.getFullPathName()).second) return;
        auto name = src.getFileName();
        for (int n = 2; usedNames.count (name) > 0 || folder.getChildFile (name).existsAsFile(); ++n)
            name = src.getFileNameWithoutExtension() + "_" + juce::String (n) + src.getFileExtension();
        usedNames.insert (name);
        items.push_back ({ std::move (a), std::move (v), src, folder.getChildFile (name) });
    };
    std::map<const Asset*, juce::File> done;
    for (auto& t : project->getSession().tracks)
        for (auto& c : t.clips)
            if (c.asset != nullptr && done.emplace (c.asset.get(), c.asset->file).second) plan (c.asset->file, c.asset, nullptr);
    if (auto v = project->getSession().video) plan (v->file, nullptr, v);

    if (items.empty()) { showError ("Tutti i media si trovano gia' nella cartella del progetto."); return; }

    // i clip che condividono un file ne condividono anche l'oggetto Asset: basta ricollegare quello
    juce::Component::SafePointer<MainComponent> safe (this);
    auto job = std::make_shared<std::unique_ptr<CollectJob>>();
    *job = std::make_unique<CollectJob> (std::move (items), [safe, job] (std::vector<CollectJob::Item> copied, juce::String err)
    {
        juce::MessageManager::callAsync ([safe, job, copied, err]
        {
            if (safe != nullptr)
            {
                if (err.isNotEmpty()) safe->showError (err);
                else
                {
                    for (auto& it : copied)
                    {
                        if (it.asset != nullptr) it.asset->file = it.dest;
                        if (it.video != nullptr) it.video->file = it.dest;
                    }
                    safe->project->markDirty();
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Media raccolti",
                                                            juce::String ((int) copied.size()) + " file copiati nella cartella del progetto.\nSalva la sessione per mantenere i nuovi collegamenti.");
                }
            }
            job->reset();
        });
    });
    (*job)->launchThread();
}

void MainComponent::requestQuit()
{
    confirmDiscard ([] { juce::JUCEApplication::quit(); });
}

void MainComponent::importDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Importa audio", juce::File(), formats.getWildcardForAllFormats());
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe] (const juce::FileChooser& fc)
    {
        auto results = fc.getResults();
        if (safe == nullptr || results.isEmpty()) return;
        const bool wasEmpty = safe->project->contentEnd() == 0;
        safe->project->importFiles (results, safe->project->selectedTrackId, safe->engine.getPosition(),
                                    [safe, wasEmpty] (juce::StringArray errs)
        {
            if (safe == nullptr) return;
            if (wasEmpty) safe->timeline->zoomToFit();
            if (errs.size() > 0) safe->showError (errs.joinIntoString ("\n"));
        });
    });
}

void MainComponent::importFromCommandLine (const juce::StringArray& paths)
{
    juce::Array<juce::File> files;
    for (auto& p : paths)
    {
        juce::File f (p);
        if (f.existsAsFile())
        {
            if (f.hasFileExtension ("arostudio")) { project->loadSession (f, [] (juce::String) {}); return; }
            if (video::isVideoFile (f)) { loadVideo (f); continue; }
            files.add (f);
        }
    }
    if (! files.isEmpty())
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        project->importFiles (files, project->selectedTrackId, 0, [safe] (juce::StringArray)
        {
            if (safe != nullptr) safe->timeline->zoomToFit();
        });
    }
}

void MainComponent::loadVideo (const juce::File& f)
{
    // macOS puo' consegnare lo stesso file due volte (argomento + evento "apri file")
    if (loadingVideo == f) return;
    if (auto v = project->getSession().video; v != nullptr && v->file == f) return;
    loadingVideo = f;

    juce::Component::SafePointer<MainComponent> safe (this);
    project->setVideo (f, [safe] (juce::String msg)
    {
        if (safe == nullptr) return;
        safe->loadingVideo = juce::File();
        if (msg.isNotEmpty()) safe->showError (msg);
        safe->timeline->zoomToFit();
    });
}

void MainComponent::importVideoDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Importa video", juce::File(), "*.mov;*.mp4;*.m4v");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe != nullptr && f.existsAsFile()) safe->loadVideo (f);
    });
}

void MainComponent::exportVideoDialog()
{
    auto video = project->getSession().video;
    if (video == nullptr) { showError ("Importa prima un video."); return; }
    if (videoJob != nullptr || exportJob != nullptr) return;

    // con un intervallo scelto sul righello si chiede se esportare solo quello
    const bool hasSel = project->exportOut > project->exportIn && project->exportIn >= 0;
    juce::Component::SafePointer<MainComponent> safe (this);
    auto choose = [safe, video] (bool useRange)
    {
        if (safe == nullptr) return;
        const auto def = video->file.getParentDirectory().getChildFile (video->file.getFileNameWithoutExtension() + (useRange ? "_intervallo" : "_mix")).withFileExtension ("mov");
        safe->chooser = std::make_unique<juce::FileChooser> ("Esporta video con mix", def, "*.mov");
        safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                    [safe, video, useRange] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        f = f.withFileExtension ("mov");
        if (f == video->file) { safe->showError ("Scegli un nome diverso da quello del video originale."); return; }

        const int64_t from = useRange ? safe->project->exportIn : 0, to = useRange ? safe->project->exportOut : -1;
        safe->videoJob = std::make_unique<VideoExportJob> (safe->project->getSession().cloneDetached(), safe->project->getSampleRate(),
                                                           video->file, video->offsetSec, f, from, to, [safe] (bool ok, juce::String msg)
        {
            juce::MessageManager::callAsync ([safe, ok, msg]
            {
                if (safe == nullptr) return;
                safe->videoJob.reset();
                juce::AlertWindow::showMessageBoxAsync (ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                        "Esportazione video", msg);
            });
        });
        safe->videoJob->launchThread();
    });
    };

    if (! hasSel) { choose (false); return; }
    const double rate = project->getSampleRate();
    auto opts = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, "Esporta video con mix",
        "E' stato scelto un intervallo sul righello (" + project->formatTc ((double) project->exportIn / rate) + " - " + project->formatTc ((double) project->exportOut / rate)
            + ").\n\nVuoi esportare solo quello?", "Solo l'intervallo", "Tutto il video", "Annulla");
    juce::NativeMessageBox::showAsync (opts, [choose] (int r)
    {
        // NativeMessageBox restituisce l'indice del pulsante (da 0): 0 = intervallo, 1 = tutto, 2 = annulla
        if (r == 0) choose (true); else if (r == 1) choose (false);
    });
}

void MainComponent::exportDialog()
{
    if (project->contentEnd() <= 0) { showError ("La sessione e' vuota: importa prima dell'audio."); return; }
    if (exportJob != nullptr || videoJob != nullptr) return;

    const bool hasLoop = engine.getLoopEnd() > engine.getLoopStart();
    juce::Component::SafePointer<MainComponent> safe (this);
    const bool hasSel = project->exportOut > project->exportIn && project->exportIn >= 0;
    const double rate = project->getSampleRate();
    const auto selLabel = hasSel ? project->formatTc ((double) project->exportIn / rate) + " - " + project->formatTc ((double) project->exportOut / rate) : juce::String();
    auto* dlg = new ExportDialog (hasLoop, engine.getLoopStart(), engine.getLoopEnd(), hasSel, project->exportIn, project->exportOut, selLabel,
                                  [safe] (const ExportOptions& o) { if (safe != nullptr) safe->runExport (o); });
    dlg->setSize (520, 600);

    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (dlg);
    lo.dialogTitle = "Esporta audio";
    lo.dialogBackgroundColour = panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    lo.launchAsync();
}

void MainComponent::runExport (const ExportOptions& o)
{
    const auto base = project->getFile() == juce::File() ? juce::String ("Mixdown") : project->getFile().getFileNameWithoutExtension();
    juce::Component::SafePointer<MainComponent> safe (this);

    auto start = [safe, o, base] (const juce::File& target)
    {
        if (safe == nullptr) return;
        safe->exportJob = std::make_unique<ExportJob> (safe->project->getSession().cloneDetached(), safe->project->getSampleRate(), o, target, base,
                                                       [safe] (ExportResult r)
        {
            juce::MessageManager::callAsync ([safe, r]
            {
                if (safe == nullptr) return;
                safe->exportJob.reset();
                if (! r.ok)
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Esportazione", r.message);
                    return;
                }
                auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::InfoIcon, "Esportazione completata", r.message,
                                                                         "Mostra nel Finder", "Chiudi");
                juce::NativeMessageBox::showAsync (opts, [r] (int res)
                {
                    if (res == 0 && r.files.size() > 0) juce::File (r.files[0]).revealToUser();   // 0 = primo pulsante
                });
            });
        });
        safe->exportJob->launchThread();
    };

    if (o.stems)
    {
        chooser = std::make_unique<juce::FileChooser> ("Cartella per gli stem", juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [start, base] (const juce::FileChooser& fc)
        {
            auto dir = fc.getResult();
            if (dir != juce::File()) start (dir.getChildFile (base + "_stems"));
        });
        return;
    }

    auto def = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (base).withFileExtension (o.format);
    chooser = std::make_unique<juce::FileChooser> ("Esporta audio", def, "*." + o.format);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [start, o] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        start (f.withFileExtension (o.format));
    });
}

void MainComponent::saveAs()
{
    chooser = std::make_unique<juce::FileChooser> ("Salva sessione", project->getFile(), "*.arostudio");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        juce::String err;
        if (! safe->project->save (f.withFileExtension ("arostudio"), err)) safe->showError (err);
        else safe->addRecent (f.withFileExtension ("arostudio"));
    });
}

void MainComponent::saveCurrent()
{
    if (project->getFile() == juce::File()) { saveAs(); return; }
    juce::String err;
    if (! project->save (project->getFile(), err)) showError (err);
}

void MainComponent::openDialog()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    confirmDiscard ([safe]
    {
        if (safe == nullptr) return;
        safe->chooser = std::make_unique<juce::FileChooser> ("Apri sessione", juce::File(), "*.arostudio");
        safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                    [safe] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (safe == nullptr || ! f.existsAsFile()) return;
            safe->engine.stop();
            safe->engine.setPosition (0);
            safe->project->loadSession (f, [safe, f] (juce::String errs)
            {
                if (safe != nullptr) safe->addRecent (f);
                if (safe != nullptr) safe->timeline->zoomToFit();
                if (safe != nullptr && errs.isNotEmpty()) safe->showError (errs);
            });
        });
    });
}

void MainComponent::audioSettings()
{
    auto* sel = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 8, 0, 2, false, false, true, false);
    sel->setSize (520, 440);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (sel);
    o.dialogTitle = "Impostazioni audio";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& c)
{
    c.addArray ({ cmdNew, cmdOpen, cmdSave, cmdSaveAs, cmdImport, cmdExport, cmdAudioSettings,
                  cmdUndo, cmdRedo, cmdSelectAll, cmdDelete, cmdDuplicate, cmdSplit,
                  cmdAddTrack, cmdRemoveTrack, cmdRenameTrack,
                  cmdPlay, cmdHome, cmdEnd, cmdLoop, cmdLoopFromSel, cmdNudgeL, cmdNudgeR, cmdNudgeLBig, cmdNudgeRBig,
                  cmdSnap, cmdFollow, cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdMixer, cmdTaller, cmdShorter,
                  cmdRecord, cmdArm, cmdMonitor, cmdImportVideo, cmdRemoveVideo, cmdExportVideo, cmdShowVideo,
                  cmdDetachVideo, cmdFitTracks, cmdVideoOffsetMinus, cmdVideoOffsetPlus,
                  cmdToolSelect, cmdToolRazor, cmdToolSlip, cmdToolHand, cmdToolZoom,
                  cmdRippleDelete, cmdTrimStart, cmdTrimEnd, cmdTrackUp, cmdTrackDown, cmdDuplicateTrack,
                  cmdAddPlugin, cmdAddMasterPlugin, cmdPdc,
                  cmdAddMarker, cmdPrevMarker, cmdNextMarker, cmdExportMarkers, cmdToolAuto, cmdClearAutomation,
                  cmdShortcuts, cmdCollectMedia, cmdClearRecent, cmdSessionSettings, cmdRulerTc,
                  cmdAdrWindow, cmdAdrImport, cmdAdrAddCue, cmdAdrRecord, cmdAdrNextCue, cmdAdrPrevCue, cmdAdrExportCsv, cmdAdrExportPT, cmdAdrExportBwf, cmdTimeStretch, cmdToolStretch });
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& i)
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    const auto cmd = ModifierKeys::commandModifier, shift = ModifierKeys::shiftModifier;
    const bool hasSel = ! project->selection.empty();

    switch (id)
    {
        case cmdNew:           i.setInfo ("Nuova sessione", "", "File", 0); i.addDefaultKeypress ('n', cmd); break;
        case cmdOpen:          i.setInfo ("Apri...", "", "File", 0); i.addDefaultKeypress ('o', cmd); break;
        case cmdSave:          i.setInfo ("Salva", "", "File", 0); i.addDefaultKeypress ('s', cmd); break;
        case cmdSaveAs:        i.setInfo ("Salva con nome...", "", "File", 0); i.addDefaultKeypress ('s', cmd | shift); break;
        case cmdImport:        i.setInfo ("Importa audio...", "", "File", 0); i.addDefaultKeypress ('i', cmd); break;
        case cmdExport:        i.setInfo ("Esporta audio...", "", "File", 0); i.addDefaultKeypress ('e', cmd); break;
        case cmdAudioSettings: i.setInfo ("Impostazioni audio...", "", "File", 0); i.addDefaultKeypress (',', cmd); break;

        case cmdUndo:          i.setInfo ("Annulla", "", "Modifica", 0); i.addDefaultKeypress ('z', cmd); i.setActive (project->canUndo()); break;
        case cmdRedo:          i.setInfo ("Ripeti", "", "Modifica", 0); i.addDefaultKeypress ('z', cmd | shift); i.setActive (project->canRedo()); break;
        case cmdSelectAll:     i.setInfo ("Seleziona tutto", "", "Modifica", 0); i.addDefaultKeypress ('a', cmd); break;
        case cmdDuplicate:     i.setInfo ("Duplica", "", "Modifica", 0); i.addDefaultKeypress ('d', cmd); i.setActive (hasSel); break;
        case cmdDelete:        i.setInfo ("Elimina", "", "Modifica", 0);
                               i.addDefaultKeypress (KeyPress::deleteKey, 0); i.addDefaultKeypress (KeyPress::backspaceKey, 0);
                               i.setActive (hasSel); break;
        case cmdSplit:         i.setInfo ("Dividi alla testina", "", "Modifica", 0); i.addDefaultKeypress ('s', 0); break;

        case cmdAddTrack:      i.setInfo ("Nuova traccia", "", "Traccia", 0); i.addDefaultKeypress ('t', cmd | shift); break;
        case cmdRemoveTrack:   i.setInfo ("Elimina traccia selezionata", "", "Traccia", 0); break;
        case cmdRenameTrack:   i.setInfo ("Rinomina traccia selezionata...", "", "Traccia", 0); break;

        case cmdPlay:          i.setInfo (engine.isPlaying() ? "Stop" : "Play", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::spaceKey, 0); break;
        case cmdHome:          i.setInfo ("Vai all'inizio", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::homeKey, 0); break;
        case cmdEnd:           i.setInfo ("Vai alla fine", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::endKey, 0); break;
        case cmdLoop:          i.setInfo ("Loop", "", "Trasporto", 0); i.addDefaultKeypress ('l', 0); i.setTicked (engine.isLooping()); break;
        case cmdLoopFromSel:   i.setInfo ("Loop dalla selezione", "", "Trasporto", 0); i.addDefaultKeypress ('l', shift); break;
        case cmdNudgeL:        i.setInfo ("Indietro di un fotogramma", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::leftKey, 0); break;
        case cmdNudgeR:        i.setInfo ("Avanti di un fotogramma", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::rightKey, 0); break;
        case cmdNudgeLBig:     i.setInfo ("Indietro di un secondo", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::leftKey, shift); break;
        case cmdNudgeRBig:     i.setInfo ("Avanti di un secondo", "", "Trasporto", 0); i.addDefaultKeypress (KeyPress::rightKey, shift); break;

        case cmdSnap:          i.setInfo ("Aggancio (snap)", "", "Vista", 0); i.addDefaultKeypress ('n', 0); i.setTicked (timeline->snapEnabled); break;
        case cmdFollow:        i.setInfo ("Segui la testina", "", "Vista", 0); i.addDefaultKeypress ('f', 0); i.setTicked (timeline->followEnabled); break;
        case cmdZoomIn:        i.setInfo ("Zoom avanti", "", "Vista", 0); i.addDefaultKeypress ('=', 0); i.addDefaultKeypress ('+', 0); break;
        case cmdZoomOut:       i.setInfo ("Zoom indietro", "", "Vista", 0); i.addDefaultKeypress ('-', 0); break;
        case cmdZoomFit:       i.setInfo ("Adatta allo schermo", "", "Vista", 0); i.addDefaultKeypress ('z', shift); break;
        case cmdMixer:         i.setInfo ("Mostra mixer", "", "Vista", 0); i.addDefaultKeypress ('m', shift); i.setTicked (mixerVisible); break;
        case cmdRecord:        i.setInfo (engine.isRecording() ? "Ferma registrazione" : "Registra", "", "Trasporto", 0); i.addDefaultKeypress ('r', 0); i.setTicked (engine.isRecording()); break;
        case cmdArm:           i.setInfo ("Arma/disarma traccia selezionata", "", "Traccia", 0); i.addDefaultKeypress ('a', 0); break;
        case cmdMonitor:       i.setInfo ("Ascolta l'ingresso durante la registrazione", "", "Trasporto", 0); i.addDefaultKeypress ('i', 0); i.setTicked (engine.monitorInput.load()); break;
        case cmdImportVideo:   i.setInfo ("Importa video...", "", "File", 0); i.addDefaultKeypress ('i', cmd | shift); break;
        case cmdRemoveVideo:   i.setInfo ("Rimuovi video", "", "File", 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdExportVideo:   i.setInfo ("Esporta video con mix...", "", "File", 0); i.addDefaultKeypress ('e', cmd | shift); i.setActive (project->getSession().video != nullptr); break;
        case cmdShowVideo:     i.setInfo ("Mostra video", "", "Vista", 0); i.addDefaultKeypress ('v', shift); i.setTicked (viewerVisible); i.setActive (project->getSession().video != nullptr); break;
        case cmdToolSelect:    i.setInfo ("Selezione", "", "Strumenti", 0); i.addDefaultKeypress ('v', 0); i.setTicked (timeline->getTool() == Tool::Select); break;
        case cmdToolRazor:     i.setInfo ("Lama", "", "Strumenti", 0); i.addDefaultKeypress ('b', 0); i.setTicked (timeline->getTool() == Tool::Razor); break;
        case cmdToolStretch:   i.setInfo ("Stretch", "", "Strumenti", 0); i.addDefaultKeypress ('e', 0); i.setTicked (timeline->getTool() == Tool::Stretch); break;
        case cmdToolSlip:      i.setInfo ("Slip", "", "Strumenti", 0); i.addDefaultKeypress ('y', 0); i.setTicked (timeline->getTool() == Tool::Slip); break;
        case cmdToolHand:      i.setInfo ("Mano", "", "Strumenti", 0); i.addDefaultKeypress ('h', 0); i.setTicked (timeline->getTool() == Tool::Hand); break;
        case cmdToolZoom:      i.setInfo ("Zoom", "", "Strumenti", 0); i.addDefaultKeypress ('z', 0); i.setTicked (timeline->getTool() == Tool::Zoom); break;
        case cmdTimeStretch:   i.setInfo ("Time-stretch dei clip selezionati...", "", "Modifica", 0); i.addDefaultKeypress ('t', cmd | juce::ModifierKeys::altModifier); i.setActive (hasSel); break;
        case cmdAdrWindow:     i.setInfo ("Finestra ADR", "", "ADR", 0); i.addDefaultKeypress ('a', cmd | shift); i.setTicked (adrWindow != nullptr); break;
        case cmdAdrImport:     i.setInfo ("Importa cue da Pro Tools / EDL / tabella...", "", "ADR", 0); break;
        case cmdAdrAddCue:     i.setInfo ("Nuovo cue dalla regione di loop o dalla testina", "", "ADR", 0); break;
        case cmdAdrRecord:     i.setInfo (adrRecCue != 0 ? "Ferma la registrazione del cue" : "Registra il cue selezionato", "", "ADR", 0);
                               i.addDefaultKeypress ('r', cmd | shift); i.setActive (adrRecCue != 0 || project->findCue (project->selectedCueId) != nullptr); break;
        case cmdAdrNextCue:    i.setInfo ("Cue successivo", "", "ADR", 0); i.addDefaultKeypress (juce::KeyPress::rightKey, cmd | shift); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrPrevCue:    i.setInfo ("Cue precedente", "", "ADR", 0); i.addDefaultKeypress (juce::KeyPress::leftKey, cmd | shift); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrExportCsv:  i.setInfo ("Esporta cue sheet (CSV)...", "", "ADR", 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrExportPT:   i.setInfo ("Esporta cue come marker Pro Tools (testo)...", "", "ADR", 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrExportBwf:  i.setInfo ("Esporta take attivi (BWF con timecode)...", "", "ADR", 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdSessionSettings: i.setInfo ("Impostazioni sessione: timecode e ADR...", "", "File", 0); break;
        case cmdRulerTc:       i.setInfo ("Righello in timecode", "", "Vista", 0); i.setTicked (project->rulerTimecode); break;
        case cmdShortcuts:     i.setInfo ("Guida rapida e scorciatoie...", "", "Aiuto", 0); i.addDefaultKeypress ('?', 0); break;
        case cmdCollectMedia:  i.setInfo ("Raccogli i media nella cartella del progetto...", "", "File", 0); break;
        case cmdClearRecent:   i.setInfo ("Svuota l'elenco dei file recenti", "", "File", 0); break;
        case cmdToolAuto:      i.setInfo ("Automazione", "", "Strumenti", 0); i.addDefaultKeypress ('u', 0); i.setTicked (timeline->getTool() == Tool::Automation); break;
        case cmdClearAutomation: i.setInfo ("Cancella l'automazione mostrata (volume, pan o send) della traccia selezionata", "", "Traccia", 0); break;
        case cmdRippleDelete:  i.setInfo ("Elimina con ripple", "", "Modifica", 0);
                               i.addDefaultKeypress (juce::KeyPress::deleteKey, shift); i.addDefaultKeypress (juce::KeyPress::backspaceKey, shift);
                               i.setActive (hasSel); break;
        case cmdTrimStart:     i.setInfo ("Taglia l'inizio alla testina", "", "Modifica", 0); i.addDefaultKeypress ('q', 0); break;
        case cmdTrimEnd:       i.setInfo ("Taglia la fine alla testina", "", "Modifica", 0); i.addDefaultKeypress ('w', 0); break;
        case cmdTrackUp:       i.setInfo ("Sposta la traccia su", "", "Traccia", 0); i.addDefaultKeypress (juce::KeyPress::upKey, cmd | juce::ModifierKeys::altModifier); break;
        case cmdTrackDown:     i.setInfo (juce::String (juce::CharPointer_UTF8 ("Sposta la traccia gi\xc3\xb9")), "", "Traccia", 0); i.addDefaultKeypress (juce::KeyPress::downKey, cmd | juce::ModifierKeys::altModifier); break;
        case cmdDuplicateTrack: i.setInfo ("Duplica la traccia", "", "Traccia", 0); i.addDefaultKeypress ('d', cmd | shift); break;
        case cmdAddPlugin:     i.setInfo ("Aggiungi plug-in VST3 alla traccia selezionata...", "", "Traccia", 0); i.addDefaultKeypress ('p', 0); break;
        case cmdAddMarker:     i.setInfo ("Aggiungi marker alla testina", "", "Marker", 0); i.addDefaultKeypress ('m', 0); break;
        case cmdPrevMarker:    i.setInfo ("Marker precedente", "", "Marker", 0); i.addDefaultKeypress (juce::KeyPress::leftKey, juce::ModifierKeys::altModifier); break;
        case cmdNextMarker:    i.setInfo ("Marker successivo", "", "Marker", 0); i.addDefaultKeypress (juce::KeyPress::rightKey, juce::ModifierKeys::altModifier); break;
        case cmdExportMarkers: i.setInfo ("Esporta elenco marker (CSV)...", "", "Marker", 0); i.setActive (! project->getSession().markers.empty()); break;
        case cmdPdc:           i.setInfo ("Compensazione latenza plug-in", "", "Trasporto", 0); i.setTicked (engine.pdcOn.load()); break;
        case cmdAddMasterPlugin: i.setInfo ("Aggiungi plug-in VST3 al master...", "", "Traccia", 0); break;
        case cmdDetachVideo:   i.setInfo (videoWindow != nullptr ? "Aggancia la finestra video" : "Stacca la finestra video", "", "Vista", 0);
                               i.addDefaultKeypress ('d', shift); i.setActive (project->getSession().video != nullptr); break;
        case cmdFitTracks:     i.setInfo ("Adatta l'altezza delle tracce", "", "Vista", 0); i.addDefaultKeypress ('f', shift); break;
        case cmdVideoOffsetMinus: i.setInfo ("Anticipa il video di 1 ms", "", "Vista", 0); i.addDefaultKeypress (',', 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdVideoOffsetPlus:  i.setInfo ("Ritarda il video di 1 ms", "", "Vista", 0); i.addDefaultKeypress ('.', 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdTaller:        i.setInfo ("Tracce piu' alte", "", "Vista", 0); i.addDefaultKeypress (KeyPress::upKey, cmd); break;
        case cmdShorter:       i.setInfo ("Tracce piu' basse", "", "Vista", 0); i.addDefaultKeypress (KeyPress::downKey, cmd); break;
        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    const double rate = project->getSampleRate();
    switch (info.commandID)
    {
        case cmdNew:
        {
            juce::Component::SafePointer<MainComponent> safe (this);
            confirmDiscard ([safe] { if (safe != nullptr) { safe->engine.stop(); safe->engine.setPosition (0); safe->project->newSession(); safe->pendingFit = true; safe->layoutAll(); } });
            break;
        }
        case cmdOpen:          openDialog(); break;
        case cmdSave:          saveCurrent(); break;
        case cmdSaveAs:        saveAs(); break;
        case cmdImport:        importDialog(); break;
        case cmdExport:        exportDialog(); break;
        case cmdAudioSettings: audioSettings(); break;

        case cmdUndo:          project->undo(); break;
        case cmdRedo:          project->redo(); break;
        case cmdSelectAll:     project->selectAll(); break;
        case cmdDuplicate:     project->duplicateSelected(); break;
        case cmdDelete:        project->deleteSelected(); break;
        case cmdSplit:         project->splitAt (engine.getPosition()); break;

        case cmdAddTrack:      project->addTrack(); break;
        case cmdRemoveTrack:   project->removeTrack (project->selectedTrackId); break;
        case cmdRenameTrack:   timeline->renameTrackDialog (project->selectedTrackId); break;

        case cmdPlay:          toggleTransport(); break;
        case cmdHome:          engine.setPosition (0); break;
        case cmdEnd:           engine.setPosition (project->contentEnd()); break;
        case cmdLoop:          toggleLoop(); break;
        case cmdLoopFromSel:   setLoopFromSelection(); break;
        case cmdNudgeL:        engine.setPosition (engine.getPosition() - (int64_t) (rate / project->frameRate())); break;
        case cmdNudgeR:        engine.setPosition (engine.getPosition() + (int64_t) (rate / project->frameRate())); break;
        case cmdRecord:        if (engine.isRecording()) finishRecording(); else startRecording(); break;
        case cmdArm:           armSelectedTrack(); break;
        case cmdMonitor:       engine.monitorInput = ! engine.monitorInput.load(); monBtn.setToggleState (engine.monitorInput.load(), juce::dontSendNotification); break;
        case cmdImportVideo:   importVideoDialog(); break;
        case cmdRemoveVideo:   project->clearVideo(); break;
        case cmdExportVideo:   exportVideoDialog(); break;
        case cmdToolSelect:    timeline->setTool (Tool::Select); break;
        case cmdToolRazor:     timeline->setTool (Tool::Razor); break;
        case cmdToolSlip:      timeline->setTool (Tool::Slip); break;
        case cmdToolStretch:   timeline->setTool (Tool::Stretch); break;
        case cmdToolHand:      timeline->setTool (Tool::Hand); break;
        case cmdToolZoom:      timeline->setTool (Tool::Zoom); break;
        case cmdSessionSettings: showSessionSettings(); break;
        case cmdTimeStretch:   timeStretchSelected (false); break;
        case cmdAdrWindow:     if (adrWindow != nullptr) { adrWindow.reset(); adrPanel = nullptr; } else showAdrWindow(); break;
        case cmdAdrImport:     importAdrDialog(); break;
        case cmdAdrAddCue:     addAdrCue(); break;
        case cmdAdrRecord:     if (adrRecCue != 0) finishRecording(); else recordAdrCue (project->selectedCueId); break;
        case cmdAdrNextCue: case cmdAdrPrevCue:
        {
            const auto& cues = project->getSession().cues;
            if (cues.empty()) break;
            int idx = -1;
            for (size_t k = 0; k < cues.size(); ++k) if (cues[k].id == project->selectedCueId) idx = (int) k;
            idx = juce::jlimit (0, (int) cues.size() - 1, idx + (info.commandID == cmdAdrNextCue ? 1 : -1));
            goToAdrCue (cues[(size_t) idx].id);
            break;
        }
        case cmdAdrExportCsv:  exportAdrCsv(); break;
        case cmdAdrExportPT:   exportAdrProTools(); break;
        case cmdAdrExportBwf:  exportAdrBwf(); break;
        case cmdRulerTc:       project->rulerTimecode = ! project->rulerTimecode; project->markDirty(); timeline->repaint(); break;
        case cmdShortcuts:     showShortcuts(); break;
        case cmdCollectMedia:  collectMedia(); break;
        case cmdClearRecent:   settings().setValue ("recent", ""); settings().saveIfNeeded(); break;
        case cmdToolAuto:      timeline->setTool (Tool::Automation); break;
        case cmdClearAutomation: project->clearAutomation (project->selectedTrackId); break;
        case cmdRippleDelete:  project->rippleDeleteSelected(); break;
        case cmdTrimStart:     project->trimToPlayhead (true, engine.getPosition()); break;
        case cmdTrimEnd:       project->trimToPlayhead (false, engine.getPosition()); break;
        case cmdTrackUp:       { const int i = project->trackIndexForId (project->selectedTrackId); if (i > 0) project->moveTrackTo (i, i - 1); break; }
        case cmdTrackDown:     { const int i = project->trackIndexForId (project->selectedTrackId); if (i >= 0) project->moveTrackTo (i, i + 1); break; }
        case cmdDuplicateTrack: project->duplicateTrack (project->selectedTrackId); break;
        case cmdAddPlugin:     if (auto* t = project->findTrack (project->selectedTrackId)) pluginHost->pickAndAdd (t->live, this); break;
        case cmdAddMarker:     project->addMarker (engine.getPosition()); break;
        case cmdPrevMarker:    { const auto p = project->previousMarkerPos (engine.getPosition() - (int64_t) (project->getSampleRate() * 0.05)); if (p >= 0) engine.setPosition (p); break; }
        case cmdNextMarker:    { const auto p = project->nextMarkerPos (engine.getPosition()); if (p >= 0) engine.setPosition (p); break; }
        case cmdExportMarkers: exportMarkersCsv(); break;
        case cmdPdc:           engine.pdcOn = ! engine.pdcOn.load(); break;
        case cmdAddMasterPlugin: pluginHost->pickAndAdd (project->getSession().master, this); break;
        case cmdDetachVideo:   setViewerDetached (videoWindow == nullptr); break;
        case cmdFitTracks:     timeline->fitTrackHeights(); break;
        case cmdVideoOffsetMinus: project->setVideoOffset (project->getSession().video ? project->getSession().video->offsetSec - 0.001 : 0.0); viewer.refreshOffset(); timeline->repaint(); break;
        case cmdVideoOffsetPlus:  project->setVideoOffset (project->getSession().video ? project->getSession().video->offsetSec + 0.001 : 0.0); viewer.refreshOffset(); timeline->repaint(); break;
        case cmdShowVideo:     viewerVisible = ! viewerVisible; videoBtn.setToggleState (viewerVisible, juce::dontSendNotification); layoutAll(); break;
        case cmdNudgeLBig:     engine.setPosition (engine.getPosition() - (int64_t) rate); break;
        case cmdNudgeRBig:     engine.setPosition (engine.getPosition() + (int64_t) rate); break;

        case cmdSnap:          timeline->snapEnabled = ! timeline->snapEnabled; snapBtn.setToggleState (timeline->snapEnabled, juce::dontSendNotification); break;
        case cmdFollow:        timeline->followEnabled = ! timeline->followEnabled; followBtn.setToggleState (timeline->followEnabled, juce::dontSendNotification); break;
        case cmdZoomIn:        timeline->zoomBy (1.5); break;
        case cmdZoomOut:       timeline->zoomBy (1.0 / 1.5); break;
        case cmdZoomFit:       timeline->zoomToFit(); break;
        case cmdMixer:         mixerVisible = ! mixerVisible; mixerBtn.setToggleState (mixerVisible, juce::dontSendNotification); mixer->setVisible (mixerVisible); resized(); break;
        case cmdTaller:        timeline->setAllTrackHeights (timeline->getTrackHeight() + 16); break;
        case cmdShorter:       timeline->setAllTrackHeights (timeline->getTrackHeight() - 16); break;
        default: return false;
    }
    commandManager.commandStatusChanged();
    return true;
}

void MainComponent::menuItemSelected (int id, int)
{
    if (id >= 2000 && id < 2100)
    {
        const auto list = juce::StringArray::fromLines (settings().getValue ("recent", ""));
        if (id - 2000 < list.size()) openFile (juce::File (list[id - 2000]));
        return;
    }
    if (id >= 1000)
    {
        const auto& marks = project->getSession().markers;
        if ((size_t) (id - 1000) < marks.size()) engine.setPosition (marks[(size_t) (id - 1000)].pos);
    }
}

void MainComponent::exportMarkersCsv()
{
    const auto base = project->getFile() == juce::File() ? juce::String ("Marker") : project->getFile().getFileNameWithoutExtension() + "_marker";
    chooser = std::make_unique<juce::FileChooser> ("Esporta elenco marker", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (base).withFileExtension ("csv"), "*.csv");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        const double rate = safe->project->getSampleRate();
        juce::String csv = "Nome;Timecode;Secondi\n";
        for (auto& m : safe->project->getSession().markers)
            csv << m.name.replace (";", ",") << ";" << safe->project->formatTc ((double) m.pos / rate) << ";" << juce::String ((double) m.pos / rate, 3) << "\n";
        if (! f.withFileExtension ("csv").replaceWithText (csv)) safe->showError ("Impossibile scrivere il file.");
    });
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Modifica", "Traccia", "Strumenti", "Marker", "ADR", "Trasporto", "Vista", "Aiuto" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu m;
    auto add = [&] (std::initializer_list<juce::CommandID> ids)
    {
        for (auto id : ids)
        {
            if (id == 0) m.addSeparator(); else m.addCommandItem (&commandManager, id);
        }
    };
    switch (index)
    {
        case 0:
        {
            add ({ cmdNew, cmdOpen });
            juce::PopupMenu recent;
            const auto list = juce::StringArray::fromLines (settings().getValue ("recent", ""));
            for (int i = 0; i < list.size(); ++i)
                if (list[i].isNotEmpty()) recent.addItem (2000 + i, juce::File (list[i]).getFileNameWithoutExtension() + "   -   " + juce::File (list[i]).getParentDirectory().getFileName());
            if (recent.getNumItems() > 0) { recent.addSeparator(); recent.addCommandItem (&commandManager, cmdClearRecent); }
            m.addSubMenu ("Apri recente", recent, recent.getNumItems() > 0);
            m.addSeparator();
            add ({ cmdSave, cmdSaveAs, cmdCollectMedia, cmdSessionSettings, 0, cmdImport, cmdImportVideo, cmdRemoveVideo, 0, cmdExport, cmdExportVideo, 0, cmdAudioSettings });
            break;
        }
        case 99: add ({ cmdNew, cmdOpen, 0, cmdSave, cmdSaveAs, 0, cmdImport, cmdImportVideo, cmdRemoveVideo, 0, cmdExport, cmdExportVideo, 0, cmdAudioSettings }); break;
        case 1: add ({ cmdUndo, cmdRedo, 0, cmdSelectAll, cmdSplit, cmdTrimStart, cmdTrimEnd, cmdTimeStretch, 0, cmdDuplicate, cmdDelete, cmdRippleDelete }); break;
        case 2: add ({ cmdAddTrack, cmdDuplicateTrack, cmdRenameTrack, cmdRemoveTrack, 0, cmdTrackUp, cmdTrackDown, 0, cmdArm, cmdClearAutomation, 0, cmdAddPlugin, cmdAddMasterPlugin }); break;
        case 3: add ({ cmdToolSelect, cmdToolRazor, cmdToolSlip, cmdToolStretch, cmdToolHand, cmdToolZoom, cmdToolAuto }); break;
        case 4:
        {
            add ({ cmdAddMarker, cmdPrevMarker, cmdNextMarker, cmdExportMarkers });
            const auto& marks = project->getSession().markers;
            if (! marks.empty()) m.addSeparator();
            for (size_t i = 0; i < marks.size() && i < 60; ++i)
                m.addItem (1000 + (int) i, marks[i].name + "   " + project->formatTc ((double) marks[i].pos / project->getSampleRate()));
            break;
        }
        case 5: add ({ cmdAdrWindow, 0, cmdAdrImport, cmdAdrAddCue, 0, cmdAdrPrevCue, cmdAdrNextCue, cmdAdrRecord, 0, cmdAdrExportCsv, cmdAdrExportPT, cmdAdrExportBwf, 0, cmdSessionSettings }); break;
        case 6: add ({ cmdPlay, cmdRecord, cmdMonitor, cmdPdc, 0, cmdHome, cmdEnd, 0, cmdLoop, cmdLoopFromSel, 0, cmdNudgeL, cmdNudgeR, cmdNudgeLBig, cmdNudgeRBig }); break;
        case 8: add ({ cmdShortcuts }); break;
        case 7: add ({ cmdSnap, cmdFollow, 0, cmdZoomIn, cmdZoomOut, cmdZoomFit, 0, cmdTaller, cmdShorter, cmdFitTracks, 0, cmdRulerTc, cmdShowVideo, cmdDetachVideo, cmdMixer }); break;
        default: break;
    }
    return m;
}

//==============================================================================
void MainComponent::timeStretchSelected (bool toCue)
{
    std::vector<int64_t> ids (project->selection.begin(), project->selection.end());
    if (ids.empty()) { showError ("Seleziona almeno un clip."); return; }
    const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ADR Dub Studio").getChildFile ("Elaborati");
    juce::Component::SafePointer<MainComponent> safe (this);
    auto report = [safe] (juce::String err) { if (safe != nullptr && err.isNotEmpty()) safe->showError (err); };

    if (toCue)
    {
        // ogni clip di un cue ADR viene portato alla durata del suo cue
        int n = 0;
        for (auto id : ids)
        {
            const auto* c = project->findClip (id);
            const auto* cue = c != nullptr && c->cueId != 0 ? project->findCue (c->cueId) : nullptr;
            if (cue == nullptr || c->length <= 0) continue;
            const double ratio = (double) (cue->outPos - cue->inPos) / (double) c->length;
            if (ratio < 0.25 || ratio > 4.0) { report ("La durata del cue " + juce::String (cue->number) + " e' troppo diversa dal take (oltre 4 volte)."); continue; }
            project->stretchClip (id, ratio, true, folder, report);
            ++n;
        }
        if (n == 0) report ("Seleziona un clip collegato a un cue ADR.");
        return;
    }

    auto* aw = new juce::AlertWindow ("Time-stretch", "Cambia la durata dei clip selezionati. Sopra 100% i clip diventano piu' lunghi (piu' lenti).", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("pct", "100", "Durata finale (% dell'originale, da 25 a 400)");
    aw->addComboBox ("mode", { "Mantieni il tono (voce, dialoghi)", "Cambia anche il tono (come un nastro)" }, "Modo");
    aw->addButton ("Applica", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Annulla", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, ids, folder, report] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        const double pct = aw->getTextEditorContents ("pct").replaceCharacter (',', '.').getDoubleValue();
        if (pct < 25.0 || pct > 400.0) { safe->showError ("Scrivi una percentuale tra 25 e 400."); return; }
        if (std::abs (pct - 100.0) < 0.05) return;
        const bool keepPitch = aw->getComboBoxComponent ("mode")->getSelectedItemIndex() == 0;
        for (auto id : ids) safe->project->stretchClip (id, pct / 100.0, keepPitch, folder, report);
    }), true);
}

//==============================================================================
// ADR
//==============================================================================
void MainComponent::showAdrWindow()
{
    if (adrWindow != nullptr) { adrWindow->toFront (true); return; }

    AdrPanel::Actions a;
    a.importFile     = [this] { importAdrDialog(); };
    a.addCue         = [this] { addAdrCue(); };
    a.exportCsv      = [this] { exportAdrCsv(); };
    a.exportProTools = [this] { exportAdrProTools(); };
    a.exportBwf      = [this] { exportAdrBwf(); };
    a.settings       = [this] { showSessionSettings(); };
    a.stopRecording  = [this] { if (adrRecCue != 0) finishRecording(); };
    a.recordCue      = [this] (int id) { recordAdrCue (id); };
    a.goToCue        = [this] (int id) { goToAdrCue (id); };
    a.isRecording    = [this] { return adrRecCue != 0; };
    a.auditionTake   = [this] (int cue, int take) { auditionTake (cue, take); };
    a.stopAudition   = [this] { endAudition(); };
    a.auditionOriginal = [this] (int cue) { auditionOriginal (cue); };
    a.isAuditioning  = [this] { return engine.getAuditionClip() != 0; };

    juce::Component::SafePointer<MainComponent> safe (this);
    adrWindow = std::make_unique<AdrWindow> ([safe]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->adrPanel = nullptr; safe->adrWindow.reset(); safe->commandManager.commandStatusChanged(); } });
    });
    auto panel = std::make_unique<AdrPanel> (*project, a);
    adrPanel = panel.get();
    adrWindow->setContentOwned (panel.release(), false);
    adrWindow->addKeyListener (commandManager.getKeyMappings());
    adrWindow->setSize (1060, 640);
    adrWindow->centreAroundComponent (this, 1060, 640);
    adrWindow->setVisible (true);
    commandManager.commandStatusChanged();
}

void MainComponent::updateAdrViewer()
{
    VideoViewer::AdrInfo info;
    if (const auto* c = project->findCue (project->selectedCueId))
    {
        const double rate = project->getSampleRate();
        info.active = true;
        info.recording = adrRecCue == c->id;
        info.streamer = project->adrStreamer;
        info.inSec = (double) c->inPos / rate;
        info.outSec = (double) c->outPos / rate;
        info.title = "ADR #" + juce::String (c->number) + "   " + c->character.toUpperCase()
                   + (c->scene.isNotEmpty() ? "   Scena " + c->scene : juce::String())
                   + "   " + project->formatTc (info.inSec) + " - " + project->formatTc (info.outSec)
                   + (info.recording ? "   REGISTRAZIONE" : juce::String());
        info.dialogue = c->dialogue;
        info.dialogueSize = project->adrDialogueSize;
    }
    const auto key = juce::String (info.active ? 1 : 0) + info.title + info.dialogue + juce::String (info.streamer ? 1 : 0) + juce::String (info.dialogueSize) + juce::String (info.inSec) + juce::String (info.outSec);
    if (key == adrViewerKey) return;
    adrViewerKey = key;
    viewer.setAdr (info);
}

void MainComponent::goToAdrCue (int cueId)
{
    const auto* c = project->findCue (cueId);
    if (c == nullptr) return;
    project->selectedCueId = cueId;
    project->sendChangeMessage();
    if (engine.isRecording()) return;
    if (engine.getAuditionClip() != 0) endAudition();
    const double rate = project->getSampleRate();
    const int64_t from = c->inPos - (int64_t) (project->adrPreRoll * rate);
    engine.setPosition (from);
    timeline->showRange (from, c->outPos + (int64_t) (project->adrPostRoll * rate));   // il cue scelto compare nella timeline
    timeline->repaint();
}

void MainComponent::auditionTake (int cueId, int takeNo)
{
    if (engine.isRecording()) return;
    const Clip* found = nullptr;
    for (auto& t : project->getSession().tracks)
        for (auto& c : t.clips)
            if (c.cueId == cueId && c.takeNo == takeNo) found = &c;
    if (found == nullptr) return;

    const double rate = project->getSampleRate();
    const int64_t id = found->id, start = juce::jmax<int64_t> (0, found->start - (int64_t) (0.7 * rate));
    engine.stop();
    engine.setAuditionClip (id);
    auditionStop = found->end() + (int64_t) (0.3 * rate);
    engine.setPosition (start);
    timeline->showRange (found->start, found->end());
    mixer->resetPeaks();
    engine.play();
    if (adrPanel != nullptr) adrPanel->refresh();
}

void MainComponent::auditionOriginal (int cueId)
{
    if (engine.isRecording()) return;
    const auto* cue = project->findCue (cueId);
    if (cue == nullptr) return;
    const double rate = project->getSampleRate();
    engine.stop();
    engine.setAuditionClip (-1);   // tutto tranne i take ADR
    auditionStop = cue->outPos + (int64_t) (0.3 * rate);
    engine.setPosition (juce::jmax<int64_t> (0, cue->inPos - (int64_t) (1.0 * rate)));
    timeline->showRange (cue->inPos, cue->outPos);
    mixer->resetPeaks();
    engine.play();
    if (adrPanel != nullptr) adrPanel->refresh();
}

void MainComponent::endAudition()
{
    engine.stop();
    engine.setAuditionClip (0);
    if (adrPanel != nullptr) adrPanel->refresh();
}

void MainComponent::addAdrCue()
{
    AdrCue c;
    const double rate = project->getSampleRate();
    if (engine.getLoopEnd() > engine.getLoopStart()) { c.inPos = engine.getLoopStart(); c.outPos = engine.getLoopEnd(); }
    else { c.inPos = engine.getPosition(); c.outPos = c.inPos + (int64_t) (4.0 * rate); }
    c.character = "Personaggio";
    const int id = project->addCue (c);
    project->selectedCueId = id;
    project->sendChangeMessage();
    showAdrWindow();
}

void MainComponent::importAdrDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Importa cue ADR (Pro Tools, EDL, CSV/TSV)", juce::File(), "*.txt;*.csv;*.tsv;*.edl;*.tab");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        const auto result = adr::parse (f.loadFileAsString());
        if (result.cues.empty())
        {
            safe->showError ("Non ho trovato cue in questo file.\n\nFormati letti: testo Session Info di Pro Tools (elenco marker), EDL CMX3600 con locatori o commenti, "
                             "tabelle CSV/TSV con una colonna di timecode.");
            return;
        }
        auto* dlg = new AdrImportDialog (*safe->project, result, [safe, result] (const AdrImportDialog::Values& v)
        {
            if (safe == nullptr) return;
            auto& p = *safe->project;
            p.tcFormat = v.tcFormat;
            p.tcStartSec = v.tcStart;
            juce::StringArray warnings;
            auto cues = adr::toCues (result, p, p.getSampleRate(), v.defaultDuration, warnings);
            const auto firstPos = cues.empty() ? int64_t (0) : cues.front().inPos;
            if (v.makeMarkers) for (auto& c : cues) p.addMarker (c.inPos, c.character + " " + juce::String (c.scene));
            p.addCues (cues, v.replace);
            if (! p.getSession().cues.empty()) { p.selectedCueId = p.getSession().cues.front().id; p.sendChangeMessage(); }
            (void) firstPos;
            safe->showAdrWindow();
            juce::String msg = "Importati " + juce::String ((int) cues.size()) + " cue.";
            if (warnings.size() > 0) msg << "\n\n" << warnings.joinIntoString ("\n");
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Importazione ADR", msg);
        });
        dlg->setSize (560, 420);
        juce::DialogWindow::LaunchOptions lo;
        lo.content.setOwned (dlg);
        lo.dialogTitle = "Importa cue ADR";
        lo.dialogBackgroundColour = theme::panel;
        lo.componentToCentreAround = safe.getComponent();
        lo.escapeKeyTriggersCloseButton = true;
        lo.useNativeTitleBar = true;
        lo.resizable = false;
        lo.launchAsync();
    });
}

void MainComponent::recordAdrCue (int cueId)
{
    if (engine.isRecording()) return;
    if (engine.getAuditionClip() != 0) endAudition();
    const auto* cue = project->findCue (cueId);
    if (cue == nullptr) { showError ("Seleziona un cue nell'elenco ADR."); return; }

    const int trackId = project->findTrack (project->adrTrackId) != nullptr ? project->adrTrackId : project->selectedTrackId;
    if (project->findTrack (trackId) == nullptr) { showError ("Crea o seleziona la traccia su cui registrare l'ADR."); return; }
    if (! engine.ensureInput())
    {
        showError ("Non riesco ad aprire un ingresso audio.\n\nControlla che il microfono sia consentito in Impostazioni di Sistema > Privacy e sicurezza > Microfono, "
                   "e scegli l'ingresso in File > Impostazioni audio.");
        return;
    }

    // si registra solo sulla traccia ADR
    for (auto& t : project->getSession().tracks) t.live->armed = (t.id == trackId);
    project->liveChanged();

    const double rate = project->getSampleRate();
    const int64_t in = cue->inPos, out = cue->outPos;
    const int64_t start = juce::jmax<int64_t> (0, in - (int64_t) (project->adrPreRoll * rate));
    const int64_t latency = engine.recordLatency();
    const int64_t punchIn = in - (int64_t) (0.5 * rate);
    const int64_t punchOut = out + (int64_t) (0.5 * rate) + latency;   // la voce arriva al disco con 'latency' campioni di ritardo

    std::vector<int64_t> beeps;
    for (int k = project->adrBeeps; k >= 1; --k)
        if (in - (int64_t) (k * rate) >= start) beeps.push_back (in - (int64_t) (k * rate));

    const auto* track = project->findTrack (trackId);
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const auto session = project->getFile() == juce::File() ? juce::String ("Sessione") : project->getFile().getFileNameWithoutExtension();
    const std::vector<TakeSpec> specs { { trackId,
        recordingsFolder().getChildFile (juce::File::createLegalFileName (session + "_ADR" + juce::String (cue->number) + "_" + track->name + "_" + stamp) + ".wav"),
        track->live->inputMode.load() } };

    engine.setPosition (start);
    const auto err = engine.startRecording (specs);
    if (err.isNotEmpty()) { showError (err); return; }
    engine.setPunch (punchIn, punchOut);
    engine.setBeeps (beeps);
    adrRecCue = cueId;
    adrRecStop = punchOut + (int64_t) (project->adrPostRoll * rate);
    project->selectedCueId = cueId;
    updateAdrViewer();
    engine.play();
    repaint();
    if (adrPanel != nullptr) adrPanel->refresh();
}

//==============================================================================
namespace
{
juce::String csvField (const juce::String& s) { return "\"" + s.replace ("\"", "\"\"").replaceCharacters ("\r\n", "  ") + "\""; }
}

void MainComponent::exportAdrCsv()
{
    const auto base = (project->getFile() == juce::File() ? juce::String ("ADR") : project->getFile().getFileNameWithoutExtension()) + "_cue_sheet";
    chooser = std::make_unique<juce::FileChooser> ("Esporta cue sheet ADR", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (base).withFileExtension ("csv"), "*.csv");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        auto& p = *safe->project;
        const double rate = p.getSampleRate();
        juce::String csv = "Cue;Stato;Personaggio;Scena;Rullo;TC In;TC Out;Durata (s);Take attivo;Take totali;Dialogo;Note\n";
        for (auto& c : p.getSession().cues)
            csv << c.number << ";" << adrStatusName (c.status) << ";" << csvField (c.character) << ";" << csvField (c.scene) << ";" << csvField (c.reel) << ";"
                << p.formatTc ((double) c.inPos / rate) << ";" << p.formatTc ((double) c.outPos / rate) << ";" << juce::String ((double) (c.outPos - c.inPos) / rate, 2) << ";"
                << c.activeTake << ";" << p.takeCount (c.id) << ";" << csvField (c.dialogue) << ";" << csvField (c.notes) << "\n";
        if (! f.withFileExtension ("csv").replaceWithText (csv)) safe->showError ("Impossibile scrivere il file.");
    });
}

juce::String adrToProToolsText (const Project& p, const juce::String& sessionName)
{
    const double rate = p.getSampleRate();
    static const char* const tcNames[] = { "23.976 Frame", "24 Frame", "25 Frame", "29.97 Frame", "29.97 Drop Frame", "30 Frame", "50 Frame", "60 Frame" };
    juce::String t;
    t << "SESSION NAME:\t" << sessionName << "\n";
    t << "SAMPLE RATE:\t" << juce::String (rate, 6) << "\n";
    t << "BIT DEPTH:\t24-bit\n";
    t << "SESSION START TIMECODE:\t" << p.formatTc (0.0) << "\n";
    t << "TIMECODE FORMAT:\t" << tcNames[juce::jlimit (0, 7, p.tcFormat)] << "\n\n\n";
    t << "M A R K E R S  L I S T I N G\n";
    t << "#   \tLOCATION     \tTIME REFERENCE      \tUNITS    \tNAME                             \tCOMMENTS\n";
    for (auto& c : p.getSession().cues)
    {
        juce::String com = "CHARACTER: " + c.character + " | SCENE: " + c.scene + " | OUT: " + p.formatTc ((double) c.outPos / rate);
        if (c.reel.isNotEmpty()) com << " | REEL: " << c.reel;
        if (c.dialogue.isNotEmpty()) com << " | DIALOGUE: " << c.dialogue.replaceCharacters ("\r\n\t|", "    ");
        if (c.notes.isNotEmpty()) com << " | NOTES: " << c.notes.replaceCharacters ("\r\n\t|", "    ");
        t << c.number << "\t" << p.formatTc ((double) c.inPos / rate) << "\t" << juce::String ((int64_t) std::llround (p.tcStartSec * rate) + c.inPos)
          << "\tSamples\t" << c.character << "\t" << com << "\n";
    }
    return t;
}

void MainComponent::exportAdrProTools()
{
    const auto base = (project->getFile() == juce::File() ? juce::String ("ADR") : project->getFile().getFileNameWithoutExtension()) + "_ADR_markers";
    chooser = std::make_unique<juce::FileChooser> ("Esporta cue come testo Pro Tools", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (base).withFileExtension ("txt"), "*.txt");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        const auto name = safe->project->getFile() == juce::File() ? juce::String ("Sessione") : safe->project->getFile().getFileNameWithoutExtension();
        if (! f.withFileExtension ("txt").replaceWithText (adrToProToolsText (*safe->project, name))) safe->showError ("Impossibile scrivere il file.");
    });
}

int exportAdrTakes (const Project& p, const juce::File& folder, juce::StringArray& errors)
{
    const double rate = p.getSampleRate();
    juce::WavAudioFormat wav;
    int count = 0;
    folder.createDirectory();
    for (auto& cue : p.getSession().cues)
    {
        if (cue.activeTake <= 0) continue;
        for (auto& tr : p.getSession().tracks)
            for (auto& c : tr.clips)
            {
                if (c.cueId != cue.id || c.takeNo != cue.activeTake || c.asset == nullptr) continue;
                const auto name = juce::File::createLegalFileName (juce::String::formatted ("%02d_", cue.number) + (cue.character.isEmpty() ? juce::String ("ADR") : cue.character)
                                                                   + (cue.scene.isEmpty() ? juce::String() : "_sc" + cue.scene) + "_T" + juce::String (c.takeNo)) + ".wav";
                auto file = folder.getNonexistentChildFile (name.upToLastOccurrenceOf (".", false, false), ".wav", false);

                // BWF: la time reference e' la posizione in campioni dalla mezzanotte (inizio sessione + posizione sulla timeline)
                const int64_t ref = (int64_t) std::llround (p.tcStartSec * rate) + c.start;
                juce::StringPairArray meta;
                meta.set (juce::WavAudioFormat::bwavTimeReference, juce::String (ref));
                meta.set (juce::WavAudioFormat::bwavDescription, "ADR " + juce::String (cue.number) + " " + cue.character + " " + cue.scene);
                meta.set (juce::WavAudioFormat::bwavOriginator, "ADR Dub Studio");
                meta.set (juce::WavAudioFormat::bwavOriginatorRef, "ADR" + juce::String (cue.number));
                meta.set (juce::WavAudioFormat::bwavOriginationDate, juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
                meta.set (juce::WavAudioFormat::bwavOriginationTime, juce::Time::getCurrentTime().formatted ("%H:%M:%S"));

                std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w;
                if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) c.asset->numChannels(), 24, meta, 0));
                if (w == nullptr) { errors.add ("Impossibile creare " + file.getFileName()); continue; }
                os.release();

                juce::AudioBuffer<float> buf (c.asset->numChannels(), (int) c.length);
                for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                    buf.copyFrom (ch, 0, c.asset->audio, ch, (int) c.offset, (int) c.length);
                buf.applyGain (juce::Decibels::decibelsToGain (c.gainDb));
                w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
                w.reset();
                ++count;
            }
    }
    return count;
}

void MainComponent::exportAdrBwf()
{
    chooser = std::make_unique<juce::FileChooser> ("Cartella di destinazione dei take ADR", juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
    {
        auto dir = fc.getResult();
        if (safe == nullptr || dir == juce::File()) return;
        juce::StringArray errors;
        const int n = exportAdrTakes (*safe->project, dir, errors);
        juce::String msg = n == 0 ? juce::String ("Nessun take attivo da esportare.") : "Esportati " + juce::String (n) + " file BWF (24 bit) in " + dir.getFullPathName()
                                                                                       + ".\n\nOgni file porta il timecode della posizione sulla timeline: in Pro Tools si importano con Spot e \"Timecode\" oppure \"Original Time Stamp\".";
        if (errors.size() > 0) msg << "\n\n" << errors.joinIntoString ("\n");
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Esporta take ADR", msg);
    });
}

} // namespace fc
