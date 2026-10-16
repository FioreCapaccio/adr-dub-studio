#include "MainComponent.h"
#include "ThemeManager.h"
#include "DubWorkflow.h"
#include "CueSheet.h"
#include "AudioAlign.h"
#include "AutoDuck.h"
#include "BoothTools.h"
#include "Relink.h"
#include "Versions.h"
#include "SessionCheck.h"
#include "WorkflowGuide.h"
#include "Conform.h"
#include "ExternalSync.h"
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
struct LayoutPreset { const char* id; const char* name; int headerW, stripW, trackH; };
const LayoutPreset kLayouts[] = { { "compact", "Compatto", 244, 100, 72 }, { "standard", "Standard", 264, 108, 96 }, { "large", "Ampio", 300, 124, 128 } };


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
        : juce::ThreadWithProgressWindow (TRANS ("Esportazione in corso..."), true, true),
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

/** Ricerca dei media mancanti in una cartella, in un thread con barra di avanzamento. */
class RelinkJob : public juce::ThreadWithProgressWindow
{
public:
    RelinkJob (juce::AudioFormatManager& f, Session s, juce::File dir, double r, juce::String videoName, std::function<void (relink::Result)> d)
        : juce::ThreadWithProgressWindow (TRANS ("Cerco i media mancanti..."), true, true),
          fm (f), session (std::move (s)), folder (std::move (dir)), rate (r), video (std::move (videoName)), done (std::move (d)) {}

    void run() override { result = relink::search (fm, session, folder, rate, video, [this] (double p) { setProgress (p); return ! threadShouldExit(); }); }
    void threadComplete (bool) override { if (done) done (result); }

private:
    juce::AudioFormatManager& fm;
    Session session;
    juce::File folder;
    double rate;
    juce::String video;
    std::function<void (relink::Result)> done;
    relink::Result result;
};

/** Rilevamento dei tagli del video in un thread, con barra di avanzamento. */
class CutJob : public juce::ThreadWithProgressWindow
{
public:
    CutJob (juce::File v, double sens, std::function<void (bool, std::vector<double>, juce::String)> d)
        : juce::ThreadWithProgressWindow (TRANS ("Analisi del video: ricerca dei tagli..."), true, true), file (std::move (v)), sensitivity (sens), done (std::move (d)) {}

    void run() override
    {
        ok = video::detectCuts (file, sensitivity, [this] (double p) { setProgress (p); return ! threadShouldExit(); }, cuts, error);
    }
    void threadComplete (bool) override { if (done) done (ok, cuts, error); }

private:
    juce::File file;
    double sensitivity;
    std::function<void (bool, std::vector<double>, juce::String)> done;
    std::vector<double> cuts;
    juce::String error;
    bool ok = false;
};

/** Esporta un .mov con il video originale (non ricodificato) e il mix della sessione come audio. */
class VideoExportJob : public juce::ThreadWithProgressWindow
{
public:
    /** from/to: intervallo della timeline in campioni (to <= from = tutto il video). */
    VideoExportJob (Session s, double r, juce::File v, double off, juce::File o, int64_t from, int64_t to, std::shared_ptr<video::BurnIn> b, std::function<void (bool, juce::String)> d)
        : juce::ThreadWithProgressWindow (TRANS ("Esportazione video con mix..."), true, true),
          session (std::move (s)), rate (r), videoFile (std::move (v)), offset (off), outFile (std::move (o)), rangeFrom (from), rangeTo (to), burn (std::move (b)), done (std::move (d)) {}

    void run() override
    {
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("arotron_mix", ".wav");
        juce::String msg;
        for (auto& t : session.tracks) if (t.live->trackType.load() == TrackGuide) t.live->mute = true;   // la Guida (dialogo originale) non entra nel video consegnato
        ok = renderMixdown (session, rate, tmp, [this] (double p) { setProgress (p * 0.4); return ! threadShouldExit(); }, msg, rangeFrom, rangeTo);
        if (ok)
        {
            auto prog = [this] (double p) { setProgress (0.4 + 0.6 * p); return ! threadShouldExit(); };
            const double a = rangeTo > rangeFrom ? (double) rangeFrom / rate : 0.0, b = rangeTo > rangeFrom ? (double) rangeTo / rate : -1.0;
            ok = burn != nullptr ? video::muxAudioBurnIn (videoFile, tmp, outFile, offset, *burn, prog, msg, a, b) : video::muxAudio (videoFile, tmp, outFile, offset, prog, msg, a, b);
        }
        tmp.deleteFile();
        message = ok ? TRANS ("Video esportato in ") + outFile.getFullPathName() : msg;
    }

    void threadComplete (bool) override { if (done) done (ok, message); }

private:
    Session session;
    double rate;
    juce::File videoFile;
    double offset = 0.0;
    juce::File outFile;
    int64_t rangeFrom = 0, rangeTo = -1;
    std::shared_ptr<video::BurnIn> burn;
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
                     &fitBtn, &importBtn, &exportBtn, &addTrackBtn, &videoBtn, &mixerBtn, &exitBtn })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    for (auto* b : { &loopBtn, &snapBtn, &followBtn, &monBtn, &videoBtn, &mixerBtn })
        b->setClickingTogglesState (true);
    exitBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a2523));
    exitBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffff8a7d));
    exitBtn.setTooltip (TRANS ("Esci dal programma (chiede se salvare le modifiche)"));
    exitBtn.onClick = [this] { requestQuit(); };
    returnToStart = settings().getBoolValue ("returnToStart", true);
    {
        const auto saved = settings().getValue ("layout", "standard");
        for (int i = 0; i < (int) std::size (kLayouts); ++i) if (saved == kLayouts[i].id) { applyLayout (i, false); break; }
    }
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

    rewindBtn.setTooltip (TRANS ("Vai all'inizio (Home)"));
    playBtn.setTooltip (TRANS ("Play / Stop (barra spaziatrice)"));
    stopBtn.setTooltip (TRANS ("Stop"));
    recBtn.setTooltip (TRANS ("Registra sulle tracce armate (R). Se nessuna e' armata usa la traccia selezionata"));
    loopBtn.setTooltip (TRANS ("Loop (L). Maiusc + trascina sul righello per definire la regione"));
    snapBtn.setTooltip (TRANS ("Aggancio ai bordi dei clip, alla testina e ai fotogrammi (N)"));
    followBtn.setTooltip (TRANS ("La vista segue la testina durante la riproduzione (F)"));
    monBtn.setTooltip (TRANS ("Ascolta l'ingresso durante la registrazione (M). Usa le cuffie per evitare il feedback"));
    zoomOutBtn.setTooltip (TRANS ("Zoom indietro (-)"));
    zoomInBtn.setTooltip (TRANS ("Zoom avanti (+)"));
    fitBtn.setTooltip (TRANS ("Adatta il progetto alla finestra (Maiusc + Z)"));
    addTrackBtn.setTooltip (TRANS ("Aggiunge una traccia (Maiusc + Cmd + T)"));
    importBtn.setTooltip (TRANS ("Importa file audio (Cmd + I)"));
    exportBtn.setTooltip (TRANS ("Esporta il mixdown in WAV, AIFF o FLAC (Cmd + E)"));
    videoBtn.setTooltip (TRANS ("Mostra o nasconde il video (Maiusc + V)"));
    mixerBtn.setTooltip (TRANS ("Mostra o nasconde il mixer (Maiusc + M)"));
    inMeter.setTooltip (TRANS ("Livello dell'ingresso audio"));

    snapBtn.setToggleState (true, juce::dontSendNotification);
    followBtn.setToggleState (true, juce::dontSendNotification);
    mixerBtn.setToggleState (true, juce::dontSendNotification);
    videoBtn.setToggleState (true, juce::dontSendNotification);

    commandManager.registerAllCommandsForTarget (this);
    if (auto xml = juce::XmlDocument::parse (settings().getValue ("keymap", "")))   // scorciatoie personalizzate
        commandManager.getKeyMappings()->restoreFromXml (*xml);
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
    scriptWindow.reset();
    cueWindow.reset();
    mtcWindow.reset();
    mtcSync.reset();
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
        g.setColour (well);
        g.fillRoundedRectangle (f, 10.0f);
        g.setColour (border);
        g.drawRoundedRectangle (f.reduced (0.5f), 10.0f, 1.0f);
    }

    // display del timecode
    {
        const bool rec = engine.isRecording();
        auto f = timeBox.toFloat();
        g.setGradientFill (juce::ColourGradient (well.darker (0.5f), 0.0f, f.getY(), well, 0.0f, f.getBottom(), false));
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

        drawCaption (g, rec ? TRANS ("REC") : TRANS ("Timecode"), timeBox.withTrimmedLeft (12).withHeight (12).translated (0, 3), juce::Justification::centredLeft, rec ? accent : dim);
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
        info << TRANS ("Nessun dispositivo audio");
    if (engine.getPdcLatency() > 0)
        info << TRANS ("   Latenza plug-in ") << juce::String (engine.getPdcLatency() * 1000.0 / project->getSampleRate(), 1) << " ms"
             << (engine.pdcOn.load() ? " (compensata)" : TRANS (" (NON compensata)"));
    info << "   CPU " << juce::String ((int) std::round (engine.deviceManager.getCpuUsage() * 100.0)) << "%";
    g.setColour (dim);
    g.drawText (info, statusBox.reduced (10, 0), juce::Justification::centredRight, true);

    juce::String left;
    juce::Colour leftCol = dim;
    if (engine.isRecording())
    {
        left = TRANS ("REGISTRAZIONE IN CORSO");
        leftCol = accent;
        if (engine.getDroppedBlocks() > 0) left << TRANS ("  -  attenzione: blocchi persi");
    }
    else if (project->getBusyCount() > 0) { left = "Caricamento..."; leftCol = yellow; }
    else if (project->getSession().video != nullptr) left = "Video: " + project->getSession().video->file.getFileName();
    g.setColour (leftCol);
    g.drawText (left, statusBox.reduced (10, 0), juce::Justification::centredLeft, true);

    // versione e copyright, al centro della barra
    g.setColour (dim.withAlpha (0.8f));
    g.setFont (font (10.5f));
    g.drawText (TRANS ("ADR Dub Studio ") + juce::JUCEApplication::getInstance()->getApplicationVersion() + juce::translate (juce::String (juce::CharPointer_UTF8 ("   \xc2\xb7   \xc2\xa9 2026 Fioravante Capaccio"))),
                statusBox, juce::Justification::centred);
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
    g.drawText (label.isEmpty() ? juce::String (TRANS ("Caricamento in corso...")) : label, box.reduced (22, 12).removeFromTop (24), juce::Justification::centredLeft, true);
    auto bar = box.reduced (22, 0).withTrimmedTop (44).withHeight (14).toFloat();
    g.setColour (well);
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

    timeBox = area.removeFromLeft (220);
    area.removeFromLeft (10);

    auto g3 = group (56 + 6 + 56 + 6 + 62);
    put (g3, loopBtn, 56); put (g3, snapBtn, 56); put (g3, followBtn, 62, 0);

    auto g4 = group (16 + 8 + 48);
    inMeter.setBounds (g4.removeFromLeft (16).reduced (0, 1));
    g4.removeFromLeft (8);
    put (g4, monBtn, 48, 0);

    auto g5 = group (32 + 6 + 32 + 6 + 62);
    put (g5, zoomOutBtn, 32); put (g5, zoomInBtn, 32); put (g5, fitBtn, 62, 0);

    auto g6 = group (84 + 6 + 66 + 6 + 66 + 6 + 54 + 6 + 58);
    put (g6, addTrackBtn, 84); put (g6, importBtn, 66); put (g6, exportBtn, 66); put (g6, videoBtn, 54); put (g6, mixerBtn, 58, 0);

    auto g7 = group (44);
    put (g7, exitBtn, 44, 0);

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
        onTitleChanged (name + (project->isDirty() ? " *" : "") + TRANS (" - ADR Dub Studio"));
    }
}

