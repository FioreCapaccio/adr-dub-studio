#pragma once

#include "AdrPanel.h"
#include "AudioEngine.h"
#include "EditBar.h"
#include "ExportEngine.h"
#include "MixerView.h"
#include "TimelineView.h"
#include "VideoViewer.h"

namespace fc
{
/** Maniglia orizzontale per ridimensionare un pannello trascinando. */
class PanelResizer : public juce::Component
{
public:
    std::function<void (int)> onDrag;      // spostamento verticale dall'inizio del trascinamento
    std::function<void()> onStart;
    PanelResizer() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg);
        g.setColour (hover ? theme::blue : theme::border);
        g.fillRoundedRectangle (getLocalBounds().toFloat().withSizeKeepingCentre (48.0f, 3.0f), 1.5f);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { if (onStart) onStart(); }
    void mouseDrag (const juce::MouseEvent& e) override { if (onDrag) onDrag (e.getDistanceFromDragStartY()); }

private:
    bool hover = false;
};

/** Testo dei cue ADR nel formato dei marker di Pro Tools (rileggibile da adr::parse) e take attivi come BWF con timecode. */
juce::String adrToProToolsText (const Project&, const juce::String& sessionName);
int exportAdrTakes (const Project&, const juce::File& folder, juce::StringArray& errors);

class ExportJob;
class VideoExportJob;

class MainComponent : public juce::Component,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      public juce::ChangeListener,
                      public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    std::function<void (const juce::String&)> onTitleChanged;

    void requestQuit();
    void importFromCommandLine (const juce::StringArray& paths);

    // Component
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // barra di avanzamento dei caricamenti lunghi
    void resized() override;

    // ApplicationCommandTarget
    ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int id, int menuIndex) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    juce::ApplicationCommandManager commandManager;

private:
    void confirmDiscard (std::function<void()> proceed);
    void importDialog();
    void importVideoDialog();
    void loadVideo (const juce::File&);
    void exportDialog();
    void exportMarkersCsv();
    void showAdrWindow();
    void importAdrDialog();
    void addAdrCue();
    void goToAdrCue (int cueId);
    void recordAdrCue (int cueId);
    void exportAdrCsv();
    void exportAdrProTools();
    void exportAdrBwf();
    void updateAdrViewer();
    void auditionTake (int cueId, int takeNo);
    void auditionOriginal (int cueId);
    void endAudition();
    void timeStretchSelected (bool toCue);
    void showShortcuts();
    void showSessionSettings();
    void collectMedia();
    void addRecent (const juce::File&);
    void openFile (const juce::File&);
    void autosaveTick();
    void checkRecovery();
    void runExport (const ExportOptions&);
    void exportVideoDialog();
    void saveAs();
    void saveCurrent();
    void openDialog();
    void audioSettings();
    void toggleTransport();
    void stopAll();
    void toggleLoop();
    void setLoopFromSelection();
    void startRecording();
    void finishRecording();
    void armSelectedTrack();
    void setViewerDetached (bool detach);
    void toggleVideoFullscreen();
    void updateTitle();
    void syncToggles();
    void layoutAll();
    void showError (const juce::String& msg);
    double fps() const { return project->frameRate(); }

    juce::AudioFormatManager formats;
    AudioEngine engine;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<Project> project;
    std::unique_ptr<TimelineView> timeline;
    std::unique_ptr<EditBar> editBar;
    PanelResizer mixerResizer;
    int mixerHeightAtDrag = 0;
    std::unique_ptr<MixerView> mixer;
    VideoViewer viewer;
    LevelMeter inMeter;

    theme::IconButton rewindBtn { theme::IconButton::Icon::Rewind, theme::blue },
                      playBtn   { theme::IconButton::Icon::Play, theme::green },
                      stopBtn   { theme::IconButton::Icon::Stop, theme::blue },
                      recBtn    { theme::IconButton::Icon::Record, theme::accent };
    juce::TextButton exitBtn { "ESCI" };
    int64_t playStartPos = 0;          // dove e' partita l'ultima riproduzione
    bool returnToStart = true, startedFromPlay = false;   // allo stop la testina torna al punto di partenza
    void stopPlayback();
    juce::TextButton loopBtn   { "LOOP" }, snapBtn { "SNAP" }, followBtn { "SEGUI" },
                     monBtn    { "MON" },
                     zoomOutBtn { "-" }, zoomInBtn { "+" }, fitBtn { "ADATTA" },
                     importBtn { "IMPORTA" }, exportBtn { "ESPORTA" }, addTrackBtn { "+ TRACCIA" },
                     videoBtn { "VIDEO" }, mixerBtn { "MIXER" };

    static constexpr int toolbarH = 74;
    juce::Rectangle<int> timeBox, statusBox;
    std::vector<juce::Rectangle<int>> capsules;
    bool mixerVisible = true, viewerVisible = true;
    int mixerHeight = 420;
    bool mixerUserSized = false;
    bool pendingFit = true;   // adatta l'altezza delle tracce alla finestra al prossimo layout
    int64_t lastShownPos = -1;
    int lastTaskPct = -1;
    int infoTick = 0, secondTick = 0, secondsSinceAutosave = 0;
    std::unique_ptr<juce::Component> helpWindow;
    std::shared_ptr<VideoAsset> shownVideo;
    juce::File loadingVideo;
    std::unique_ptr<AdrWindow> adrWindow;
    AdrPanel* adrPanel = nullptr;
    int adrRecCue = 0;                // cue in registrazione (0 = nessuno)
    int64_t adrRecStop = 0;
    int64_t auditionStop = 0;         // posizione a cui l'ascolto di un take si ferma da solo           // posizione a cui la registrazione si ferma da sola
    juce::String adrViewerKey;
    std::unique_ptr<VideoWindow> videoWindow;   // dichiarata dopo 'viewer': viene distrutta prima
    juce::TooltipWindow tooltipWindow { this, 450 };

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<ExportJob> exportJob;
    std::unique_ptr<VideoExportJob> videoJob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace fc
