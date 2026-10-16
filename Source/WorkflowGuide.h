#pragma once

#include "Model.h"
#include "Theme.h"

namespace fc::workflow
{
struct Step
{
    juce::String title, detail;
    bool done = false;
    int action = 0;            // identificatore dell'azione che il pulsante lancia (0 = nessuna)
    juce::String button;
};

enum Action { ActImportVideo = 1, ActImportScript, ActTracks, ActRecord, ActApprove, ActCheck, ActDeliver };

/** I passi del flusso di doppiaggio e il loro stato, calcolati dalla sessione. */
std::vector<Step> steps (const Project&, bool checkedClean, bool delivered);

class Panel : public juce::Component
{
public:
    Panel (std::vector<Step>, std::function<void (int)> act);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::vector<Step> items;
    std::function<void (int)> action;
    juce::OwnedArray<juce::TextButton> buttons;
};
} // namespace fc::workflow
