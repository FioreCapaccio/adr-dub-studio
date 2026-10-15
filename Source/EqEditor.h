#pragma once

#include "Model.h"
#include "Theme.h"
#include "TrackPresets.h"

namespace fc
{
/** Editor dell'equalizzatore parametrico di una traccia: curva di risposta con nodi trascinabili e controlli per banda. */
class EqEditor : public juce::Component
{
public:
    EqEditor (Project&, std::shared_ptr<TrackLive>, std::function<double()> sampleRate);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragBand = -1; }
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    /** Rilegge i parametri dalla traccia (dopo un caricamento o un'azione esterna). */
    void refresh();

private:
    struct BandUi
    {
        juce::ComboBox type;
        juce::ToggleButton on;
        juce::Slider freq { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                     gain { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                     q { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label freqLbl, gainLbl, qLbl;
        juce::Rectangle<int> box;
    };

    juce::Rectangle<float> plot() const;
    float freqToX (float hz) const;
    float xToFreq (float x) const;
    float dbToY (float db) const;
    float yToDb (float y) const;
    int bandAt (juce::Point<int>) const;
    void changed();
    static juce::Colour bandColour (int i);

    Project& project;
    std::shared_ptr<TrackLive> live;
    std::function<double()> rate;
    juce::ToggleButton enableBtn { "EQ ATTIVO" };
    juce::Label typeLbl, typeInfo;
    juce::ComboBox typeBox;   // tipo di traccia: applica una catena consigliata
    juce::ToggleButton compOn { "COMPRESSORE" };
    juce::Slider compThr { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight }, compRatio { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                 compAtk { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight }, compRel { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                 compMakeup { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label compThrLbl, compRatioLbl, compAtkLbl, compRelLbl, compMakeupLbl;
    juce::Rectangle<int> compBox;
    juce::TextButton resetBtn { "AZZERA" };
    BandUi ui[ParamEq::kBands];
    int dragBand = -1;
    bool loading = false;
    static constexpr float kRange = 18.0f;
};

/** Finestra che ospita l'editor dell'equalizzatore. */
class EqWindow : public juce::DocumentWindow
{
public:
    EqWindow (const juce::String& title, std::function<void()> closeCallback)
        : juce::DocumentWindow (title, theme::bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton), onClose (std::move (closeCallback))
    {
        setUsingNativeTitleBar (true);
        setResizable (false, false);
    }
    void closeButtonPressed() override { if (onClose) onClose(); }

private:
    std::function<void()> onClose;
};

} // namespace fc
