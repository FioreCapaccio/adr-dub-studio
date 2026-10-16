#include "AdrPanel.h"

namespace fc
{
using namespace theme;

namespace
{
juce::Colour statusColour (int s)
{
    switch (s)
    {
        case 1:  return blue;
        case 2:  return green;
        case 3:  return accent;
        default: return dim;
    }
}
} // namespace

//==============================================================================
AdrImportDialog::AdrImportDialog (const Project& p, const adr::ImportResult& r, std::function<void (const Values&)> onAccept)
    : project (p), accept (std::move (onAccept)), result (r)
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (tcLbl, TRANS ("FORMATO TIMECODE")); label (startLbl, TRANS ("INIZIO SESSIONE (TC)")); label (durLbl, TRANS ("DURATA SE MANCA L'USCITA (S)"));

    juce::String txt = TRANS ("Formato riconosciuto: ") + result.format + TRANS ("\nCue trovati: ") + juce::String ((int) result.cues.size());
    if (result.sessionStartTc.isNotEmpty()) txt << TRANS ("\nInizio sessione dichiarato dal file: ") + result.sessionStartTc;
    txt << TRANS ("\n\nI timecode del file sono assoluti: la posizione sulla timeline si ottiene sottraendo l'inizio sessione.");
    info.setText (txt, juce::dontSendNotification);
    info.setFont (font (12.5f));
    info.setColour (juce::Label::textColourId, text);
    info.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (info);

    for (int i = 0; i < numTcFormats; ++i) tcFormat.addItem (tcFormats()[i].name, i + 1);
    const int fmt = adr::suggestedTcFormat (result, project.tcFormat);
    tcFormat.setSelectedId (fmt + 1, juce::dontSendNotification);
    if (result.tcFormatIndex < 0 && fmt != project.tcFormat)
    {
        info.setText (info.getText() + TRANS ("\n\nNel file i fotogrammi arrivano fino a ") + juce::String (result.maxFrameField) + TRANS (": non sono compatibili con ")
                      + tcFormats()[project.tcFormat].name + TRANS (". Ho scelto ") + tcFormats()[fmt].name + TRANS ("; cambia il formato qui sotto se non e' quello giusto."),
                      juce::dontSendNotification);
    }
    start.setText (result.sessionStartTc.isNotEmpty() ? result.sessionStartTc : project.formatTc (0.0), false);
    start.setJustification (juce::Justification::centred);
    start.setInputRestrictions (11, "0123456789:;.");
    duration.setText ("4.0", false);
    duration.setInputRestrictions (5, "0123456789.,");
    duration.setJustification (juce::Justification::centred);
    replace.setToggleState (project.getSession().cues.empty(), juce::dontSendNotification);
    markers.setToggleState (false, juce::dontSendNotification);
    {
        std::set<juce::String> names;
        for (auto& c : result.cues) if (c.character.trim().isNotEmpty()) names.insert (c.character.trim().toLowerCase());
        characterTracks.setToggleState (names.size() >= 2, juce::dontSendNotification);   // utile quando ci sono piu' personaggi
    }

    for (auto* c : std::initializer_list<juce::Component*> { &tcFormat, &start, &duration, &replace, &markers, &characterTracks, &okBtn, &cancelBtn })
        addAndMakeVisible (*c);
    okBtn.onClick = [this] { close (true); };
    cancelBtn.onClick = [this] { close (false); };
}

void AdrImportDialog::close (bool ok)
{
    Values v;
    v.tcFormat = tcFormat.getSelectedId() - 1;
    v.defaultDuration = juce::jlimit (0.5, 60.0, duration.getText().replaceCharacter (',', '.').getDoubleValue());
    v.replace = replace.getToggleState();
    v.makeMarkers = markers.getToggleState();
    v.makeTracks = characterTracks.getToggleState();
    v.tcStart = project.tcStartSec;
    if (ok)
    {
        double s = 0.0;
        if (! timecodeToSeconds (start.getText(), tcFormats()[juce::jlimit (0, numTcFormats - 1, v.tcFormat)], s))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Timecode non valido"),
                                                    TRANS ("Scrivi l'inizio sessione come ore:minuti:secondi:fotogrammi, per esempio 01:00:00:00."));
            return;
        }
        v.tcStart = s;
    }
    auto cb = accept;
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (ok ? 1 : 0);
    if (ok && cb) juce::MessageManager::callAsync ([cb, v] { cb (v); });
}

