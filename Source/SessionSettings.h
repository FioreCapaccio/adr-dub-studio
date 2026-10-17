#pragma once

#include "Model.h"
#include "Theme.h"

namespace fc
{
/** Impostazioni della sessione: timecode (come in Pro Tools) e parametri ADR. */
class SessionSettingsDialog : public juce::Component
{
public:
    struct Values { int tcFormat; double tcStart; bool rulerTc; double preRoll, postRoll; int beeps; bool streamer; int overlay; int dialogueSize; juce::String naming; };

    SessionSettingsDialog (const Project&, std::function<void (const Values&)> onAccept);
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;

private:
    void close (bool ok);
    void updateStartHint();

    const Project& project;
    std::function<void (const Values&)> accept;
    juce::Label tcLbl, startLbl, preLbl, postLbl, beepLbl, sizeLbl, nameLbl, hint;
    juce::Slider dialogueSize { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::ComboBox tcFormat, beeps;
    juce::TextEditor start, preRoll, postRoll, naming;
    juce::ToggleButton ovNumber { TRANS ("Numero") }, ovChar { TRANS ("Personaggio") }, ovScene { TRANS ("Scena") }, ovTc { TRANS ("Timecode") }, ovLine { TRANS ("Battuta") };
    juce::Label ovLbl;
    juce::ToggleButton rulerTc { TRANS ("Il righello mostra il timecode") }, streamer { TRANS ("Streamer visivo sul video (ADR)") };
    juce::TextButton okBtn { TRANS ("OK") }, cancelBtn { TRANS ("Annulla") };
};

} // namespace fc
