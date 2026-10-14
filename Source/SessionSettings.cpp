#include "SessionSettings.h"

namespace fc
{
using namespace theme;

SessionSettingsDialog::SessionSettingsDialog (const Project& p, std::function<void (const Values&)> onAccept)
    : project (p), accept (std::move (onAccept))
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (tcLbl, "FORMATO TIMECODE"); label (startLbl, "INIZIO SESSIONE (TC)"); label (preLbl, "PRE-ROLL (SECONDI)");
    label (postLbl, "POST-ROLL (SECONDI)"); label (beepLbl, "BIP PRIMA DELL'INGRESSO"); label (sizeLbl, "DIALOGO SUL VIDEO (PX)");

    for (int i = 0; i < numTcFormats; ++i) tcFormat.addItem (tcFormats()[i].name, i + 1);
    tcFormat.setSelectedId (project.tcFormat + 1, juce::dontSendNotification);
    for (int i = 0; i <= 4; ++i) beeps.addItem (i == 0 ? juce::String ("Nessuno") : juce::String (i), i + 1);
    beeps.setSelectedId (juce::jlimit (0, 4, project.adrBeeps) + 1, juce::dontSendNotification);

    start.setText (project.formatTc (0.0), false);
    start.setJustification (juce::Justification::centred);
    start.setInputRestrictions (11, "0123456789:;.");
    preRoll.setText (juce::String (project.adrPreRoll, 1), false);
    postRoll.setText (juce::String (project.adrPostRoll, 1), false);
    for (auto* t : { &preRoll, &postRoll }) { t->setInputRestrictions (5, "0123456789.,"); t->setJustification (juce::Justification::centred); }
    dialogueSize.setRange (14.0, 96.0, 1.0);
    dialogueSize.setValue (project.adrDialogueSize, juce::dontSendNotification);
    dialogueSize.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    dialogueSize.setTooltip ("Dimensione dei caratteri del dialogo sotto il video");
    rulerTc.setToggleState (project.rulerTimecode, juce::dontSendNotification);
    streamer.setToggleState (project.adrStreamer, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &tcFormat, &beeps, &start, &preRoll, &postRoll, &dialogueSize, &rulerTc, &streamer, &okBtn, &cancelBtn, &hint })
        addAndMakeVisible (*c);

    hint.setFont (font (11.5f));
    hint.setColour (juce::Label::textColourId, dim);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setText ("L'inizio sessione e' il timecode della posizione zero della timeline: nelle sessioni Pro Tools di solito 01:00:00:00. "
                  "I cue importati da Pro Tools (timecode assoluto) vengono posizionati sottraendo questo valore.", juce::dontSendNotification);

    tcFormat.onChange = [this] { updateStartHint(); };
    okBtn.onClick = [this] { close (true); };
    cancelBtn.onClick = [this] { close (false); };
}

void SessionSettingsDialog::updateStartHint() {}

void SessionSettingsDialog::close (bool ok)
{
    Values v;
    v.tcFormat = tcFormat.getSelectedId() - 1;
    v.rulerTc = rulerTc.getToggleState();
    v.preRoll = juce::jlimit (0.0, 30.0, preRoll.getText().replaceCharacter (',', '.').getDoubleValue());
    v.postRoll = juce::jlimit (0.0, 30.0, postRoll.getText().replaceCharacter (',', '.').getDoubleValue());
    v.beeps = beeps.getSelectedId() - 1;
    v.streamer = streamer.getToggleState();
    v.dialogueSize = (int) dialogueSize.getValue();
    v.tcStart = project.tcStartSec;

    if (ok)
    {
        double secs = 0.0;
        if (! timecodeToSeconds (start.getText(), tcFormats()[juce::jlimit (0, numTcFormats - 1, v.tcFormat)], secs))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Timecode non valido",
                                                    "Scrivi l'inizio sessione come ore:minuti:secondi:fotogrammi, per esempio 01:00:00:00.");
            return;
        }
        v.tcStart = secs;
    }
    auto cb = accept;
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (ok ? 1 : 0);
    if (ok && cb) juce::MessageManager::callAsync ([cb, v] { cb (v); });
}

void SessionSettingsDialog::resized()
{
    auto r = getLocalBounds().reduced (18);
    auto row = [&r] (juce::Label& l, juce::Component& c)
    {
        auto line = r.removeFromTop (28);
        l.setBounds (line.removeFromLeft (200));
        c.setBounds (line);
        r.removeFromTop (8);
    };
    row (tcLbl, tcFormat);
    row (startLbl, start);
    rulerTc.setBounds (r.removeFromTop (24));
    r.removeFromTop (10);
    row (preLbl, preRoll);
    row (postLbl, postRoll);
    row (beepLbl, beeps);
    row (sizeLbl, dialogueSize);
    streamer.setBounds (r.removeFromTop (24));
    r.removeFromTop (6);
    auto bottom = r.removeFromBottom (34);
    hint.setBounds (r);
    cancelBtn.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (8);
    okBtn.setBounds (bottom.removeFromRight (100));
}

} // namespace fc