void AdrImportDialog::resized()
{
    auto r = getLocalBounds().reduced (18);
    info.setBounds (r.removeFromTop (150));
    auto row = [&r] (juce::Label& l, juce::Component& c)
    {
        auto line = r.removeFromTop (28);
        l.setBounds (line.removeFromLeft (230));
        c.setBounds (line);
        r.removeFromTop (8);
    };
    row (tcLbl, tcFormat);
    row (startLbl, start);
    row (durLbl, duration);
    replace.setBounds (r.removeFromTop (24));
    markers.setBounds (r.removeFromTop (24));
    characterTracks.setBounds (r.removeFromTop (24));
    auto bottom = r.removeFromBottom (34);
    cancelBtn.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (8);
    okBtn.setBounds (bottom.removeFromRight (110));
}

//==============================================================================
AdrPanel::AdrPanel (Project& p, Actions a) : project (p), actions (std::move (a))
{
    table.setModel (this);
    table.setColour (juce::ListBox::backgroundColourId, lane);
    table.setColour (juce::ListBox::outlineColourId, border);
    table.setOutlineThickness (1);
    table.setRowHeight (26);
    auto& h = table.getHeader();
    h.addColumn ("#", 1, 44, 36, 80, juce::TableHeaderComponent::notResizableOrSortable);
    h.addColumn ("Stato", 2, 110, 80, 160, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Personaggio", 3, 140, 80, 300, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Scena", 4, 60, 40, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("In", 5, 100, 80, 160, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Out", 6, 100, 80, 160, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Durata", 7, 64, 50, 100, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Take", 8, 56, 40, 100, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Dialogo", 9, 300, 120, 2000, juce::TableHeaderComponent::notSortable);
    addAndMakeVisible (table);

    for (auto* b : { &importBtn, &addBtn, &delBtn, &settingsBtn, &csvBtn, &ptBtn, &bwfBtn, &recBtn, &prevBtn, &nextBtn, &auditionBtn, &originalBtn, &activateBtn, &deleteTakeBtn })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    recBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5a2420));
    recBtn.setColour (juce::TextButton::buttonOnColourId, accent);
    recBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffff8a7d));
    recBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);

    importBtn.setTooltip (TRANS ("Importa cue da Pro Tools (testo Session Info / marker), EDL o tabella CSV/TSV"));
    addBtn.setTooltip (TRANS ("Crea un cue dalla regione di loop (oppure 4 secondi dalla testina)"));
    csvBtn.setTooltip (TRANS ("Esporta l'elenco dei cue in CSV (cue sheet)"));
    ptBtn.setTooltip (TRANS ("Esporta l'elenco dei cue come testo di marker di Pro Tools"));
    bwfBtn.setTooltip (TRANS ("Esporta i take attivi come file BWF con timecode, pronti da posizionare in Pro Tools"));
    recBtn.setTooltip (TRANS ("Pre-roll, bip, streamer e punch automatico sul cue selezionato"));
    auditionBtn.setTooltip (TRANS ("Ascolta il take scelto da solo (con il video in sincrono). Un altro clic ferma l'ascolto"));
    activateBtn.setTooltip (TRANS ("Rende attivo il take scelto: e' quello che suona nel mix e che si esporta"));
    deleteTakeBtn.setTooltip (TRANS ("Elimina il take scelto (si puo' annullare)"));
    originalBtn.setTooltip (TRANS ("Ascolta la battuta originale del cue (l'audio di produzione, senza i take ADR) con il video in sincrono. Un altro clic ferma l'ascolto"));
    auditionBtn.onClick = [this]
    {
        if (actions.isAuditioning && actions.isAuditioning()) { if (actions.stopAudition) actions.stopAudition(); }
        else if (viewTake != 0 && actions.auditionTake) { auditionIsOriginal = false; actions.auditionTake (shownCueId, viewTake); }
    };
    originalBtn.onClick = [this]
    {
        if (actions.isAuditioning && actions.isAuditioning()) { if (actions.stopAudition) actions.stopAudition(); }
        else if (shownCueId != 0 && actions.auditionOriginal) { auditionIsOriginal = true; actions.auditionOriginal (shownCueId); }
    };
    activateBtn.onClick = [this] { if (viewTake != 0 && shownCueId != 0) project.setActiveTake (shownCueId, viewTake); };
    deleteTakeBtn.onClick = [this] { if (viewTake != 0 && shownCueId != 0) project.deleteTake (shownCueId, viewTake); };
    importBtn.onClick = [this] { if (actions.importFile) actions.importFile(); };
    addBtn.onClick = [this] { if (actions.addCue) actions.addCue(); };
    delBtn.onClick = [this] { if (project.selectedCueId != 0) project.removeCue (project.selectedCueId); };
    settingsBtn.onClick = [this] { if (actions.settings) actions.settings(); };
    csvBtn.onClick = [this] { if (actions.exportCsv) actions.exportCsv(); };
    ptBtn.onClick = [this] { if (actions.exportProTools) actions.exportProTools(); };
    bwfBtn.onClick = [this] { if (actions.exportBwf) actions.exportBwf(); };
    recBtn.onClick = [this]
    {
        if (actions.isRecording && actions.isRecording()) { if (actions.stopRecording) actions.stopRecording(); }
        else if (actions.recordCue && project.selectedCueId != 0) actions.recordCue (project.selectedCueId);
    };
    auto step = [this] (int d)
    {
        const auto& cues = project.getSession().cues;
        if (cues.empty()) return;
        int idx = -1;
        for (size_t i = 0; i < cues.size(); ++i) if (cues[i].id == project.selectedCueId) idx = (int) i;
        idx = juce::jlimit (0, (int) cues.size() - 1, idx + d);
        project.selectedCueId = cues[(size_t) idx].id;
        project.sendChangeMessage();
        if (actions.goToCue) actions.goToCue (project.selectedCueId);
    };
    prevBtn.onClick = [step] { step (-1); };
    nextBtn.onClick = [step] { step (1); };

    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (10.5f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (lblChar, TRANS ("PERSONAGGIO")); label (lblScene, TRANS ("SCENA")); label (lblReel, TRANS ("RULLO")); label (lblIn, TRANS ("IN (TC)")); label (lblOut, TRANS ("OUT (TC)"));
    label (lblStatus, TRANS ("STATO")); label (lblDialogue, TRANS ("DIALOGO")); label (lblNotes, TRANS ("NOTE")); label (lblTrack, TRANS ("REGISTRA SU")); label (lblTakes, TRANS ("TAKE"));

    for (auto* e : { &edChar, &edScene, &edReel, &edIn, &edOut, &edDialogue, &edNotes })
    {
        e->setColour (juce::TextEditor::backgroundColourId, bg);
        e->onReturnKey = [this] { commitDetails(); };
        e->onFocusLost = [this] { commitDetails(); };
        addAndMakeVisible (*e);
    }
    for (auto* e : { &edIn, &edOut }) { e->setInputRestrictions (11, "0123456789:;."); e->setFont (mono (13.0f)); }
    edDialogue.setMultiLine (true, true);
    edDialogue.setReturnKeyStartsNewLine (false);
    edDialogue.setFont (font (14.0f));
    edNotes.setMultiLine (true, true);
    edNotes.setReturnKeyStartsNewLine (false);

    for (int i = 0; i < 4; ++i) cbStatus.addItem (adrStatusName (i), i + 1);
    cbStatus.onChange = [this] { commitDetails(); };
    addAndMakeVisible (cbStatus);
    cbTrack.onChange = [this]
    {
        const int idx = cbTrack.getSelectedId() - 2;
        const auto& tracks = project.getSession().tracks;
        project.adrTrackId = idx >= 0 && idx < (int) tracks.size() ? tracks[(size_t) idx].id : -1;
    };
    addAndMakeVisible (cbTrack);

    project.addChangeListener (this);
    refresh();
}

