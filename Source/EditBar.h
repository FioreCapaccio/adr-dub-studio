#pragma once

#include "Commands.h"
#include "Theme.h"

namespace fc
{
/** Pulsante della palette strumenti, con icona vettoriale. */
class ToolButton : public juce::Button
{
public:
    explicit ToolButton (Tool t) : juce::Button ({}), tool (t) { setClickingTogglesState (false); }
    Tool getTool() const noexcept { return tool; }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    Tool tool;
};

/** Barra sopra la timeline: strumenti di editing, azioni sui clip, altezza delle tracce. */
class EditBar : public juce::Component
{
public:
    explicit EditBar (juce::ApplicationCommandManager&);

    std::function<void (Tool)> onToolSelected;
    std::function<void (int)> onHeightChanged;
    std::function<void()> onFitHeights;
    std::function<void (double)> onZoomChanged;   // campioni per pixel

    void setTool (Tool);
    void setHeightValue (int h);
    void setZoomValue (double samplesPerPixel);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::ApplicationCommandManager& commands;
    std::vector<std::unique_ptr<ToolButton>> toolButtons;
    struct Action { std::unique_ptr<juce::TextButton> button; int width; };
    std::vector<Action> actions;
    juce::Slider heightSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider zoomSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };   // 0 = tutto, 1 = campione per campione (scala logaritmica)
    juce::TextButton fitBtn { TRANS ("ADATTA") };
    juce::Rectangle<int> toolsBox;
    int separatorX = 0, sliderCaptionX = 0, zoomCaptionX = 0;
};

} // namespace fc
