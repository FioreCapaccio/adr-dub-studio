#pragma once

#include "AudioEngine.h"
#include "BuiltinFx.h"
#include "Theme.h"

namespace fc
{
/** Finestra con l'editor di un plug-in. */
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (std::shared_ptr<InsertSlot> s, const juce::String& title, std::function<void (PluginWindow*)> onClose);
    void closeButtonPressed() override { if (closeCallback) closeCallback (this); }
    std::shared_ptr<InsertSlot> slot;

private:
    std::function<void (PluginWindow*)> closeCallback;
};

/** Dialogo di ricerca tra i VST3 installati. */
class PluginPicker : public juce::Component, private juce::ListBoxModel
{
public:
    PluginPicker (juce::Array<juce::File> files, std::function<void (juce::File)> onPick);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override { return shown.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { choose (row); }
    void returnKeyPressed (int row) override { choose (row); }
    void applyFilter();
    void choose (int row);
    void closeDialog();

    juce::Array<juce::File> all, shown;
    std::function<void (juce::File)> pick;
    juce::TextEditor search;
    juce::ListBox list { {}, this };
    juce::TextButton addButton { "Aggiungi" }, cancelButton { "Annulla" };
};

/** Gestisce gli inserti VST3 (per traccia e sul master): ricerca, caricamento, editor, ripristino dalla sessione. */
class PluginHost
{
public:
    static constexpr int kBlock = Mixer::kChunk;   // dimensione massima dei blocchi con cui si chiama processBlock

    explicit PluginHost (AudioEngine&);
    ~PluginHost();

    std::function<void()> onChanged;                       // la catena di una traccia e' cambiata (aggiorna la UI)
    std::function<void (const juce::String&)> onError;

    /** Apre il dialogo di ricerca e aggiunge il plug-in scelto in coda alla catena. */
    void pickAndAdd (std::shared_ptr<TrackLive> live, juce::Component* centreAround);
    void removeSlot (TrackLive&, int index);
    void moveSlot (TrackLive&, int index, int delta);
    void setBypass (TrackLive&, int index, bool bypass);
    void openEditor (std::shared_ptr<InsertSlot>, const juce::String& ownerName);

    /** Dopo il caricamento di una sessione: istanzia i plug-in salvati (in modo asincrono). */
    void restoreAll (const Session&);
    /** Alla variazione della frequenza di campionamento. */
    void reprepareAll (const Session&);
    /** Chiude gli editor dei plug-in che non esistono piu' nella sessione. */
    void pruneWindows (const Session&);
    void closeAllWindows();

    void addFromFile (std::shared_ptr<TrackLive> live, const juce::File&, std::function<void (juce::String)> done = {});
    /** Aggiunge in coda un effetto integrato. */
    void addBuiltin (std::shared_ptr<TrackLive> live, BuiltinFx::Kind, std::function<void (juce::String)> done = {});

    static juce::Array<juce::File> findInstalledVST3();

private:
    juce::String prepareInstance (juce::AudioProcessor&);
    void openPicker (std::shared_ptr<TrackLive> live, juce::Component* centreAround);
    void instantiate (std::shared_ptr<InsertSlot>, std::function<void (juce::String)> done);
    void append (TrackLive&, std::shared_ptr<InsertSlot>);

    AudioEngine& engine;
    juce::AudioPluginFormatManager formats;
    juce::OwnedArray<PluginWindow> windows;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace fc
