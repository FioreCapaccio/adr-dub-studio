#pragma once

#include "AdrImport.h"
#include "Model.h"
#include "Theme.h"

namespace fc
{
/** Dialogo di importazione dei cue ADR: formato rilevato, timecode, durata predefinita. */
class AdrImportDialog : public juce::Component
{
public:
    struct Values { int tcFormat; double tcStart; double defaultDuration; bool replace; bool makeMarkers; bool makeTracks; };

    AdrImportDialog (const Project&, const adr::ImportResult&, std::function<void (const Values&)> onAccept);
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;

private:
    void close (bool ok);

    const Project& project;
    std::function<void (const Values&)> accept;
    juce::Label info, tcLbl, startLbl, durLbl;
    juce::ComboBox tcFormat;
    juce::TextEditor start, duration;
    juce::ToggleButton replace { TRANS ("Sostituisci i cue esistenti") }, markers { TRANS ("Crea anche un marker per ogni cue (come le Memory Location)") },
                       characterTracks { TRANS ("Crea una traccia Dialoghi per ogni personaggio") };
    juce::TextButton okBtn { TRANS ("Importa") }, cancelBtn { "Annulla" };
    adr::ImportResult result;
};

/** Pannello ADR: elenco dei cue, dettagli, take e registrazione. */
class AdrPanel : public juce::Component,
                 private juce::TableListBoxModel,
                 private juce::ChangeListener
{
public:
    struct Actions
    {
        std::function<void()> importFile, addCue, exportCsv, exportProTools, exportBwf, settings, stopRecording, stopAudition;
        std::function<void (int)> recordCue, goToCue;
        std::function<void (int, int)> auditionTake;   // (cue, numero del take): ascolta solo quel take
        std::function<void (int)> auditionOriginal;    // ascolta l'originale del cue (tutto tranne i take ADR)
        std::function<bool()> isRecording, isAuditioning;
    };

    AdrPanel (Project&, Actions);
    ~AdrPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    // TableListBoxModel
    int getNumRows() override { return (int) project.getSession().cues.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int columnId, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int columnId, bool isRowSelected, juce::Component* existing) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void cellDoubleClicked (int row, int columnId, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    const AdrCue* cueAtRow (int row) const;
    int selectedRow() const;
    void loadDetails();
    void commitDetails();
    void rebuildTakes();

    Project& project;
    Actions actions;
    juce::TableListBox table { {}, this };
    juce::TextButton importBtn { TRANS ("IMPORTA...") }, addBtn { TRANS ("+ CUE") }, delBtn { "ELIMINA" }, settingsBtn { "IMPOSTAZIONI" },
                     csvBtn { TRANS ("ESPORTA CSV") }, ptBtn { TRANS ("ESPORTA PRO TOOLS") }, bwfBtn { TRANS ("ESPORTA TAKE (BWF)") },
                     recBtn { TRANS ("REGISTRA CUE") }, prevBtn { TRANS ("< CUE") }, nextBtn { TRANS ("CUE >") },
                     auditionBtn { "ASCOLTA" }, originalBtn { "ORIGINALE" }, activateBtn { "ATTIVA" }, deleteTakeBtn { TRANS ("ELIMINA TAKE") };
    juce::Label lblChar, lblScene, lblReel, lblIn, lblOut, lblStatus, lblDialogue, lblNotes, lblTrack, lblTakes;
    juce::TextEditor edChar, edScene, edReel, edIn, edOut, edDialogue, edNotes;
    juce::ComboBox cbStatus, cbTrack;
    std::vector<std::unique_ptr<juce::TextButton>> takeButtons;
    bool loading = false;
    int shownCueId = 0;
    int viewTake = 0;
    bool auditionIsOriginal = false;   // take scelto per l'ascolto (di norma quello attivo)
    juce::Rectangle<int> detailsArea;
};

/** Finestra che ospita il pannello ADR. */
class AdrWindow : public juce::DocumentWindow
{
public:
    explicit AdrWindow (std::function<void()> closeCallback)
        : juce::DocumentWindow (TRANS ("Cue ADR - ADR Dub Studio"), theme::bg, juce::DocumentWindow::allButtons), onClose (std::move (closeCallback))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setResizeLimits (860, 600, 4000, 3000);
    }
    void closeButtonPressed() override { if (onClose) onClose(); }

private:
    std::function<void()> onClose;
};

} // namespace fc
