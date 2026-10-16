#pragma once

#include "AudioEngine.h"
#include "Theme.h"

namespace fc::mtc
{
/** Decodifica del MIDI Time Code: quarter-frame (0xF1) e full-frame (SysEx). Non dipende da altro: si prova con messaggi sintetici. */
class Decoder
{
public:
    struct State
    {
        bool valid = false;      // c'e' un timecode ricevuto
        bool running = false;    // i quarter-frame arrivano (negli ultimi ~150 ms)
        double seconds = 0.0;    // timecode corrente (interpolato fra un messaggio e l'altro)
        int fmtIndex = 2;        // indice in tcFormats()
        juce::String text;
    };

    /** Un messaggio MIDI ricevuto all'istante nowMs. */
    void feed (const juce::MidiMessage&, double nowMs);
    State state (double nowMs) const;

private:
    void complete (double nowMs);

    int pieces[8] = {};
    int received = 0, expectedNext = 0;
    int h = 0, m = 0, s = 0, f = 0, rateCode = 1;
    bool valid = false;
    double baseSeconds = 0.0, lastQuarterMs = -1.0, baseMs = 0.0;
    int baseFmt = 2;
    bool running = false;
};

enum class Action { None, LocateAndPlay, Locate, Stop };
struct Decision { Action action = Action::None; int64_t position = 0; };
struct Memory { int badTicks = 0; bool startedByMtc = false; };

/** Cosa deve fare il motore dato il timecode ricevuto. La deriva oltre 'driftFrames' (per due controlli di fila) fa riposizionare. */
Decision decide (const Decoder::State&, double tcStartSec, double offsetSec, double sampleRate, bool enginePlaying, int64_t enginePos, Memory&, double driftFrames = 2.0);

/** Ingresso MIDI + logica: un timer applica le decisioni al motore. */
class Sync : private juce::MidiInputCallback, private juce::Timer
{
public:
    Sync (AudioEngine&, std::function<double()> tcStartSec);
    ~Sync() override;

    /** Apre l'ingresso MIDI (identifier vuoto = chiude). Ritorna un errore, o stringa vuota. */
    juce::String open (const juce::String& deviceIdentifier);
    void setEnabled (bool on);
    bool isEnabled() const noexcept { return enabled; }
    void setOffsetMs (double ms) { offsetSec = ms / 1000.0; }
    Decoder::State state() const;
    bool isLocked() const noexcept { return locked; }

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void timerCallback() override;

    AudioEngine& engine;
    std::function<double()> tcStart;
    std::unique_ptr<juce::MidiInput> input;
    mutable juce::CriticalSection lock;
    Decoder decoder;
    Memory memory;
    bool enabled = false, locked = false;
    double offsetSec = 0.0;
};

class Panel : public juce::Component, private juce::Timer
{
public:
    explicit Panel (Sync&);
    ~Panel() override { stopTimer(); }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;

private:
    void timerCallback() override;
    void apply();

    Sync& sync;
    juce::Label deviceLbl, offsetLbl, status, hint;
    juce::ComboBox devices;
    juce::TextEditor offset;
    juce::ToggleButton enable { "Aggancia il trasporto al timecode MTC" };
    juce::Array<juce::MidiDeviceInfo> infos;
    bool loading = true;
};
} // namespace fc::mtc
