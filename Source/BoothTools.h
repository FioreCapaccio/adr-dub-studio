#pragma once

#include "AudioEngine.h"
#include "Theme.h"

namespace fc
{
namespace script
{
struct State
{
    bool valid = false;          // c'e' almeno un cue da mostrare
    int current = -1;            // indice (nell'elenco ordinato per posizione) del cue in corso o, se non e' ancora cominciato, del prossimo
    bool active = false;         // vero dal 'lead' prima dell'ingresso fino all'uscita
    double progress = 0.0;       // 0..1 dall'ingresso all'uscita (0 prima)
    double secondsToStart = 0.0; // > 0 prima dell'ingresso
    double secondsToEnd = 0.0;
};

/** Cue ordinati per posizione d'ingresso. */
std::vector<AdrCue> sorted (const std::vector<AdrCue>&);
/** Quale cue mostrare alla posizione 'pos': quello in corso, altrimenti il prossimo (lead = anticipo con cui lo si considera attivo). */
State stateAt (const std::vector<AdrCue>& sortedCues, int64_t pos, double rate, double leadSec);
} // namespace script

/** Lettore copione per l'attore (secondo schermo): battuta in corso grande, avanzamento, prossime battute. */
class ScriptComponent : public juce::Component, private juce::Timer
{
public:
    ScriptComponent (Project&, AudioEngine&);
    ~ScriptComponent() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    void setFontSize (int px);

    Project& project;
    AudioEngine& engine;
    juce::TextButton smaller { "A-" }, bigger { "A+" };
    int fontSize = 56;
    int64_t lastPos = -1;
    int lastCue = -2;
    bool lastActive = false;
};

/** Pannello delle cuffie dell'attore: uscita, livelli, talkback. */
class CueMixPanel : public juce::Component, private juce::Timer
{
public:
    explicit CueMixPanel (AudioEngine&);
    ~CueMixPanel() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshOutputs();
    void apply();
    void save() const;

    AudioEngine& engine;
    juce::ToggleButton enable { TRANS ("Cuffie dell'attore attive") }, duckMix { TRANS ("Abbassa il mix mentre il direttore parla") };
    juce::ComboBox outputs, talkInput;
    juce::Slider mix { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                 beep { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                 talk { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label outLbl, mixLbl, beepLbl, talkLbl, inLbl, status;
    juce::TextButton talkButton { TRANS ("TALKBACK (tieni premuto)") };
    float shownL = 0.0f, shownR = 0.0f;
    bool loading = false;
};

class BoothWindow : public juce::DocumentWindow
{
public:
    BoothWindow (const juce::String& title, std::function<void()> closeCallback)
        : juce::DocumentWindow (title, theme::bg, juce::DocumentWindow::allButtons), onClose (std::move (closeCallback))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
    }
    void closeButtonPressed() override { if (onClose) onClose(); }

private:
    std::function<void()> onClose;
};
} // namespace fc
