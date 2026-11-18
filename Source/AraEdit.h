#pragma once
#include <JuceHeader.h>
#include "Model.h"

#if ADR_ARA
namespace fc::ara
{
/** Porzione di audio di un clip che si presenta al plug-in ARA (copiata: il plug-in la legge da altri thread). */
struct Source
{
    juce::AudioBuffer<float> audio;   // stereo o mono, alla frequenza del progetto
    double rate = 44100.0;
    juce::String name;
};

/** Un plug-in ARA2 (Photon, Melodyne, SpectraLayers...) agganciato a un documento con un solo clip.
    L'interfaccia del plug-in lavora sul clip; render() produce l'audio con le modifiche fatte. Solo thread dei messaggi. */
class Session
{
public:
    static void create (Source, const juce::File& vst3, std::function<void (std::unique_ptr<Session>, juce::String error)> done);
    ~Session();

    juce::String pluginName() const;
    juce::AudioProcessor& plugin();
    bool hasEditor() const;
    juce::AudioProcessorEditor* createEditor();

    /** Ascolto dall'interfaccia del plug-in (tasto play del plug-in): suona sul dispositivo audio dell'app. */
    void attachPreview (juce::AudioDeviceManager&);
    /** Esegue il render fuori linea del clip attraverso il plug-in (stereo, stessa lunghezza dell'ingresso). */
    juce::AudioBuffer<float> render();

    struct Impl;
private:
    explicit Session (std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl;   // condiviso solo durante il caricamento asincrono
};

/** Apre la finestra con l'interfaccia ARA del plug-in: "Applica al clip" chiama onApply con l'audio modificato. */
void openWindow (Source, const juce::File& vst3, juce::AudioDeviceManager&, juce::Component* centreAround,
                 std::function<void (juce::AudioBuffer<float>)> onApply, std::function<void (juce::String)> onError);
}
#endif