AdrPanel::~AdrPanel() { project.removeChangeListener (this); }

const AdrCue* AdrPanel::cueAtRow (int row) const
{
    const auto& c = project.getSession().cues;
    return juce::isPositiveAndBelow (row, (int) c.size()) ? &c[(size_t) row] : nullptr;
}

int AdrPanel::selectedRow() const
{
    const auto& c = project.getSession().cues;
    for (size_t i = 0; i < c.size(); ++i) if (c[i].id == project.selectedCueId) return (int) i;
    return -1;
}

void AdrPanel::refresh()
{
    loading = true;
    table.updateContent();
    const int row = selectedRow();
    if (row >= 0) table.selectRow (row, false, true); else table.deselectAllRows();
    loading = false;

    // elenco delle tracce
    cbTrack.clear (juce::dontSendNotification);
    cbTrack.addItem (TRANS ("Traccia selezionata"), 1);
    int sel = 1;
    const auto& tracks = project.getSession().tracks;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        cbTrack.addItem (juce::String ((int) i + 1) + "  " + tracks[i].name, (int) i + 2);
        if (tracks[i].id == project.adrTrackId) sel = (int) i + 2;
    }
    cbTrack.setSelectedId (sel, juce::dontSendNotification);

    loadDetails();
    table.repaint();
    repaint();
}

