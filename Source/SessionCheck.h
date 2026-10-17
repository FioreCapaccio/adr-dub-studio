#pragma once

#include "Model.h"
#include "Theme.h"

namespace fc::check
{
struct Issue
{
    enum Level { Info, Warning, Error } level = Info;
    juce::String text;
    int cueId = 0;     // se > 0, "Vai a" porta a questo cue
    int trackId = 0;   // se > 0, seleziona questa traccia
};

/** Controlli prima della consegna: media mancanti, tracce senza ruolo, cue non ancora registrati o approvati, frame rate, ecc. */
std::vector<Issue> checkSession (const Project&);

/** Finestra con l'elenco dei problemi; 'goTo' riceve il problema scelto. */
class Panel : public juce::Component, private juce::ListBoxModel
{
public:
    Panel (std::vector<Issue>, std::function<void (const Issue&)> goTo, std::function<void()> relink);
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;

private:
    int getNumRows() override { return (int) issues.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { if (row >= 0 && row < (int) issues.size() && goTo) goTo (issues[(size_t) row]); }

    std::vector<Issue> issues;
    std::function<void (const Issue&)> goTo;
    juce::Label summary;
    juce::ListBox list { {}, this };
    juce::TextButton goBtn { TRANS ("Vai a") }, relinkBtn { TRANS ("Ricollega i media...") };
};
} // namespace fc::check