void MainComponent::syncToggles()
{
    loopBtn.setToggleState (engine.isLooping(), juce::dontSendNotification);
}

void MainComponent::showError (const juce::String& msg)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("ADR Dub Studio"), msg);
}

//==============================================================================
void MainComponent::toggleTransport()
{
    if (engine.isRecording()) { finishRecording(); return; }
    if (engine.isPlaying()) { stopPlayback(); return; }
    playStartPos = engine.getPosition();   // da qui si ripartira' dopo lo stop
    startedFromPlay = true;
    mixer->resetPeaks();
    engine.play();
}

void MainComponent::stopPlayback()
{
    const bool wasPlaying = engine.isPlaying();
    engine.stop();
    if (returnToStart && wasPlaying && startedFromPlay && engine.getAuditionClip() == 0) engine.setPosition (playStartPos);
    startedFromPlay = false;
}

void MainComponent::stopAll()
{
    if (engine.isRecording()) finishRecording(); else stopPlayback();
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
        showError (TRANS ("Non riesco ad aprire un ingresso audio.\n\nControlla che il microfono sia consentito in Impostazioni di Sistema > Privacy e sicurezza > Microfono, "
                   "e scegli l'ingresso in File > Impostazioni audio."));
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
        takeName = dub::expandNaming (project->adrNaming, *cue, project->getFile() == juce::File() ? juce::String ("Sessione") : project->getFile().getFileNameWithoutExtension());
    std::vector<Project::RecItem> items;
    for (auto& r : results) items.push_back ({ r.trackId, r.file, r.startPos, r.latency, cueId, takeName });

    juce::Component::SafePointer<MainComponent> safe (this);
    project->addRecordings (items, [safe, drops] (juce::StringArray errs)
    {
        if (safe == nullptr) return;
        if (drops > 0) errs.add (TRANS ("Durante la registrazione sono stati persi alcuni blocchi audio: il disco o il computer non reggevano il ritmo."));
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
    auto opts = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, TRANS ("Modifiche non salvate"),
                                                                 TRANS ("Vuoi salvare le modifiche alla sessione?"), TRANS ("Salva"), TRANS ("Non salvare"), TRANS ("Annulla"));
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
                safe->chooser = std::make_unique<juce::FileChooser> ("Salva sessione", juce::File(), "*.adb");
                safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                                | juce::FileBrowserComponent::warnAboutOverwriting,
                                            [safe, proceed] (const juce::FileChooser& fc)
                {
                    auto f = fc.getResult();
                    if (safe == nullptr || f == juce::File()) return;
                    juce::String err;
                    if (safe->project->save (f.withFileExtension ("adb"), err)) proceed(); else safe->showError (err);
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
    auto opts = juce::MessageBoxOptions::makeOptionsYesNo (juce::MessageBoxIconType::QuestionIcon, TRANS ("Recupero della sessione"),
                                                          TRANS ("ADR Dub Studio non e' stato chiuso correttamente.\n\nVuoi ripristinare la copia di sicurezza del ") + when + "?",
                                                          TRANS ("Ripristina"), TRANS ("Ignora"));
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
            if (safe->project->hasOfflineMedia())
            {
                auto opts = juce::MessageBoxOptions::makeOptionsYesNo (juce::MessageBoxIconType::WarningIcon, TRANS ("Media mancanti"),
                    errs + TRANS ("\n\nI clip restano nella sessione come OFFLINE (senza audio) e non vanno persi se salvi. Vuoi cercare i file in una cartella?"), TRANS ("Cerca..."), TRANS ("Piu' tardi"));
                juce::NativeMessageBox::showAsync (opts, [safe] (int r) { if (r == 0 && safe != nullptr) safe->relinkMedia(); });
            }
            else if (errs.isNotEmpty()) safe->showError (errs);
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
        showError (TRANS ("Il file non esiste piu': ") + f.getFullPathName());
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
        p.adrNaming = v.naming;
        safe->adrViewerKey.clear();
        p.markDirty();
        safe->repaint();
        safe->timeline->repaint();
    });
    dlg->setSize (520, 580);
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (dlg);
    lo.dialogTitle = TRANS ("Impostazioni sessione: timecode e ADR");
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
    t << TRANS ("SCORCIATOIE DA TASTIERA\n\n");
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
    t << TRANS ("MOUSE\n"
         "  Trascina il bordo inferiore di una traccia      altezza traccia (Maiusc: tutte)\n"
         "  Doppio clic sul nome della traccia              rinomina\n"
         "  Clic sul righello                               sposta la testina\n"
         "  Maiusc + trascina sul righello                  regione di loop\n"
         "  Doppio clic sul righello                        elimina la regione di loop\n"
         "  Cmd + rotella / pinch                           zoom orizzontale\n"
         "  Alt + rotella                                   altezza delle tracce\n"
         "  Clic destro su clip, traccia, marker            menu contestuale\n");

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
    lo.dialogTitle = TRANS ("Guida rapida e scorciatoie");
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
        : juce::ThreadWithProgressWindow (TRANS ("Raccolta dei media..."), true, true), items (std::move (i)), done (std::move (d)) {}

    void run() override
    {
        for (size_t k = 0; k < items.size() && ! threadShouldExit(); ++k)
        {
            setProgress ((double) k / (double) items.size());
            setStatusMessage (items[k].source.getFileName());
            if (! items[k].source.copyFileTo (items[k].dest)) { error = TRANS ("Impossibile copiare ") + items[k].source.getFileName(); return; }
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
        showError (TRANS ("Salva prima la sessione: i media vengono raccolti in una cartella accanto al file di sessione."));
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

    if (items.empty()) { showError (TRANS ("Tutti i media si trovano gia' nella cartella del progetto.")); return; }

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
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Media raccolti"),
                                                            juce::String ((int) copied.size()) + TRANS (" file copiati nella cartella del progetto.\nSalva la sessione per mantenere i nuovi collegamenti."));
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
            if (isSessionFile (f)) { project->loadSession (f, [] (juce::String) {}); return; }
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
    const int formatBefore = project->tcFormat;
    project->setVideo (f, [safe, formatBefore] (juce::String msg)
    {
        if (safe == nullptr) return;
        safe->loadingVideo = juce::File();
        if (msg.isNotEmpty()) safe->showError (msg);
        else if (auto v = safe->project->getSession().video; v != nullptr && safe->project->tcFormat != formatBefore)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Frequenza di fotogrammi"),
                                                    TRANS ("Il video e' a ") + juce::String (v->fps, 3) + TRANS (" fps: ho impostato il timecode della sessione su ")
                                                        + juce::String (safe->project->tcFmt().name) + TRANS (". Si cambia in ADR > Impostazioni sessione."));
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
    if (video == nullptr) { showError (TRANS ("Importa prima un video.")); return; }
    if (videoJob != nullptr || exportJob != nullptr) return;

    // con un intervallo scelto sul righello si chiede se esportare solo quello
    const bool hasSel = project->exportOut > project->exportIn && project->exportIn >= 0;
    juce::Component::SafePointer<MainComponent> safe (this);
    auto chooseFile = [safe, video] (bool useRange, bool burnIn)
    {
        if (safe == nullptr) return;
        const auto def = video->file.getParentDirectory().getChildFile (video->file.getFileNameWithoutExtension() + (burnIn ? "_revisione" : (useRange ? "_intervallo" : "_mix"))).withFileExtension ("mov");
        safe->chooser = std::make_unique<juce::FileChooser> ("Esporta video con mix", def, "*.mov");
        safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                    [safe, video, useRange, burnIn] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        f = f.withFileExtension ("mov");
        if (f == video->file) { safe->showError (TRANS ("Scegli un nome diverso da quello del video originale.")); return; }

        const int64_t from = useRange ? safe->project->exportIn : 0, to = useRange ? safe->project->exportOut : -1;
        std::shared_ptr<video::BurnIn> burn;
        if (burnIn)
        {
            burn = std::make_shared<video::BurnIn>();
            const double rate = safe->project->getSampleRate();
            burn->timecodeAt = [fmt = safe->project->tcFmt(), start = safe->project->tcStartSec] (double t) { return secondsToTimecode (t + start, fmt); };
            for (auto& c : safe->project->getSession().cues)
                burn->lines.push_back ({ (double) c.inPos / rate - 1.0, (double) c.outPos / rate, c.character, c.dialogue, c.number });   // la battuta appare 1 s prima dell'ingresso
        }
        safe->videoJob = std::make_unique<VideoExportJob> (safe->project->getSession().cloneDetached(), safe->project->getSampleRate(),
                                                           video->file, video->offsetSec, f, from, to, burn, [safe] (bool ok, juce::String msg)
        {
            juce::MessageManager::callAsync ([safe, ok, msg]
            {
                if (safe == nullptr) return;
                safe->videoJob.reset();
                juce::AlertWindow::showMessageBoxAsync (ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                        TRANS ("Esportazione video"), msg);
            });
        });
        safe->videoJob->launchThread();
    });
    };

    // con il burn-in (timecode, personaggio e battuta incisi sul video) il video viene ricodificato: si chiede prima
    auto choose = [safe, chooseFile] (bool useRange)
    {
        if (safe == nullptr) return;
        if (! video::burnInSupported()) { chooseFile (useRange, false); return; }
        auto opts = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, TRANS ("Esporta video con mix"),
            TRANS ("Vuoi incidere sul video il timecode, il personaggio e la battuta di ogni cue (per la revisione)?\n\nCon il burn-in il video viene ricodificato in H.264: ci vuole piu' tempo."),
            TRANS ("Con burn-in"), TRANS ("Solo il mix"), TRANS ("Annulla"));
        juce::NativeMessageBox::showAsync (opts, [chooseFile, useRange] (int r)
        {
            if (r == 0) chooseFile (useRange, true); else if (r == 1) chooseFile (useRange, false);
        });
    };

    if (! hasSel) { choose (false); return; }
    const double rate = project->getSampleRate();
    auto opts = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, TRANS ("Esporta video con mix"),
        TRANS ("E' stato scelto un intervallo sul righello (") + project->formatTc ((double) project->exportIn / rate) + " - " + project->formatTc ((double) project->exportOut / rate)
            + TRANS (").\n\nVuoi esportare solo quello?"), TRANS ("Solo l'intervallo"), TRANS ("Tutto il video"), TRANS ("Annulla"));
    juce::NativeMessageBox::showAsync (opts, [choose] (int r)
    {
        // NativeMessageBox restituisce l'indice del pulsante (da 0): 0 = intervallo, 1 = tutto, 2 = annulla
        if (r == 0) choose (true); else if (r == 1) choose (false);
    });
}