void AdrPanel::loadDetails()
{
    const auto* c = project.findCue (project.selectedCueId);
    loading = true;
    shownCueId = c != nullptr ? c->id : 0;
    const double rate = project.getSampleRate();
    for (auto* e : { &edChar, &edScene, &edReel, &edIn, &edOut, &edDialogue, &edNotes }) e->setEnabled (c != nullptr);
    cbStatus.setEnabled (c != nullptr);
    if (c != nullptr)
    {
        auto setIf = [] (juce::TextEditor& e, const juce::String& v) { if (e.getText() != v && ! e.hasKeyboardFocus (true)) e.setText (v, false); };
        setIf (edChar, c->character); setIf (edScene, c->scene); setIf (edReel, c->reel);
        setIf (edIn, project.formatTc ((double) c->inPos / rate)); setIf (edOut, project.formatTc ((double) c->outPos / rate));
        setIf (edDialogue, c->dialogue); setIf (edNotes, c->notes);
        cbStatus.setSelectedId (c->status + 1, juce::dontSendNotification);
    }
    else
        for (auto* e : { &edChar, &edScene, &edReel, &edIn, &edOut, &edDialogue, &edNotes }) e->setText ({}, false);
    loading = false;
    rebuildTakes();
}

void AdrPanel::rebuildTakes()
{
    takeButtons.clear();
    const auto* c = project.findCue (project.selectedCueId);
    std::set<int> numbers;
    if (c != nullptr)
        for (auto& t : project.getSession().tracks) for (auto& cl : t.clips) if (cl.cueId == c->id) numbers.insert (cl.takeNo);
    if (numbers.count (viewTake) == 0) viewTake = c != nullptr && numbers.count (c->activeTake) > 0 ? c->activeTake : (numbers.empty() ? 0 : *numbers.rbegin());

    if (c != nullptr)
        for (int n : numbers)
        {
            auto b = std::make_unique<juce::TextButton> ("T" + juce::String (n));
            const bool active = c->activeTake == n;
            b->setClickingTogglesState (false);
            b->setToggleState (n == viewTake, juce::dontSendNotification);
            b->setColour (juce::TextButton::textColourOffId, active ? green : text);
            b->setWantsKeyboardFocus (false);
            b->setTooltip (TRANS ("Take ") + juce::String (n) + (active ? TRANS (" (attivo, in verde). ") : ". ") + TRANS ("Clic: sceglie il take e lo ascolta"));
            const int cueId = c->id;
            b->onClick = [this, cueId, n]
            {
                viewTake = n;
                rebuildTakes();
                if (actions.auditionTake) actions.auditionTake (cueId, n);
            };
            addAndMakeVisible (*b);
            takeButtons.push_back (std::move (b));
        }

    const bool any = viewTake != 0;
    const bool playing = actions.isAuditioning && actions.isAuditioning();
    auditionBtn.setButtonText (playing && ! auditionIsOriginal ? TRANS ("STOP") : TRANS ("ASCOLTA"));
    auditionBtn.setEnabled (any && ! (playing && auditionIsOriginal));
    originalBtn.setButtonText (playing && auditionIsOriginal ? TRANS ("STOP") : TRANS ("ORIGINALE"));
    originalBtn.setEnabled (c != nullptr && ! (playing && ! auditionIsOriginal));
    activateBtn.setEnabled (any && c != nullptr && c->activeTake != viewTake);
    deleteTakeBtn.setEnabled (any);
    resized();
}

void AdrPanel::commitDetails()
{
    if (loading || shownCueId == 0) return;
    const auto* c = project.findCue (shownCueId);
    if (c == nullptr) return;
    const double rate = project.getSampleRate();
    double tin = 0.0, tout = 0.0;
    const bool okIn = project.parseTc (edIn.getText(), tin) && tin >= 0.0;
    const bool okOut = project.parseTc (edOut.getText(), tout) && tout >= 0.0;
    const int64_t newIn = okIn ? (int64_t) std::llround (tin * rate) : c->inPos;
    int64_t newOut = okOut ? (int64_t) std::llround (tout * rate) : c->outPos;
    if (newOut <= newIn) newOut = newIn + (int64_t) rate;
    const int status = cbStatus.getSelectedId() - 1;

    const bool changed = c->character != edChar.getText() || c->scene != edScene.getText() || c->reel != edReel.getText()
                      || c->dialogue != edDialogue.getText() || c->notes != edNotes.getText() || c->inPos != newIn || c->outPos != newOut || c->status != status;
    if (! changed) return;
    const auto ch = edChar.getText(), sc = edScene.getText(), rl = edReel.getText(), di = edDialogue.getText(), no = edNotes.getText();
    project.editCue (shownCueId, [=] (AdrCue& cue)
    {
        cue.character = ch; cue.scene = sc; cue.reel = rl; cue.dialogue = di; cue.notes = no;
        cue.inPos = newIn; cue.outPos = newOut; cue.status = juce::jlimit (0, 3, status);
    });
}

