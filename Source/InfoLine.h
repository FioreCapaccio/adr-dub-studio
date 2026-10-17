#pragma once

#include "Model.h"
#include "Theme.h"

namespace fc
{
/** Info Line (come in Nuendo): inizio, fine, lunghezza, offset, guadagno, dissolvenze, mute e fase del clip selezionato, modificabili a mano. */
class InfoLinePanel : public juce::Component, private juce::ChangeListener
{
public:
    explicit InfoLinePanel (Project&);
    ~InfoLinePanel() override;
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;
    void refresh();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void commit (int field);
    int64_t clipId() const;

    Project& project;
    juce::Label title, lStart, lEnd, lLen, lOff, lGain, lFi, lFo;
    juce::TextEditor eStart, eEnd, eLen, eOff, eGain, eFi, eFo;
    juce::ToggleButton muteTb { "MUTE" }, invertTb { TRANS ("FASE INVERTITA") };
    bool loading = false;
};

class InfoLineWindow : public juce::DocumentWindow
{
public:
    InfoLineWindow (Project& p, std::function<void()> closed)
        : juce::DocumentWindow (TRANS ("Info clip (Info Line)"), theme::bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton), onClose (std::move (closed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new InfoLinePanel (p), true);
        setResizable (true, false);
        setAlwaysOnTop (true);
        setResizeLimits (640, 150, 1800, 260);
        centreWithSize (860, 170);
    }
    void closeButtonPressed() override { if (onClose) onClose(); }

private:
    std::function<void()> onClose;
};
} // namespace fc
