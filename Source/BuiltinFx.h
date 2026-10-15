#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

namespace fc
{
/** Effetti integrati ("ADR Built-in"): processori stereo semplici, senza dipendenze esterne, che si inseriscono come i VST3
    (stessa catena di insert, stesso PDC, stesso export). L'editor e' quello generico di JUCE; lo stato e' l'insieme dei parametri. */
class BuiltinFx : public juce::AudioProcessor
{
public:
    enum Kind { Filter, DeEsser, Gate, Reverb, Delay, Saturator, Chorus, numKinds };
    static constexpr const char* formatName = "ADR Built-in";

    explicit BuiltinFx (Kind);

    static juce::String kindName (Kind);
    static juce::String kindHelp (Kind);
    /** Descrizione da salvare nella sessione (format "ADR Built-in", identificatore "builtin:<id>"). */
    static juce::PluginDescription describe (Kind);
    /** Crea l'effetto descritto, oppure nullptr se la descrizione non e' di un integrato. */
    static std::unique_ptr<BuiltinFx> create (const juce::PluginDescription&);

    Kind getKind() const noexcept { return kind; }
    /** Parametro per identificatore (nei test). */
    juce::RangedAudioParameter* param (const juce::String& id) const;

    const juce::String getName() const override { return kindName (kind); }
    void prepareToPlay (double rate, int maxBlock) override;
    void releaseResources() override {}
    void reset() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

private:
    float get (int i) const { return static_cast<juce::AudioParameterFloat*> (params[(size_t) i])->get(); }
    void addFloat (const juce::String& id, const juce::String& name, float lo, float hi, float def, const juce::String& unit = {}, float centre = 0.0f);
    void addBool (const juce::String& id, const juce::String& name, bool def);
    bool getBool (int i) const { return static_cast<juce::AudioParameterBool*> (params[(size_t) i])->get(); }

    void doFilter (juce::AudioBuffer<float>&);
    void doDeEsser (juce::AudioBuffer<float>&);
    void doGate (juce::AudioBuffer<float>&);
    void doReverb (juce::AudioBuffer<float>&);
    void doDelay (juce::AudioBuffer<float>&);
    void doSaturator (juce::AudioBuffer<float>&);
    void doChorus (juce::AudioBuffer<float>&);

    Kind kind;
    std::vector<juce::RangedAudioParameter*> params;   // di proprieta' del processore (addParameter)
    double rate = 48000.0;

    // filtri (filtro HP/LP, de-esser, tono della saturazione): [canale][stadio]
    std::array<std::array<juce::IIRFilter, 2>, 2> hp, lp;
    float lastHp = -1.0f, lastLp = -1.0f;
    // dinamica
    float env = 0.0f, gainNow = 1.0f;
    bool gateOpen = false;
    // riverbero, delay, chorus
    juce::Reverb reverb;
    std::array<std::vector<float>, 2> ring;
    int ringPos = 0;
    float delayNow = -1.0f;
    std::array<float, 2> fbLp {};
    double lfoPhase = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltinFx)
};
} // namespace fc