void MainComponent::exportDialog()
{
    if (project->contentEnd() <= 0) { showError (TRANS ("La sessione e' vuota: importa prima dell'audio.")); return; }
    if (exportJob != nullptr || videoJob != nullptr) return;

    const bool hasLoop = engine.getLoopEnd() > engine.getLoopStart();
    juce::Component::SafePointer<MainComponent> safe (this);
    const bool hasSel = project->exportOut > project->exportIn && project->exportIn >= 0;
    const double rate = project->getSampleRate();
    const auto selLabel = hasSel ? project->formatTc ((double) project->exportIn / rate) + " - " + project->formatTc ((double) project->exportOut / rate) : juce::String();
    auto* dlg = new ExportDialog (hasLoop, engine.getLoopStart(), engine.getLoopEnd(), hasSel, project->exportIn, project->exportOut, selLabel,
                                  [safe] (const ExportOptions& o) { if (safe != nullptr) safe->runExport (o); });
    dlg->setSize (520, 620);

    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (dlg);
    lo.dialogTitle = TRANS ("Esporta audio");
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
                                                       [safe, delivery = o.delivery] (ExportResult r)
        {
            juce::MessageManager::callAsync ([safe, r, delivery]
            {
                if (safe == nullptr) return;
                safe->exportJob.reset();
                if (r.ok && delivery) safe->deliveredThisSession = true;
                if (! r.ok)
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Esportazione"), r.message);
                    return;
                }
                auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::InfoIcon, TRANS ("Esportazione completata"), r.message,
                                                                         TRANS ("Mostra nel Finder"), TRANS ("Chiudi"));
                juce::NativeMessageBox::showAsync (opts, [r] (int res)
                {
                    if (res == 0 && r.files.size() > 0) juce::File (r.files[0]).revealToUser();   // 0 = primo pulsante
                });
            });
        });
        safe->exportJob->launchThread();
    };

    if (o.stems || o.delivery)
    {
        chooser = std::make_unique<juce::FileChooser> (o.delivery ? "Cartella per la consegna" : "Cartella per gli stem", juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [start, base, o] (const juce::FileChooser& fc)
        {
            auto dir = fc.getResult();
            if (dir != juce::File()) start (dir.getChildFile (base + (o.delivery ? "_consegna" : "_stems")));
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
    chooser = std::make_unique<juce::FileChooser> ("Salva sessione", project->getFile(), "*.adb");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        juce::String err;
        if (! safe->project->save (f.withFileExtension ("adb"), err)) safe->showError (err);
        else safe->addRecent (f.withFileExtension ("adb"));
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
        safe->chooser = std::make_unique<juce::FileChooser> ("Apri sessione", juce::File(), "*.adb;*.arostudio");
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
    o.dialogTitle = TRANS ("Impostazioni audio");
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
                  cmdAdrWindow, cmdAdrImport, cmdAdrAddCue, cmdAdrRecord, cmdAdrNextCue, cmdAdrPrevCue, cmdAdrExportCsv, cmdAdrExportPT, cmdAdrExportBwf, cmdTimeStretch, cmdToolStretch, cmdReturnToStart, cmdQuit, cmdAdrCueSheet, cmdAlignToGuide, cmdAutoDuck, cmdDetectCuts, cmdScriptReader, cmdCueMix, cmdRelink, cmdSaveVersion, cmdVersions, cmdCheckSession, cmdWorkflow, cmdReConform, cmdMtc, cmdKeyEditor });
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& i)
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    const auto cmd = ModifierKeys::commandModifier, shift = ModifierKeys::shiftModifier;
    const bool hasSel = ! project->selection.empty();

    switch (id)
    {
        case cmdNew:           i.setInfo (TRANS ("Nuova sessione"), "", TRANS ("File"), 0); i.addDefaultKeypress ('n', cmd); break;
        case cmdOpen:          i.setInfo (TRANS ("Apri..."), "", TRANS ("File"), 0); i.addDefaultKeypress ('o', cmd); break;
        case cmdSave:          i.setInfo (TRANS ("Salva"), "", TRANS ("File"), 0); i.addDefaultKeypress ('s', cmd); break;
        case cmdSaveAs:        i.setInfo (TRANS ("Salva con nome..."), "", TRANS ("File"), 0); i.addDefaultKeypress ('s', cmd | shift); break;
        case cmdImport:        i.setInfo (TRANS ("Importa audio..."), "", TRANS ("File"), 0); i.addDefaultKeypress ('i', cmd); break;
        case cmdExport:        i.setInfo (TRANS ("Esporta audio..."), "", TRANS ("File"), 0); i.addDefaultKeypress ('e', cmd); break;
        case cmdAudioSettings: i.setInfo (TRANS ("Impostazioni audio..."), "", TRANS ("File"), 0); i.addDefaultKeypress (',', cmd); break;

        case cmdUndo:          i.setInfo (TRANS ("Annulla"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('z', cmd); i.setActive (project->canUndo()); break;
        case cmdRedo:          i.setInfo (TRANS ("Ripeti"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('z', cmd | shift); i.setActive (project->canRedo()); break;
        case cmdSelectAll:     i.setInfo (TRANS ("Seleziona tutto"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('a', cmd); break;
        case cmdDuplicate:     i.setInfo (TRANS ("Duplica"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('d', cmd); i.setActive (hasSel); break;
        case cmdDelete:        i.setInfo (TRANS ("Elimina"), "", TRANS ("Modifica"), 0);
                               i.addDefaultKeypress (KeyPress::deleteKey, 0); i.addDefaultKeypress (KeyPress::backspaceKey, 0);
                               i.setActive (hasSel); break;
        case cmdSplit:         i.setInfo (TRANS ("Dividi alla testina"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('s', 0); break;

        case cmdAddTrack:      i.setInfo (TRANS ("Nuova traccia"), "", TRANS ("Traccia"), 0); i.addDefaultKeypress ('t', cmd | shift); break;
        case cmdRemoveTrack:   i.setInfo (TRANS ("Elimina traccia selezionata"), "", TRANS ("Traccia"), 0); break;
        case cmdRenameTrack:   i.setInfo (TRANS ("Rinomina traccia selezionata..."), "", TRANS ("Traccia"), 0); break;

        case cmdPlay:          i.setInfo (engine.isPlaying() ? TRANS ("Stop") : TRANS ("Play"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::spaceKey, 0); break;
        case cmdHome:          i.setInfo (TRANS ("Vai all'inizio"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::homeKey, 0); break;
        case cmdEnd:           i.setInfo (TRANS ("Vai alla fine"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::endKey, 0); break;
        case cmdLoop:          i.setInfo (TRANS ("Loop"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress ('l', 0); i.setTicked (engine.isLooping()); break;
        case cmdLoopFromSel:   i.setInfo (TRANS ("Loop dalla selezione"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress ('l', shift); break;
        case cmdNudgeL:        i.setInfo (TRANS ("Indietro di un fotogramma"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::leftKey, 0); break;
        case cmdNudgeR:        i.setInfo (TRANS ("Avanti di un fotogramma"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::rightKey, 0); break;
        case cmdNudgeLBig:     i.setInfo (TRANS ("Indietro di un secondo"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::leftKey, shift); break;
        case cmdNudgeRBig:     i.setInfo (TRANS ("Avanti di un secondo"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress (KeyPress::rightKey, shift); break;

        case cmdSnap:          i.setInfo (TRANS ("Aggancio (snap)"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('n', 0); i.setTicked (timeline->snapEnabled); break;
        case cmdFollow:        i.setInfo (TRANS ("Segui la testina"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('f', 0); i.setTicked (timeline->followEnabled); break;
        case cmdZoomIn:        i.setInfo (TRANS ("Zoom avanti"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('=', 0); i.addDefaultKeypress ('+', 0); break;
        case cmdZoomOut:       i.setInfo (TRANS ("Zoom indietro"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('-', 0); break;
        case cmdZoomFit:       i.setInfo (TRANS ("Adatta allo schermo"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('z', shift); break;
        case cmdMixer:         i.setInfo (TRANS ("Mostra mixer"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('m', shift); i.setTicked (mixerVisible); break;
        case cmdRecord:        i.setInfo (engine.isRecording() ? TRANS ("Ferma registrazione") : TRANS ("Registra"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress ('r', 0); i.setTicked (engine.isRecording()); break;
        case cmdArm:           i.setInfo (TRANS ("Arma/disarma traccia selezionata"), "", TRANS ("Traccia"), 0); i.addDefaultKeypress ('a', 0); break;
        case cmdMonitor:       i.setInfo (TRANS ("Ascolta l'ingresso durante la registrazione"), "", TRANS ("Trasporto"), 0); i.addDefaultKeypress ('i', 0); i.setTicked (engine.monitorInput.load()); break;
        case cmdImportVideo:   i.setInfo (TRANS ("Importa video..."), "", TRANS ("File"), 0); i.addDefaultKeypress ('i', cmd | shift); break;
        case cmdRemoveVideo:   i.setInfo (TRANS ("Rimuovi video"), "", TRANS ("File"), 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdExportVideo:   i.setInfo (TRANS ("Esporta video con mix..."), "", TRANS ("File"), 0); i.addDefaultKeypress ('e', cmd | shift); i.setActive (project->getSession().video != nullptr); break;
        case cmdShowVideo:     i.setInfo (TRANS ("Mostra video"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('v', shift); i.setTicked (viewerVisible); i.setActive (project->getSession().video != nullptr); break;
        case cmdToolSelect:    i.setInfo (TRANS ("Selezione"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('v', 0); i.setTicked (timeline->getTool() == Tool::Select); break;
        case cmdToolRazor:     i.setInfo (TRANS ("Lama"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('b', 0); i.setTicked (timeline->getTool() == Tool::Razor); break;
        case cmdToolStretch:   i.setInfo (TRANS ("Stretch"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('e', 0); i.setTicked (timeline->getTool() == Tool::Stretch); break;
        case cmdToolSlip:      i.setInfo (TRANS ("Slip"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('y', 0); i.setTicked (timeline->getTool() == Tool::Slip); break;
        case cmdToolHand:      i.setInfo (TRANS ("Mano"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('h', 0); i.setTicked (timeline->getTool() == Tool::Hand); break;
        case cmdToolZoom:      i.setInfo (TRANS ("Zoom"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('z', 0); i.setTicked (timeline->getTool() == Tool::Zoom); break;
        case cmdTimeStretch:   i.setInfo (TRANS ("Time-stretch dei clip selezionati..."), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('t', cmd | juce::ModifierKeys::altModifier); i.setActive (hasSel); break;
        case cmdAdrWindow:     i.setInfo (TRANS ("Finestra ADR"), "", TRANS ("ADR"), 0); i.addDefaultKeypress ('a', cmd | shift); i.setTicked (adrWindow != nullptr); break;
        case cmdAdrImport:     i.setInfo (TRANS ("Importa cue da Pro Tools / EDL / tabella..."), "", TRANS ("ADR"), 0); break;
        case cmdAdrAddCue:     i.setInfo (TRANS ("Nuovo cue dalla regione di loop o dalla testina"), "", TRANS ("ADR"), 0); break;
        case cmdAdrRecord:     i.setInfo (adrRecCue != 0 ? TRANS ("Ferma la registrazione del cue") : TRANS ("Registra il cue selezionato"), "", TRANS ("ADR"), 0);
                               i.addDefaultKeypress ('r', cmd | shift); i.setActive (adrRecCue != 0 || project->findCue (project->selectedCueId) != nullptr); break;
        case cmdAdrNextCue:    i.setInfo (TRANS ("Cue successivo"), "", TRANS ("ADR"), 0); i.addDefaultKeypress (juce::KeyPress::rightKey, cmd | shift); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrPrevCue:    i.setInfo (TRANS ("Cue precedente"), "", TRANS ("ADR"), 0); i.addDefaultKeypress (juce::KeyPress::leftKey, cmd | shift); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrExportCsv:  i.setInfo (TRANS ("Esporta cue sheet (CSV)..."), "", TRANS ("ADR"), 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrExportPT:   i.setInfo (TRANS ("Esporta cue come marker Pro Tools (testo)..."), "", TRANS ("ADR"), 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdAdrExportBwf:  i.setInfo (TRANS ("Esporta take attivi (BWF con timecode)..."), "", TRANS ("ADR"), 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdMtc:           i.setInfo (TRANS ("Sincronia esterna (MTC)..."), "", TRANS ("Trasporto"), 0); i.setTicked (mtcSync != nullptr && mtcSync->isEnabled()); break;
        case cmdKeyEditor:     i.setInfo (TRANS ("Personalizza le scorciatoie da tastiera..."), "", TRANS ("Aiuto"), 0); break;
        case cmdReConform:     i.setInfo (TRANS ("ReConform: nuovo montaggio da EDL..."), "", TRANS ("Modifica"), 0); break;
        case cmdRelink:        i.setInfo (TRANS ("Ricollega i media mancanti..."), "", TRANS ("File"), 0); i.setActive (project->hasOfflineMedia()); break;
        case cmdSaveVersion:   i.setInfo (TRANS ("Salva una versione della sessione..."), "", TRANS ("File"), 0); i.setActive (project->getFile() != juce::File()); break;
        case cmdVersions:      i.setInfo (TRANS ("Versioni della sessione..."), "", TRANS ("File"), 0); i.setActive (project->getFile() != juce::File()); break;
        case cmdCheckSession:  i.setInfo (TRANS ("Controlla la sessione prima della consegna..."), "", TRANS ("File"), 0); break;
        case cmdWorkflow:      i.setInfo (TRANS ("Flusso di lavoro del doppiaggio"), "", TRANS ("Aiuto"), 0); break;
        case cmdScriptReader:  i.setInfo (TRANS ("Lettore copione per l'attore"), "", TRANS ("ADR"), 0); i.setTicked (scriptWindow != nullptr); break;
        case cmdCueMix:        i.setInfo (TRANS ("Cuffie dell'attore e talkback..."), "", TRANS ("ADR"), 0); i.setTicked (cueWindow != nullptr); break;
        case cmdAlignToGuide:  i.setInfo (TRANS ("Allinea alla guida (sincronia automatica)..."), "", TRANS ("Modifica"), 0); break;
        case cmdAutoDuck:      i.setInfo (TRANS ("Ducking automatico del fondo sotto i dialoghi..."), "", TRANS ("Traccia"), 0); break;
        case cmdDetectCuts:    i.setInfo (TRANS ("Rileva i tagli del video (marker)..."), "", TRANS ("Marker"), 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdAdrCueSheet:   i.setInfo (TRANS ("Foglio cue e copione per personaggio (stampabile)..."), "", TRANS ("ADR"), 0); i.setActive (! project->getSession().cues.empty()); break;
        case cmdSessionSettings: i.setInfo (TRANS ("Impostazioni sessione: timecode e ADR..."), "", TRANS ("File"), 0); break;
        case cmdRulerTc:       i.setInfo (TRANS ("Righello in timecode"), "", TRANS ("Vista"), 0); i.setTicked (project->rulerTimecode); break;
        case cmdShortcuts:     i.setInfo (TRANS ("Guida rapida e scorciatoie..."), "", TRANS ("Aiuto"), 0); i.addDefaultKeypress ('?', 0); break;
        case cmdCollectMedia:  i.setInfo (TRANS ("Raccogli i media nella cartella del progetto..."), "", TRANS ("File"), 0); break;
        case cmdClearRecent:   i.setInfo (TRANS ("Svuota l'elenco dei file recenti"), "", TRANS ("File"), 0); break;
        case cmdToolAuto:      i.setInfo (TRANS ("Automazione"), "", TRANS ("Strumenti"), 0); i.addDefaultKeypress ('u', 0); i.setTicked (timeline->getTool() == Tool::Automation); break;
        case cmdClearAutomation: i.setInfo (TRANS ("Cancella l'automazione mostrata (volume, pan o send) della traccia selezionata"), "", TRANS ("Traccia"), 0); break;
        case cmdRippleDelete:  i.setInfo (TRANS ("Elimina con ripple"), "", TRANS ("Modifica"), 0);
                               i.addDefaultKeypress (juce::KeyPress::deleteKey, shift); i.addDefaultKeypress (juce::KeyPress::backspaceKey, shift);
                               i.setActive (hasSel); break;
        case cmdTrimStart:     i.setInfo (TRANS ("Taglia l'inizio alla testina"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('q', 0); break;
        case cmdTrimEnd:       i.setInfo (TRANS ("Taglia la fine alla testina"), "", TRANS ("Modifica"), 0); i.addDefaultKeypress ('w', 0); break;
        case cmdTrackUp:       i.setInfo (TRANS ("Sposta la traccia su"), "", TRANS ("Traccia"), 0); i.addDefaultKeypress (juce::KeyPress::upKey, cmd | juce::ModifierKeys::altModifier); break;
        case cmdTrackDown:     i.setInfo (juce::String (juce::CharPointer_UTF8 ("Sposta la traccia gi\xc3\xb9")), "", TRANS ("Traccia"), 0); i.addDefaultKeypress (juce::KeyPress::downKey, cmd | juce::ModifierKeys::altModifier); break;
        case cmdDuplicateTrack: i.setInfo (TRANS ("Duplica la traccia"), "", TRANS ("Traccia"), 0); i.addDefaultKeypress ('d', cmd | shift); break;
        case cmdAddPlugin:     i.setInfo (TRANS ("Aggiungi plug-in VST3 alla traccia selezionata..."), "", TRANS ("Traccia"), 0); i.addDefaultKeypress ('p', 0); break;
        case cmdAddMarker:     i.setInfo (TRANS ("Aggiungi marker alla testina"), "", TRANS ("Marker"), 0); i.addDefaultKeypress ('m', 0); break;
        case cmdPrevMarker:    i.setInfo (TRANS ("Marker precedente"), "", TRANS ("Marker"), 0); i.addDefaultKeypress (juce::KeyPress::leftKey, juce::ModifierKeys::altModifier); break;
        case cmdNextMarker:    i.setInfo (TRANS ("Marker successivo"), "", TRANS ("Marker"), 0); i.addDefaultKeypress (juce::KeyPress::rightKey, juce::ModifierKeys::altModifier); break;
        case cmdExportMarkers: i.setInfo (TRANS ("Esporta elenco marker (CSV)..."), "", TRANS ("Marker"), 0); i.setActive (! project->getSession().markers.empty()); break;
        case cmdPdc:           i.setInfo (TRANS ("Compensazione latenza plug-in"), "", TRANS ("Trasporto"), 0); i.setTicked (engine.pdcOn.load()); break;
        case cmdReturnToStart: i.setInfo (TRANS ("Allo stop la testina torna al punto di partenza"), "", TRANS ("Trasporto"), 0); i.setTicked (returnToStart); break;
        case cmdQuit:          i.setInfo (TRANS ("Esci"), "", TRANS ("File"), 0);
#if ! JUCE_MAC
                               i.addDefaultKeypress ('q', cmd);
#endif
                               break;
        case cmdAddMasterPlugin: i.setInfo (TRANS ("Aggiungi plug-in VST3 al master..."), "", TRANS ("Traccia"), 0); break;
        case cmdDetachVideo:   i.setInfo (videoWindow != nullptr ? TRANS ("Aggancia la finestra video") : TRANS ("Stacca la finestra video"), "", TRANS ("Vista"), 0);
                               i.addDefaultKeypress ('d', shift); i.setActive (project->getSession().video != nullptr); break;
        case cmdFitTracks:     i.setInfo (TRANS ("Adatta l'altezza delle tracce"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('f', shift); break;
        case cmdVideoOffsetMinus: i.setInfo (TRANS ("Anticipa il video di 1 ms"), "", TRANS ("Vista"), 0); i.addDefaultKeypress (',', 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdVideoOffsetPlus:  i.setInfo (TRANS ("Ritarda il video di 1 ms"), "", TRANS ("Vista"), 0); i.addDefaultKeypress ('.', 0); i.setActive (project->getSession().video != nullptr); break;
        case cmdTaller:        i.setInfo (TRANS ("Tracce piu' alte"), "", TRANS ("Vista"), 0); i.addDefaultKeypress (KeyPress::upKey, cmd); break;
        case cmdShorter:       i.setInfo (TRANS ("Tracce piu' basse"), "", TRANS ("Vista"), 0); i.addDefaultKeypress (KeyPress::downKey, cmd); break;
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
        case cmdAdrCueSheet:   exportCueSheet(); break;
        case cmdAlignToGuide:  alignSelectedToGuide(); break;
        case cmdMtc:           toggleMtcWindow(); break;
        case cmdKeyEditor:     keyEditorDialog(); break;
        case cmdReConform:     reconformDialog(); break;
        case cmdRelink:        relinkMedia(); break;
        case cmdSaveVersion:   saveVersionDialog(); break;
        case cmdVersions:      versionsDialog(); break;
        case cmdCheckSession:  checkSessionDialog(); break;
        case cmdWorkflow:      workflowDialog(); break;
        case cmdScriptReader:  toggleScriptWindow(); break;
        case cmdCueMix:        toggleCueWindow(); break;
        case cmdAutoDuck:      autoDuckDialog(); break;
        case cmdDetectCuts:    detectVideoCuts(); break;
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
        case cmdReturnToStart: returnToStart = ! returnToStart; settings().setValue ("returnToStart", returnToStart); settings().saveIfNeeded(); break;
        case cmdQuit:          requestQuit(); break;
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
    if (id >= 32000 && id < 32100)
    {
        const size_t i = (size_t) (id - 32000);
        if (i < langActions.size() && lang::savedCode() != langActions[i])
        {
            lang::saveCode (langActions[i]);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "ADR Dub Studio",
                                                    lang::translateTo (langActions[i], "Il cambio di lingua si applica al riavvio dell'app."));
        }
        return;
    }
    if (id >= 31000 && id < 31100)
    {
        newFromTemplate (id - 31000);
        return;
    }
    if (id >= 30000)
    {
        const size_t i = (size_t) (id - 30000);
        if (i < menuActions.size())
            juce::MessageManager::callAsync ([action = menuActions[i]] { action(); });
        return;
    }
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
        if (! f.withFileExtension ("csv").replaceWithText (csv)) safe->showError (TRANS ("Impossibile scrivere il file."));
    });
}

//==============================================================================
// Temi e layout
//==============================================================================
juce::PopupMenu MainComponent::buildThemeMenu()
{
    juce::PopupMenu sub;
    const auto current = ThemeManager::currentId();
    auto addAction = [this] (juce::PopupMenu& menu, const juce::String& label, bool ticked, std::function<void()> f)
    {
        menu.addItem (30000 + (int) menuActions.size(), label, true, ticked);
        menuActions.push_back (std::move (f));
    };

    for (auto& t : ThemeManager::builtins())
        addAction (sub, t.name, current == t.id, [id = t.id] { ThemeManager::select (id); });

    const auto imported = ThemeManager::imported();
    if (! imported.empty())
    {
        sub.addSeparator();
        for (auto& t : imported)
            addAction (sub, t.name, current == t.id, [id = t.id] { ThemeManager::select (id); });
    }

    sub.addSeparator();
    juce::PopupMenu found;
    for (auto& f : reaper::findInstalledThemes())
        addAction (found, f.getFileNameWithoutExtension(), false, [this, f] { importThemeFile (f); });
    if (found.getNumItems() == 0) found.addItem (29999, TRANS ("Nessun tema trovato nella cartella di Reaper"), false);
    sub.addSubMenu (juce::translate (juce::String::fromUTF8 ("Temi di Reaper installati")), found);
    addAction (sub, TRANS ("Importa un tema Reaper..."), false, [this] { chooseThemeFile(); });

    // rimuovere un tema importato (solo se e' quello in uso)
    for (auto& t : imported)
        if (t.id == current)
        {
            sub.addSeparator();
            addAction (sub, TRANS ("Rimuovi \"") + t.name + TRANS ("\" dall'elenco"), false, [t] { ThemeManager::remove (t); ThemeManager::select ("dark"); });
        }
    return sub;
}

void MainComponent::importThemeFile (const juce::File& f)
{
    ThemeEntry t;
    juce::String err;
    if (! ThemeManager::importReaperTheme (f, t, err)) { showError (TRANS ("Tema non importato: ") + err); return; }
    ThemeManager::select (t.id);
}

void MainComponent::chooseThemeFile()
{
    auto start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto installed = reaper::findInstalledThemes();
    if (installed.size() > 0) start = installed[0].getParentDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Scegli un tema di Reaper (.ReaperThemeZip o .ReaperTheme)", start, "*.ReaperThemeZip;*.ReaperTheme;*.zip");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult() != juce::File()) safe->importThemeFile (fc.getResult());
    });
}

juce::PopupMenu MainComponent::buildLayoutMenu()
{
    juce::PopupMenu sub;
    for (int i = 0; i < (int) std::size (kLayouts); ++i)
    {
        sub.addItem (30000 + (int) menuActions.size(), kLayouts[i].name, true, i == layoutIndex);
        menuActions.push_back ([this, i] { applyLayout (i, true); });
    }
    return sub;
}

void MainComponent::applyLayout (int index, bool resizeTracks)
{
    index = juce::jlimit (0, (int) std::size (kLayouts) - 1, index);
    layoutIndex = index;
    TimelineView::headerW = kLayouts[index].headerW;
    MixerView::stripW = kLayouts[index].stripW;
    if (timeline == nullptr) return;   // all'avvio: si imposta solo la misura, i componenti non ci sono ancora
    if (resizeTracks) timeline->setAllTrackHeights (kLayouts[index].trackH);
    settings().setValue ("layout", juce::String (kLayouts[index].id));
    settings().saveIfNeeded();
    resized();
    timeline->resized();
    mixer->resized();
    repaint();
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { TRANS ("File"), TRANS ("Modifica"), TRANS ("Traccia"), TRANS ("Strumenti"), TRANS ("Marker"), TRANS ("ADR"), TRANS ("Trasporto"), TRANS ("Vista"), TRANS ("Aiuto") };
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
            add ({ cmdNew });
            {
                juce::PopupMenu models;
                const auto& list = dub::templates();
                for (size_t i = 0; i < list.size(); ++i)
                    models.addItem (31000 + (int) i, juce::translate (list[i].name) + "   -   " + juce::translate (list[i].description));
                m.addSubMenu (TRANS ("Nuovo da modello"), models);
            }
            add ({ cmdOpen });
            juce::PopupMenu recent;
            const auto list = juce::StringArray::fromLines (settings().getValue ("recent", ""));
            for (int i = 0; i < list.size(); ++i)
                if (list[i].isNotEmpty()) recent.addItem (2000 + i, juce::File (list[i]).getFileNameWithoutExtension() + "   -   " + juce::File (list[i]).getParentDirectory().getFileName());
            if (recent.getNumItems() > 0) { recent.addSeparator(); recent.addCommandItem (&commandManager, cmdClearRecent); }
            m.addSubMenu (TRANS ("Apri recente"), recent, recent.getNumItems() > 0);
            m.addSeparator();
            add ({ cmdSave, cmdSaveAs, cmdSaveVersion, cmdVersions, 0, cmdCollectMedia, cmdRelink, cmdCheckSession, 0, cmdSessionSettings, 0, cmdImport, cmdImportVideo, cmdRemoveVideo, 0, cmdExport, cmdExportVideo, 0, cmdAudioSettings,
#if ! JUCE_MAC
                  0, cmdQuit
#endif
            });
            break;
        }
        case 99: add ({ cmdNew, cmdOpen, 0, cmdSave, cmdSaveAs, 0, cmdImport, cmdImportVideo, cmdRemoveVideo, 0, cmdExport, cmdExportVideo, 0, cmdAudioSettings }); break;
        case 1: add ({ cmdUndo, cmdRedo, 0, cmdSelectAll, cmdSplit, cmdTrimStart, cmdTrimEnd, cmdTimeStretch, cmdAlignToGuide, cmdReConform, 0, cmdDuplicate, cmdDelete, cmdRippleDelete }); break;
        case 2: add ({ cmdAddTrack, cmdDuplicateTrack, cmdRenameTrack, cmdRemoveTrack, 0, cmdTrackUp, cmdTrackDown, 0, cmdArm, cmdClearAutomation, 0, cmdAddPlugin, cmdAddMasterPlugin, 0, cmdAutoDuck }); break;
        case 3: add ({ cmdToolSelect, cmdToolRazor, cmdToolSlip, cmdToolStretch, cmdToolHand, cmdToolZoom, cmdToolAuto }); break;
        case 4:
        {
            add ({ cmdAddMarker, cmdPrevMarker, cmdNextMarker, cmdExportMarkers, cmdDetectCuts });
            const auto& marks = project->getSession().markers;
            if (! marks.empty()) m.addSeparator();
            for (size_t i = 0; i < marks.size() && i < 60; ++i)
                m.addItem (1000 + (int) i, marks[i].name + "   " + project->formatTc ((double) marks[i].pos / project->getSampleRate()));
            break;
        }
        case 5: add ({ cmdAdrWindow, 0, cmdAdrImport, cmdAdrAddCue, 0, cmdAdrPrevCue, cmdAdrNextCue, cmdAdrRecord, 0, cmdAdrExportCsv, cmdAdrExportPT, cmdAdrExportBwf, cmdAdrCueSheet, 0, cmdScriptReader, cmdCueMix, 0, cmdSessionSettings }); break;
        case 6: add ({ cmdPlay, cmdRecord, cmdMonitor, cmdPdc, cmdReturnToStart, 0, cmdHome, cmdEnd, 0, cmdLoop, cmdLoopFromSel, 0, cmdNudgeL, cmdNudgeR, cmdNudgeLBig, cmdNudgeRBig, 0, cmdMtc }); break;
        case 8:
        {
            add ({ cmdWorkflow, cmdShortcuts, cmdKeyEditor });
            m.addSeparator();
            juce::PopupMenu langs;
            langActions.clear();
            for (auto& l : lang::languages())
            {
                langs.addItem (32000 + (int) langActions.size(), l.name, true, lang::savedCode() == l.code);
                langActions.push_back (l.code);
            }
            m.addSubMenu (TRANS ("Lingua / Language"), langs);
            break;
        }
        case 7:
            add ({ cmdSnap, cmdFollow, 0, cmdZoomIn, cmdZoomOut, cmdZoomFit, 0, cmdTaller, cmdShorter, cmdFitTracks, 0, cmdRulerTc, cmdShowVideo, cmdDetachVideo, cmdMixer });
            menuActions.clear();
            m.addSeparator();
            m.addSubMenu (TRANS ("Tema"), buildThemeMenu());
            m.addSubMenu (TRANS ("Layout"), buildLayoutMenu());
            break;
        default: break;
    }
    return m;
}

//==============================================================================
void MainComponent::timeStretchSelected (bool toCue)
{
    std::vector<int64_t> ids (project->selection.begin(), project->selection.end());
    if (ids.empty()) { showError (TRANS ("Seleziona almeno un clip.")); return; }
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
            if (ratio < 0.25 || ratio > 4.0) { report (TRANS ("La durata del cue ") + juce::String (cue->number) + TRANS (" e' troppo diversa dal take (oltre 4 volte).")); continue; }
            project->stretchClip (id, ratio, true, folder, report);
            ++n;
        }
        if (n == 0) report ("Seleziona un clip collegato a un cue ADR.");
        return;
    }

    auto* aw = new juce::AlertWindow (TRANS ("Time-stretch"), TRANS ("Cambia la durata dei clip selezionati. Sopra 100% i clip diventano piu' lunghi (piu' lenti)."), juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("pct", "100", TRANS ("Durata finale (% dell'originale, da 25 a 400)"));
    aw->addComboBox ("mode", { TRANS ("Mantieni il tono (voce, dialoghi)"), TRANS ("Cambia anche il tono (come un nastro)") }, TRANS ("Modo"));
    aw->addButton (TRANS ("Applica"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, ids, folder, report] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        const double pct = aw->getTextEditorContents ("pct").replaceCharacter (',', '.').getDoubleValue();
        if (pct < 25.0 || pct > 400.0) { safe->showError (TRANS ("Scrivi una percentuale tra 25 e 400.")); return; }
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
        info.title = TRANS ("ADR #") + juce::String (c->number) + "   " + c->character.toUpperCase()
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
            safe->showError (TRANS ("Non ho trovato cue in questo file.\n\nFormati letti: testo Session Info di Pro Tools (elenco marker), EDL CMX3600 con locatori o commenti, "
                             "tabelle CSV/TSV con una colonna di timecode."));
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
            if (v.makeTracks) { const int n = dub::createCharacterTracks (p, cues); if (n > 0) warnings.add (juce::String (n) + TRANS (" tracce Dialoghi create (una per personaggio).")); }
            p.addCues (cues, v.replace);
            if (! p.getSession().cues.empty()) { p.selectedCueId = p.getSession().cues.front().id; p.sendChangeMessage(); }
            (void) firstPos;
            safe->showAdrWindow();
            juce::String msg = "Importati " + juce::String ((int) cues.size()) + " cue.";
            if (warnings.size() > 0) msg << "\n\n" << warnings.joinIntoString ("\n");
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Importazione ADR"), msg);
        });
        dlg->setSize (560, 500);
        juce::DialogWindow::LaunchOptions lo;
        lo.content.setOwned (dlg);
        lo.dialogTitle = TRANS ("Importa cue ADR");
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
    if (cue == nullptr) { showError (TRANS ("Seleziona un cue nell'elenco ADR.")); return; }

    int trackId = project->findTrack (project->adrTrackId) != nullptr ? project->adrTrackId : project->selectedTrackId;
    if (const int forCharacter = dub::trackForCue (*project, *cue); forCharacter >= 0 && project->findTrack (project->adrTrackId) == nullptr) trackId = forCharacter;   // traccia del personaggio, se non ne hai scelta una a mano
    if (project->findTrack (trackId) == nullptr) { showError (TRANS ("Crea o seleziona la traccia su cui registrare l'ADR.")); return; }
    if (! engine.ensureInput())
    {
        showError (TRANS ("Non riesco ad aprire un ingresso audio.\n\nControlla che il microfono sia consentito in Impostazioni di Sistema > Privacy e sicurezza > Microfono, "
                   "e scegli l'ingresso in File > Impostazioni audio."));
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

juce::String MainComponent::reconformRun (const juce::File& oldEdl, const juce::File& newEdl, bool markers, bool saveVersion, bool apply)
{
    if (! oldEdl.existsAsFile() || ! newEdl.existsAsFile()) return TRANS ("Scegli i due file EDL: quello del vecchio montaggio e quello del nuovo.");
    const auto po = conform::parseEdl (oldEdl.loadFileAsString(), project->tcFormat), pn = conform::parseEdl (newEdl.loadFileAsString(), project->tcFormat);
    juce::String out;
    for (auto& w : po.warnings) out << TRANS ("VECCHIO EDL: ") << w << "\n";
    for (auto& w : pn.warnings) out << TRANS ("NUOVO EDL: ") << w << "\n";
    if (po.events.empty() || pn.events.empty()) return out + TRANS ("\nUno dei due EDL non contiene eventi utilizzabili: niente da fare.");

    auto map = conform::buildMap (po.events, pn.events);
    for (auto& w : map.warnings) out << w << "\n";
    if (map.segments.empty()) return out;

    if (apply && saveVersion && project->getFile() != juce::File())
    {
        juce::String err;
        const auto v = versions::save (*project, TRANS ("prima del ReConform"), err);
        if (v == juce::File()) return out + "\nNon sono riuscito a salvare la versione di sicurezza (" + err + "): non applico nulla.";
        out << TRANS ("Versione di sicurezza salvata: ") << v.getFileName() << "\n";
    }
    const auto plan = conform::makePlan (map, project->tcStartSec, project->getSampleRate(), markers);
    const auto st = project->conform (plan, apply);
    if (! out.isEmpty()) out << "\n";
    out << conform::describe (map, st, project->getSampleRate()) << "\n\n"
        << (apply ? TRANS ("FATTO. Per tornare indietro: Modifica > Annulla.\nOra importa il video del nuovo montaggio (File > Importa video) e controlla la sincronia.")
                  : TRANS ("Questa e' solo un'anteprima: la sessione non e' cambiata. Premi Applica per eseguire."));
    if (apply) { timeline->repaint(); adrViewerKey.clear(); }
    return out;
}

namespace
{
/** Finestra del ReConform: due EDL, opzioni, anteprima e applicazione. */
class ReconformPanel : public juce::Component
{
public:
    using Runner = std::function<juce::String (const juce::File&, const juce::File&, bool, bool, bool)>;
    explicit ReconformPanel (Runner r, bool canSaveVersion) : run (std::move (r))
    {
        for (auto* l : { &oldLbl, &newLbl }) { l->setFont (font (12.0f, true)); l->setColour (juce::Label::textColourId, dim); addAndMakeVisible (*l); }
        oldLbl.setText (TRANS ("EDL DEL VECCHIO MONTAGGIO"), juce::dontSendNotification);
        newLbl.setText (TRANS ("EDL DEL NUOVO MONTAGGIO"), juce::dontSendNotification);
        for (auto* p : { &oldPath, &newPath }) { p->setReadOnly (true); p->setTextToShowWhenEmpty (TRANS ("(nessun file)"), dim); addAndMakeVisible (*p); }
        for (auto* b : { &oldBtn, &newBtn, &previewBtn, &applyBtn, &closeBtn }) addAndMakeVisible (*b);
        addAndMakeVisible (markers);
        addAndMakeVisible (saveVersion);
        markers.setToggleState (true, juce::dontSendNotification);
        saveVersion.setToggleState (canSaveVersion, juce::dontSendNotification);
        saveVersion.setEnabled (canSaveVersion);
        report.setMultiLine (true);
        report.setReadOnly (true);
        report.setFont (font (12.5f));
        report.setText (TRANS ("Scegli l'EDL del montaggio su cui hai lavorato fin qui e quello del nuovo montaggio (stessi nomi di bobina), poi premi Anteprima.\n\n"
                        "Audio, cue e marker vengono spostati dove si trova ora lo stesso materiale video. Il materiale tolto viene tagliato, i cue che cadono li' diventano \"Da rifare\", "
                        "il materiale nuovo resta senza audio. Il timecode degli EDL si legge nel formato della sessione."), false);
        addAndMakeVisible (report);
        applyBtn.setEnabled (false);
        oldBtn.onClick = [this] { choose (oldFile, oldPath); };
        newBtn.onClick = [this] { choose (newFile, newPath); };
        previewBtn.onClick = [this] { report.setText (run (oldFile, newFile, markers.getToggleState(), false, false), false); applyBtn.setEnabled (oldFile.existsAsFile() && newFile.existsAsFile()); };
        applyBtn.onClick = [this]
        {
            report.setText (run (oldFile, newFile, markers.getToggleState(), saveVersion.getToggleState(), true), false);
            applyBtn.setEnabled (false);
        };
        closeBtn.onClick = [this] { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); };
    }
    void paint (juce::Graphics& g) override { g.fillAll (panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto row = [&r] (juce::Label& l, juce::TextEditor& t, juce::TextButton& b)
        {
            l.setBounds (r.removeFromTop (18));
            auto line = r.removeFromTop (28);
            b.setBounds (line.removeFromRight (100));
            line.removeFromRight (8);
            t.setBounds (line);
            r.removeFromTop (8);
        };
        row (oldLbl, oldPath, oldBtn);
        row (newLbl, newPath, newBtn);
        markers.setBounds (r.removeFromTop (24));
        saveVersion.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (34);
        r.removeFromBottom (8);
        report.setBounds (r);
        closeBtn.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (8);
        applyBtn.setBounds (bottom.removeFromRight (100));
        bottom.removeFromRight (8);
        previewBtn.setBounds (bottom.removeFromRight (110));
    }

private:
    void choose (juce::File& target, juce::TextEditor& label)
    {
        chooser = std::make_shared<juce::FileChooser> ("Scegli l'EDL (CMX3600)", juce::File(), "*.edl;*.txt");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, &target, &label] (const juce::FileChooser& fc)
        {
            if (fc.getResult() == juce::File()) return;
            target = fc.getResult();
            label.setText (target.getFullPathName(), false);
            applyBtn.setEnabled (false);
        });
    }

    Runner run;
    juce::File oldFile, newFile;
    std::shared_ptr<juce::FileChooser> chooser;
    juce::Label oldLbl, newLbl;
    juce::TextEditor oldPath, newPath, report;
    juce::TextButton oldBtn { TRANS ("Scegli...") }, newBtn { "Scegli..." }, previewBtn { "Anteprima" }, applyBtn { "Applica" }, closeBtn { "Chiudi" };
    juce::ToggleButton markers { TRANS ("Aggiungi un marker sul materiale nuovo (senza audio)") }, saveVersion { TRANS ("Salva prima una versione della sessione (consigliato)") };
};
} // namespace

void MainComponent::toggleMtcWindow()
{
    if (mtcWindow != nullptr) { mtcWindow.reset(); commandManager.commandStatusChanged(); return; }
    if (mtcSync == nullptr) mtcSync = std::make_unique<mtc::Sync> (engine, [this] { return project->tcStartSec; });
    juce::Component::SafePointer<MainComponent> safe (this);
    mtcWindow = std::make_unique<BoothWindow> (TRANS ("Sincronia esterna (MTC) - ADR Dub Studio"), [safe]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->mtcWindow.reset(); safe->commandManager.commandStatusChanged(); } });
    });
    mtcWindow->setContentOwned (new mtc::Panel (*mtcSync), false);
    mtcWindow->setResizable (false, false);
    mtcWindow->setSize (520, 330);
    mtcWindow->centreAroundComponent (this, 520, 330);
    mtcWindow->setVisible (true);
    commandManager.commandStatusChanged();
}

namespace
{
/** Editor delle scorciatoie: salva le modifiche nelle preferenze quando si chiude. */
class KeyEditorPanel : public juce::Component
{
public:
    explicit KeyEditorPanel (juce::ApplicationCommandManager& cm) : manager (cm), editor (*cm.getKeyMappings(), true)
    {
        addAndMakeVisible (editor);
        addAndMakeVisible (resetBtn);
        resetBtn.onClick = [this]
        {
            manager.getKeyMappings()->resetToDefaultMappings();
            editor.parentHierarchyChanged();
            editor.resized();
            editor.repaint();
        };
        setSize (720, 560);
    }
    ~KeyEditorPanel() override
    {
        if (auto xml = manager.getKeyMappings()->createXml (true)) settings().setValue ("keymap", xml->toString());
        else settings().removeValue ("keymap");   // uguale alle predefinite
        settings().saveIfNeeded();
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bottom = r.removeFromBottom (34);
        r.removeFromBottom (8);
        editor.setBounds (r);
        resetBtn.setBounds (bottom.removeFromRight (190));
    }

private:
    juce::ApplicationCommandManager& manager;
    juce::KeyMappingEditorComponent editor;
    juce::TextButton resetBtn { TRANS ("Ripristina le predefinite") };
};
} // namespace

void MainComponent::keyEditorDialog()
{
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (new KeyEditorPanel (commandManager));
    lo.dialogTitle = TRANS ("Scorciatoie da tastiera");
    lo.dialogBackgroundColour = theme::panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = true;
    lo.launchAsync();
}

void MainComponent::reconformDialog()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* panel = new ReconformPanel ([safe] (const juce::File& a, const juce::File& b, bool m, bool v, bool apply) -> juce::String
    {
        return safe != nullptr ? safe->reconformRun (a, b, m, v, apply) : juce::String();
    }, project->getFile() != juce::File());
    panel->setSize (620, 520);
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (panel);
    lo.dialogTitle = TRANS ("ReConform: nuovo montaggio");
    lo.dialogBackgroundColour = theme::panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = true;
    lo.launchAsync();
}

void MainComponent::relinkMedia()
{
    if (! project->hasOfflineMedia()) { showError (TRANS ("Tutti i media della sessione sono presenti.")); return; }
    chooser = std::make_unique<juce::FileChooser> ("Scegli la cartella in cui cercare i file mancanti (si cerca anche nelle sottocartelle)", juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();
        if (safe == nullptr || folder == juce::File()) return;
        const juce::String videoName = safe->project->getSession().video == nullptr && safe->project->missingVideoPath.isNotEmpty() ? juce::File (safe->project->missingVideoPath).getFileName() : juce::String();
        safe->relinkJob = std::make_unique<RelinkJob> (safe->project->getFormats(), safe->project->getSession().cloneDetached(), folder, safe->project->getSampleRate(), videoName,
                                                       [safe] (relink::Result r)
        {
            juce::MessageManager::callAsync ([safe, r]
            {
                if (safe == nullptr) return;
                safe->relinkJob.reset();
                safe->project->replaceAssets (r.replacements);
                if (r.video != juce::File())
                {
                    safe->project->relinkVideo (r.video, [safe] (juce::String err) { if (safe != nullptr) { safe->timeline->repaint(); if (err.isNotEmpty()) safe->showError (err); } });
                }
                juce::String msg = juce::String (r.found) + (r.found == 1 ? " file ricollegato" : " file ricollegati") + (r.video != juce::File() ? " e il video" : juce::String()) + ".";
                if (! r.tooShort.isEmpty()) msg << TRANS ("\n\nTrovati ma troppo corti (non usati): ") << r.tooShort.joinIntoString (", ");
                if (! r.notFound.isEmpty()) msg << TRANS ("\n\nNon trovati: ") << r.notFound.joinIntoString (", ");
                safe->commandManager.commandStatusChanged();
                safe->timeline->repaint();
                juce::AlertWindow::showMessageBoxAsync (r.notFound.isEmpty() && r.tooShort.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon, TRANS ("Ricollega i media"), msg);
            });
        });
        safe->relinkJob->launchThread();
    });
}

void MainComponent::saveVersionDialog()
{
    if (project->getFile() == juce::File()) { showError (TRANS ("Salva prima la sessione: le versioni stanno in una cartella accanto al file.")); return; }
    auto* aw = new juce::AlertWindow (TRANS ("Salva una versione"), TRANS ("Copia numerata della sessione cosi' com'e' adesso, per poter tornare indietro (anche dopo aver chiuso il programma)."), juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("note", "", TRANS ("Nota (facoltativa, per esempio \"prima del nuovo montaggio\")"));
    aw->addButton (TRANS ("Salva versione"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        juce::String err;
        const auto f = versions::save (*safe->project, aw->getTextEditorContents ("note"), err);
        if (f == juce::File()) safe->showError (err);
        else juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Versione salvata"), f.getFileName() + TRANS ("\n\nSi ritrova in File > Versioni della sessione."));
    }), true);
}

namespace
{
/** Elenco delle versioni di una sessione con Apri ed Elimina. */
class VersionsPanel : public juce::Component, private juce::ListBoxModel
{
public:
    VersionsPanel (juce::File session, std::function<void (juce::File)> open) : sessionFile (std::move (session)), openFn (std::move (open))
    {
        list.setRowHeight (40);
        list.setColour (juce::ListBox::backgroundColourId, bg);
        list.setColour (juce::ListBox::outlineColourId, border);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);
        addAndMakeVisible (openBtn);
        addAndMakeVisible (delBtn);
        openBtn.onClick = [this] { const int r = list.getSelectedRow(); if (r >= 0 && r < (int) items.size()) { auto f = items[(size_t) r].file; if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); openFn (f); } };
        delBtn.onClick = [this]
        {
            const int r = list.getSelectedRow();
            if (r < 0 || r >= (int) items.size()) return;
            versions::remove (items[(size_t) r]);
            refresh();
        };
        refresh();
    }
    void paint (juce::Graphics& g) override { g.fillAll (panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto bottom = r.removeFromBottom (32);
        r.removeFromBottom (8);
        list.setBounds (r);
        openBtn.setBounds (bottom.removeFromRight (110));
        bottom.removeFromRight (8);
        delBtn.setBounds (bottom.removeFromRight (100));
    }

private:
    void refresh() { items = versions::list (sessionFile); list.updateContent(); list.repaint(); if (! items.empty()) list.selectRow (0); }
    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) items.size()) return;
        const auto& v = items[(size_t) row];
        if (selected) { g.setColour (blue.withAlpha (0.3f)); g.fillRect (0, 0, w, h); }
        g.setColour (text);
        g.setFont (font (13.0f, true));
        g.drawText (TRANS ("Versione ") + juce::String (v.number) + "   " + v.time.formatted ("%d/%m/%Y %H:%M"), 12, 3, w - 24, 18, juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (font (12.0f));
        g.drawText (v.note.isEmpty() ? juce::String (TRANS ("(nessuna nota)")) : v.note, 12, 21, w - 24, 16, juce::Justification::centredLeft, true);
    }
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { openBtn.triggerClick(); }

    juce::File sessionFile;
    std::function<void (juce::File)> openFn;
    std::vector<versions::Entry> items;
    juce::ListBox list { {}, this };
    juce::TextButton openBtn { TRANS ("Apri") }, delBtn { "Elimina" };
};
} // namespace

void MainComponent::versionsDialog()
{
    if (project->getFile() == juce::File()) { showError (TRANS ("Salva prima la sessione.")); return; }
    if (versions::list (project->getFile()).empty()) { showError (TRANS ("Non ci sono ancora versioni: usa File > Salva una versione.")); return; }
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* panel = new VersionsPanel (project->getFile(), [safe] (juce::File f) { juce::MessageManager::callAsync ([safe, f] { if (safe != nullptr) safe->openFile (f); }); });
    panel->setSize (480, 360);
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (panel);
    lo.dialogTitle = TRANS ("Versioni della sessione");
    lo.dialogBackgroundColour = theme::panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    lo.launchAsync();
}

void MainComponent::checkSessionDialog()
{
    auto issues = check::checkSession (*project);
    lastCheckClean = true;
    for (auto& i : issues) lastCheckClean = lastCheckClean && i.level == check::Issue::Info;
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* panel = new check::Panel (std::move (issues),
        [safe] (const check::Issue& i)
        {
            if (safe == nullptr) return;
            if (i.cueId > 0) { safe->goToAdrCue (i.cueId); safe->showAdrWindow(); }
            else if (i.trackId > 0) { safe->project->selectedTrackId = i.trackId; safe->project->sendChangeMessage(); }
        },
        [safe] { if (safe != nullptr) safe->relinkMedia(); });
    panel->setSize (640, 420);
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (panel);
    lo.dialogTitle = TRANS ("Controllo della sessione");
    lo.dialogBackgroundColour = theme::panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = true;
    lo.launchAsync();
}

void MainComponent::workflowDialog()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* panel = new workflow::Panel (workflow::steps (*project, lastCheckClean, deliveredThisSession), [safe] (int a)
    {
        if (safe == nullptr) return;
        switch (a)
        {
            case workflow::ActImportVideo:   safe->importVideoDialog(); break;
            case workflow::ActImportScript:  safe->importAdrDialog(); break;
            case workflow::ActTracks:
            {
                const int n = dub::createCharacterTracks (*safe->project, safe->project->getSession().cues);
                safe->showError (n > 0 ? juce::String (n) + TRANS (" tracce Dialoghi create, una per personaggio.") : TRANS ("Non c'e' nulla da creare: servono dei cue con i personaggi, oppure le tracce esistono gia'."));
                break;
            }
            case workflow::ActRecord: case workflow::ActApprove: safe->showAdrWindow(); break;
            case workflow::ActCheck:         safe->checkSessionDialog(); break;
            case workflow::ActDeliver:       safe->exportDialog(); break;
            default: break;
        }
    });
    panel->setSize (560, 14 + 7 * 52 + 14);
    juce::DialogWindow::LaunchOptions lo;
    lo.content.setOwned (panel);
    lo.dialogTitle = TRANS ("Flusso di lavoro del doppiaggio");
    lo.dialogBackgroundColour = theme::panel;
    lo.componentToCentreAround = this;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    lo.launchAsync();
}

void MainComponent::toggleScriptWindow()
{
    if (scriptWindow != nullptr) { scriptWindow.reset(); commandManager.commandStatusChanged(); return; }
    juce::Component::SafePointer<MainComponent> safe (this);
    scriptWindow = std::make_unique<BoothWindow> (TRANS ("Lettore copione - ADR Dub Studio"), [safe]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->scriptWindow.reset(); safe->commandManager.commandStatusChanged(); } });
    });
    scriptWindow->setContentOwned (new ScriptComponent (*project, engine), false);
    scriptWindow->setResizeLimits (480, 320, 4000, 3000);
    scriptWindow->addKeyListener (commandManager.getKeyMappings());
    scriptWindow->setSize (900, 520);
    scriptWindow->centreAroundComponent (this, 900, 520);
    scriptWindow->setVisible (true);
    commandManager.commandStatusChanged();
}

void MainComponent::toggleCueWindow()
{
    if (cueWindow != nullptr) { cueWindow.reset(); commandManager.commandStatusChanged(); return; }
    juce::Component::SafePointer<MainComponent> safe (this);
    cueWindow = std::make_unique<BoothWindow> (TRANS ("Cuffie dell'attore - ADR Dub Studio"), [safe]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->cueWindow.reset(); safe->commandManager.commandStatusChanged(); } });
    });
    cueWindow->setContentOwned (new CueMixPanel (engine), false);
    cueWindow->setResizable (false, false);
    cueWindow->setSize (520, 560);
    cueWindow->centreAroundComponent (this, 520, 560);
    cueWindow->setVisible (true);
    commandManager.commandStatusChanged();
}

void MainComponent::alignSelectedToGuide()
{
    bool haveGuide = false;
    for (auto& t : project->getSession().tracks) haveGuide = haveGuide || (t.live->trackType.load() == TrackGuide && ! t.clips.empty());
    if (! haveGuide) { showError (TRANS ("Serve una traccia Guida con il dialogo originale.\n\nImposta il tipo della traccia su Guida (clic destro sulla traccia, Tipo di traccia) e mettici l'audio di riferimento.")); return; }

    std::vector<int64_t> ids (project->selection.begin(), project->selection.end());
    if (ids.empty())   // nessun clip selezionato: il take attivo del cue selezionato
        if (const auto* cue = project->findCue (project->selectedCueId))
            for (auto& t : project->getSession().tracks)
                for (auto& c : t.clips) if (c.cueId == cue->id && c.takeNo == cue->activeTake) ids.push_back (c.id);
    if (ids.empty()) { showError (TRANS ("Seleziona i clip da allineare (o un cue ADR con un take attivo).")); return; }

    auto* aw = new juce::AlertWindow (TRANS ("Allinea alla guida"), TRANS ("Sposta i clip selezionati finche' il parlato coincide con quello della traccia Guida (si confronta l'andamento del volume, non le parole)."),
                                      juce::MessageBoxIconType::NoIcon);
    aw->addComboBox ("mode", { TRANS ("Solo spostamento (consigliato)"), TRANS ("Spostamento e adatta la durata del parlato") }, TRANS ("Modo"));
    aw->addTextEditor ("range", "1500", TRANS ("Spostamento massimo (millisecondi)"));
    aw->addButton (TRANS ("Allinea"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, ids] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        const bool adapt = aw->getComboBoxComponent ("mode")->getSelectedItemIndex() == 1;
        const double range = juce::jlimit (50.0, 5000.0, aw->getTextEditorContents ("range").replaceCharacter (',', '.').getDoubleValue()) / 1000.0;
        auto& p = *safe->project;
        const auto items = align::alignToGuide (p, ids, range, 0.55, adapt);

        const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ADR Dub Studio").getChildFile ("Elaborati");
        auto report = [safe] (juce::String err) { if (safe != nullptr && err.isNotEmpty()) safe->showError (err); };
        int done = 0;
        juce::String msg;
        for (auto& it : items)
        {
            const auto* c = p.findClip (it.clipId);
            const juce::String name = c != nullptr && c->asset != nullptr ? c->asset->name : juce::String ("clip");
            if (it.applied) ++done;
            msg << name << ": " << (it.applied ? (juce::String (it.shiftMs >= 0 ? "+" : "") + juce::String (it.shiftMs, 0) + " ms") : juce::String (TRANS ("non spostato")))
                << " (" << it.note << (it.applied ? TRANS (", somiglianza ") + juce::String (it.confidence, 2) : juce::String()) << ")\n";
            if (it.applied && it.stretchRatio != 1.0) p.stretchClip (it.clipId, it.stretchRatio, true, folder, report, it.stretchStart);
        }
        safe->timeline->repaint();
        juce::AlertWindow::showMessageBoxAsync (done > 0 ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon, TRANS ("Allinea alla guida"),
                                                juce::String (done) + TRANS (" di ") + juce::String ((int) items.size()) + TRANS (" clip allineati.\n\n") + msg
                                                    + (done > 0 ? TRANS ("\nSe il risultato non ti convince, annulla con Modifica > Annulla.") : juce::String()));
    }), true);
}

void MainComponent::autoDuckDialog()
{
    bool haveDialogue = false;
    for (auto& t : project->getSession().tracks) haveDialogue = haveDialogue || (t.live->trackType.load() == TrackDialogue && ! t.clips.empty());
    if (! haveDialogue) { showError (TRANS ("Serve almeno una traccia Dialoghi con dei clip.\n\nImposta il tipo delle tracce con i dialoghi su Dialoghi (clic destro sulla traccia, Tipo di traccia).")); return; }

    auto* aw = new juce::AlertWindow (TRANS ("Ducking automatico"), TRANS ("Abbassa M&E ed Effetti quando parlano i Dialoghi, scrivendo l'automazione del volume (annullabile)."), juce::MessageBoxIconType::NoIcon);
    duck::Settings d;
    aw->addTextEditor ("depth", juce::String (d.depthDb, 0), TRANS ("Quanto abbassare il fondo (dB)"));
    aw->addTextEditor ("thr", juce::String (d.thresholdDb, 0), TRANS ("Soglia del dialogo (dBFS: sotto e' silenzio)"));
    aw->addTextEditor ("atk", juce::String ((int) (d.attackSec * 1000.0)), TRANS ("Discesa (ms)"));
    aw->addTextEditor ("rel", juce::String ((int) (d.releaseSec * 1000.0)), TRANS ("Risalita (ms)"));
    aw->addButton (TRANS ("Applica"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        auto num = [aw] (const char* n) { return aw->getTextEditorContents (n).replaceCharacter (',', '.').getDoubleValue(); };
        duck::Settings st;
        st.depthDb = juce::jlimit (1.0, 40.0, std::abs (num ("depth")));
        st.thresholdDb = juce::jlimit (-80.0, -10.0, num ("thr"));
        st.attackSec = juce::jlimit (0.02, 3.0, num ("atk") / 1000.0);
        st.releaseSec = juce::jlimit (0.05, 5.0, num ("rel") / 1000.0);
        const int n = duck::apply (*safe->project, st, safe->project->selectedTrackId);
        safe->timeline->repaint();
        juce::AlertWindow::showMessageBoxAsync (n > 0 ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon, TRANS ("Ducking automatico"),
            n > 0 ? TRANS ("Automazione del volume scritta su ") + juce::String (n) + (n == 1 ? " traccia" : " tracce") + TRANS (".\nLe tracce sono in modalita' lettura: la curva si vede e si modifica nella corsia VOL.\n\nPer togliere tutto: Modifica > Annulla.")
                  : TRANS ("Non ho trovato parlato sopra la soglia, oppure non ci sono tracce M&E / Effetti (o una traccia selezionata) su cui scrivere."));
    }), true);
}

void MainComponent::detectVideoCuts()
{
    auto video = project->getSession().video;
    if (video == nullptr) { showError (TRANS ("Importa prima un video.")); return; }
    if (videoJob != nullptr || exportJob != nullptr) return;

    auto* aw = new juce::AlertWindow (TRANS ("Rileva i tagli del video"), TRANS ("Cerca i cambi di inquadratura e mette un marker su ciascuno. Con un film lungo puo' volerci qualche minuto."), juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("sens", "5", TRANS ("Sensibilita' (da 1 a 10: piu' alta = piu' tagli, anche dissolvenze veloci)"));
    aw->addButton (TRANS ("Analizza"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (TRANS ("Annulla"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, video] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        const double sens = (juce::jlimit (1.0, 10.0, aw->getTextEditorContents ("sens").replaceCharacter (',', '.').getDoubleValue()) - 1.0) / 9.0;
        safe->cutJob = std::make_unique<CutJob> (video->file, sens, [safe, video] (bool ok, std::vector<double> cuts, juce::String err)
        {
            juce::MessageManager::callAsync ([safe, video, ok, cuts, err]
            {
                if (safe == nullptr) return;
                safe->cutJob.reset();
                if (! ok) { if (err != "Analisi annullata.") safe->showError (err); return; }
                const double rate = safe->project->getSampleRate();
                std::vector<std::pair<int64_t, juce::String>> marks;
                int n = 0;
                for (auto t : cuts) marks.emplace_back ((int64_t) std::llround ((t + video->offsetSec) * rate), "Taglio " + juce::String (++n));
                safe->project->addMarkers (marks, juce::Colour (0xff7aa7c7));
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Tagli del video"),
                    cuts.empty() ? juce::String (TRANS ("Non ho trovato tagli. Prova ad aumentare la sensibilita'.")) : TRANS ("Ho aggiunto ") + juce::String ((int) cuts.size()) + TRANS (" marker sui cambi di inquadratura.\n\nSi vedono nel righello e nel menu Marker; se sono troppi o troppo pochi, annulla e cambia la sensibilita'."));
            });
        });
        safe->cutJob->launchThread();
    }), true);
}

void MainComponent::exportCueSheet()
{
    const auto base = (project->getFile() == juce::File() ? juce::String ("ADR") : project->getFile().getFileNameWithoutExtension()) + "_foglio_cue";
    chooser = std::make_unique<juce::FileChooser> ("Salva il foglio cue (si apre nel browser: stampa o salva come PDF)",
                                                   juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (base).withFileExtension ("html"), "*.html");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, base] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        f = f.withFileExtension ("html");
        if (! f.replaceWithText (cuesheet::buildHtml (*safe->project, TRANS ("Foglio cue - ") + base.replace ("_foglio_cue", "")))) { safe->showError (TRANS ("Impossibile scrivere il file.")); return; }
        juce::URL (f).launchInDefaultBrowser();
    });
}

void MainComponent::newFromTemplate (int index)
{
    const auto& list = dub::templates();
    if (index < 0 || index >= (int) list.size()) return;
    juce::Component::SafePointer<MainComponent> safe (this);
    confirmDiscard ([safe, index]
    {
        if (safe == nullptr) return;
        safe->engine.stop();
        safe->engine.setPosition (0);
        dub::applyTemplate (*safe->project, dub::templates()[(size_t) index]);
        safe->pendingFit = true;
        safe->layoutAll();
    });
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
        juce::String csv = TRANS ("Cue;Stato;Personaggio;Scena;Rullo;TC In;TC Out;Durata (s);Take attivo;Take totali;Dialogo;Note\n");
        for (auto& c : p.getSession().cues)
            csv << c.number << ";" << adrStatusName (c.status) << ";" << csvField (c.character) << ";" << csvField (c.scene) << ";" << csvField (c.reel) << ";"
                << p.formatTc ((double) c.inPos / rate) << ";" << p.formatTc ((double) c.outPos / rate) << ";" << juce::String ((double) (c.outPos - c.inPos) / rate, 2) << ";"
                << c.activeTake << ";" << p.takeCount (c.id) << ";" << csvField (c.dialogue) << ";" << csvField (c.notes) << "\n";
        if (! f.withFileExtension ("csv").replaceWithText (csv)) safe->showError (TRANS ("Impossibile scrivere il file."));
    });
}

juce::String adrToProToolsText (const Project& p, const juce::String& sessionName)
{
    const double rate = p.getSampleRate();
    static const char* const tcNames[] = { "23.976 Frame", "24 Frame", "25 Frame", "29.97 Frame", "29.97 Drop Frame", "30 Frame", "50 Frame", "60 Frame" };
    juce::String t;
    t << TRANS ("SESSION NAME:\t") << sessionName << "\n";
    t << TRANS ("SAMPLE RATE:\t") << juce::String (rate, 6) << "\n";
    t << TRANS ("BIT DEPTH:\t24-bit\n");
    t << TRANS ("SESSION START TIMECODE:\t") << p.formatTc (0.0) << "\n";
    t << TRANS ("TIMECODE FORMAT:\t") << tcNames[juce::jlimit (0, 7, p.tcFormat)] << "\n\n\n";
    t << "M A R K E R S  L I S T I N G\n";
    t << TRANS ("#   \tLOCATION     \tTIME REFERENCE      \tUNITS    \tNAME                             \tCOMMENTS\n");
    for (auto& c : p.getSession().cues)
    {
        juce::String com = "CHARACTER: " + c.character + TRANS (" | SCENE: ") + c.scene + TRANS (" | OUT: ") + p.formatTc ((double) c.outPos / rate);
        if (c.reel.isNotEmpty()) com << TRANS (" | REEL: ") << c.reel;
        if (c.dialogue.isNotEmpty()) com << TRANS (" | DIALOGUE: ") << c.dialogue.replaceCharacters ("\r\n\t|", "    ");
        if (c.notes.isNotEmpty()) com << TRANS (" | NOTES: ") << c.notes.replaceCharacters ("\r\n\t|", "    ");
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
        if (! f.withFileExtension ("txt").replaceWithText (adrToProToolsText (*safe->project, name))) safe->showError (TRANS ("Impossibile scrivere il file."));
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

                if (c.asset->offline) { errors.add (TRANS ("Il file di ") + c.asset->name + TRANS (" non e' stato trovato: il take non si puo' esportare.")); continue; }
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
                                                                                       + TRANS (".\n\nOgni file porta il timecode della posizione sulla timeline: in Pro Tools si importano con Spot e \"Timecode\" oppure \"Original Time Stamp\".");
        if (errors.size() > 0) msg << "\n\n" << errors.joinIntoString ("\n");
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Esporta take ADR"), msg);
    });
}

} // namespace fc
