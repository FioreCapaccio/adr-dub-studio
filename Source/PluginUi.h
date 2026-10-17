#pragma once

#include "BuiltinFx.h"
#include "Theme.h"

namespace fc
{
/** Editor degli effetti integrati: intestazione con bypass, gruppi di manopole con valore e nome, misuratori di riduzione
    e, per il filtro, la curva di risposta. Le manopole sono collegate ai parametri (gesti e automazione compresi). */
class BuiltinEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    BuiltinEditor (BuiltinFx&, std::atomic<bool>& bypass, std::function<void()> onBypassChanged);
    ~BuiltinEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    struct Control
    {
        juce::Label name;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::TextButton> toggle;
        std::unique_ptr<juce::SliderParameterAttachment> sliderAttach;   // dopo gli slider: vengono distrutti per primi
        std::unique_ptr<juce::ButtonParameterAttachment> buttonAttach;
    };
    struct Panel
    {
        BuiltinFx::Group group;
        std::vector<std::unique_ptr<Control>> controls;
        juce::Rectangle<int> bounds, meterBounds;
    };

    void timerCallback() override;
    void paintCurve (juce::Graphics&, juce::Rectangle<float>);

    BuiltinFx& fx;
    std::atomic<bool>& bypass;
    std::function<void()> bypassChanged;
    juce::TextButton bypassButton { TRANS ("BYPASS") };
    std::vector<std::unique_ptr<Panel>> panels;
    juce::Rectangle<int> headerArea, curveArea;
    std::array<float, 3> shownMeter {};
    float shownMeterHold[3] = { 0, 0, 0 };
};

/** Finestra di scelta: effetti integrati e VST3 in un'unica lista con ricerca, categorie e descrizioni. */
class PluginBrowser : public juce::Component, private juce::ListBoxModel
{
public:
    struct Callbacks
    {
        std::function<void (BuiltinFx::Kind)> builtin;
        std::function<void (juce::File)> vst3;
        std::function<void()> browse;   // "Sfoglia...": scegliere un .vst3 da una cartella qualsiasi
    };

    PluginBrowser (juce::Array<juce::File> vst3Files, Callbacks);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct SearchBox : juce::TextEditor
    {
        std::function<bool (const juce::KeyPress&)> onKey;
        bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::TextEditor::keyPressed (k); }
    };
    struct Entry
    {
        bool header = false, builtin = false;
        juce::String title, desc, tag, badge;
        juce::Colour colour;
        BuiltinFx::Kind kind = BuiltinFx::Filter;
        juce::File file;
    };

    int getNumRows() override { return (int) shown.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { choose (row); }
    void returnKeyPressed (int row) override { choose (row); }
    void selectedRowsChanged (int row) override;
    void applyFilter();
    void choose (int row);
    void closeDialog();

    std::vector<Entry> all, shown;
    Callbacks cb;
    int tab = 0, lastRow = -1;   // 0 tutti, 1 integrati, 2 VST3
    SearchBox search;
    juce::ListBox list { {}, this };
    juce::TextButton tabs[3] { juce::TextButton (TRANS ("Tutti")), juce::TextButton (TRANS ("Integrati")), juce::TextButton (TRANS ("VST3")) };
    juce::TextButton addButton { TRANS ("Aggiungi") }, cancelButton { TRANS ("Annulla") }, browseButton { TRANS ("Sfoglia...") };
};
} // namespace fc