//==============================================================================
void AdrPanel::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool selected)
{
    g.setColour (selected ? blue.withAlpha (0.35f) : (row % 2 == 0 ? lane : laneAlt));
    g.fillRect (0, 0, w, h);
}

namespace
{
/** Cella di testo della tabella: doppio clic per modificarla sul posto, Invio conferma, Esc annulla. */
class CueTextCell : public juce::Label
{
public:
    CueTextCell (juce::TableListBox& t, std::function<void (int row, int column, const juce::String&)> onEdit) : table (t), edited (std::move (onEdit))
    {
        setEditable (false, true, false);
        setInterceptsMouseClicks (true, false);
        onTextChange = [this] { if (edited) edited (row, column, getText()); };
    }
    void setCell (int r, int c, const juce::String& txt, juce::Colour col, bool sel)
    {
        row = r; column = c;
        setFont (c == 3 ? theme::font (13.0f, true) : theme::font (13.0f));
        setColour (juce::Label::textColourId, sel ? juce::Colours::white : col);
        setColour (juce::Label::backgroundWhenEditingColourId, bg);
        setColour (juce::Label::textWhenEditingColourId, theme::text);
        setBorderSize ({ 0, 8, 0, 4 });
        if (! isBeingEdited() && getText() != txt) setText (txt, juce::dontSendNotification);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        table.selectRowsBasedOnModifierKeys (row, e.mods, false);   // un clic seleziona la riga come sulle altre colonne
        juce::Label::mouseDown (e);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override { showEditor(); (void) e; }
    void editorShown (juce::TextEditor* ed) override { ed->setInputRestrictions (0); }

private:
    juce::TableListBox& table;
    std::function<void (int, int, const juce::String&)> edited;
    int row = 0, column = 0;
};
} // namespace

juce::Component* AdrPanel::refreshComponentForCell (int row, int columnId, bool isRowSelected, juce::Component* existing)
{
    const auto* c = cueAtRow (row);
    if ((columnId != 3 && columnId != 4 && columnId != 9) || c == nullptr)
    {
        delete existing;
        return nullptr;
    }
    auto* cell = dynamic_cast<CueTextCell*> (existing);
    if (cell == nullptr)
    {
        delete existing;
        cell = new CueTextCell (table, [this] (int r, int col, const juce::String& v)
        {
            if (const auto* cue = cueAtRow (r))
                project.editCue (cue->id, [col, v] (AdrCue& a)
                {
                    if (col == 3) a.character = v; else if (col == 4) a.scene = v; else a.dialogue = v;
                });
        });
    }
    cell->setTooltip (TRANS ("Doppio clic per modificare. Invio conferma, Esc annulla"));
    cell->setCell (row, columnId, columnId == 3 ? c->character : columnId == 4 ? c->scene : c->dialogue.replaceCharacters ("\r\n", "  "),
                   columnId == 9 ? dim.brighter (0.3f) : text, isRowSelected);
    return cell;
}

void AdrPanel::paintCell (juce::Graphics& g, int row, int columnId, int w, int h, bool selected)
{
    const auto* c = cueAtRow (row);
    if (c == nullptr || columnId == 3 || columnId == 4 || columnId == 9) return;   // queste colonne sono celle modificabili
    const double rate = project.getSampleRate();
    juce::String t;
    juce::Colour col = text;
    bool mono_ = false;
    switch (columnId)
    {
        case 1: t = juce::String (c->number); col = dim; break;
        case 2: t = adrStatusName (c->status); col = statusColour (c->status); break;
        case 3: t = c->character; break;
        case 4: t = c->scene; break;
        case 5: t = project.formatTc ((double) c->inPos / rate); mono_ = true; break;
        case 6: t = project.formatTc ((double) c->outPos / rate); mono_ = true; break;
        case 7: t = juce::String ((double) (c->outPos - c->inPos) / rate, 1) + " s"; col = dim; break;
        case 8: { const int n = project.takeCount (c->id); t = n == 0 ? juce::String ("-") : juce::String (c->activeTake) + "/" + juce::String (n); col = n == 0 ? dim : text; break; }
        case 9: t = c->dialogue.replaceCharacters ("\r\n", "  "); col = dim.brighter (0.3f); break;
        default: break;
    }
    if (selected && columnId != 2) col = juce::Colours::white;
    g.setColour (col);
    g.setFont (mono_ ? mono (12.5f) : font (13.0f, columnId == 3));
    g.drawText (t, 8, 0, w - 12, h, juce::Justification::centredLeft, true);
}

void AdrPanel::selectedRowsChanged (int lastRow)
{
    if (loading) return;
    if (const auto* c = cueAtRow (lastRow))
    {
        project.selectedCueId = c->id;
        project.sendChangeMessage();
        if (actions.goToCue) actions.goToCue (c->id);
    }
}

void AdrPanel::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (const auto* c = cueAtRow (row))
    {
        project.selectedCueId = c->id;
        if (actions.recordCue && ! (actions.isRecording && actions.isRecording())) actions.recordCue (c->id);
    }
}

//==============================================================================
void AdrPanel::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (panel);
    g.fillRect (detailsArea);
    g.setColour (border);
    g.drawHorizontalLine (detailsArea.getY(), 0.0f, (float) getWidth());
}

void AdrPanel::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto bar = r.removeFromTop (34);
    auto place = [&bar] (juce::Component& c, int w) { c.setBounds (bar.removeFromLeft (w)); bar.removeFromLeft (6); };
    place (importBtn, 100); place (addBtn, 72); place (delBtn, 80); place (settingsBtn, 118);
    place (csvBtn, 110); place (ptBtn, 150); place (bwfBtn, 160);
    r.removeFromTop (8);

    // area dei dettagli in basso
    detailsArea = juce::Rectangle<int> (0, getHeight() - 278, getWidth(), 278);
    auto d = detailsArea.reduced (10, 10);
    auto row1 = d.removeFromTop (44);
    auto cell = [&row1] (juce::Label& l, juce::Component& c, int w)
    {
        auto col = row1.removeFromLeft (w);
        l.setBounds (col.removeFromTop (14));
        c.setBounds (col);
        row1.removeFromLeft (8);
    };
    cell (lblChar, edChar, 180); cell (lblScene, edScene, 70); cell (lblReel, edReel, 70);
    cell (lblIn, edIn, 120); cell (lblOut, edOut, 120); cell (lblStatus, cbStatus, 140);
    d.removeFromTop (8);
    auto row2 = d.removeFromTop (74);
    auto dial = row2.removeFromLeft (row2.getWidth() * 2 / 3 - 4);
    lblDialogue.setBounds (dial.removeFromTop (14));
    edDialogue.setBounds (dial);
    row2.removeFromLeft (8);
    lblNotes.setBounds (row2.removeFromTop (14));
    edNotes.setBounds (row2);
    d.removeFromTop (8);

    auto takesRow = d.removeFromTop (30);
    lblTakes.setBounds (takesRow.removeFromLeft (44).withTrimmedTop (8));
    for (auto& b : takeButtons) { b->setBounds (takesRow.removeFromLeft (46)); takesRow.removeFromLeft (4); }
    deleteTakeBtn.setBounds (takesRow.removeFromRight (110));
    takesRow.removeFromRight (6);
    activateBtn.setBounds (takesRow.removeFromRight (80));
    takesRow.removeFromRight (6);
    auditionBtn.setBounds (takesRow.removeFromRight (90));
    takesRow.removeFromRight (6);
    originalBtn.setBounds (takesRow.removeFromRight (100));
    d.removeFromTop (6);

    auto row3 = d.removeFromTop (34);
    recBtn.setBounds (row3.removeFromRight (150));
    row3.removeFromRight (8);
    nextBtn.setBounds (row3.removeFromRight (70));
    row3.removeFromRight (6);
    prevBtn.setBounds (row3.removeFromRight (70));
    row3.removeFromRight (14);
    cbTrack.setBounds (row3.removeFromRight (190));
    lblTrack.setBounds (row3.removeFromRight (90));

    table.setBounds (r.withTrimmedBottom (getHeight() - detailsArea.getY() - 10 + 10).withHeight (detailsArea.getY() - r.getY() - 8));
}

} // namespace fc
