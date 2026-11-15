#pragma once

#include "Model.h"
#include "Theme.h"

namespace fc::surface
{
/** Un comando del controller. Protocollo Mackie Control (quello dei mixer con fader come il SINCO SMC-Mixer): fader = pitch bend sui canali 1-8,
    manopole = CC 0x10-0x17 (1 = giro a destra, 0x41 = a sinistra), tasti = note on (0x7F premuto). */
struct Event
{
    enum Kind { None, Fader, Knob, Rec, Solo, Mute, Select, Rewind, Forward, Stop, Play, Record, BankDown, BankUp } kind = None;
    int index = 0;       // fader/manopola/tasto di traccia: 0..7
    float value = 0.0f;  // Fader: posizione 0..1; Knob: scatti (+ destra, - sinistra)
};

Event decode (const juce::MidiMessage&);
/** Posizione del fader (0..1) -> dB, con la stessa curva dei fader del mixer della app (-80..+6, meta' a -18 dB). */
float faderToDb (float pos01);

/** Ingresso MIDI: applica i comandi alle tracce (8 per volta, a banchi) e al trasporto. */
class Surface : private juce::MidiInputCallback
{
public:
    struct Transport
    {
        std::function<void()> play, stop, record, rewind, forward;
        std::function<void (Track&)> arm;
    };
    Surface (Project&, Transport);
    ~Surface() override;
    /** Apre un ingresso (identifier), o tutti se "*", o nessuno se vuoto. Ritorna un errore, o stringa vuota. */
    juce::String open (const juce::String& identifier);
    void apply (const Event&);   // thread dei messaggi
    juce::String lastEvent() const;
    int getBank() const noexcept { return bank; }
    bool isOpen() const noexcept { return ! inputs.empty(); }

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    Track* trackAt (int strip);

    Project& project;
    Transport tr;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    int bank = 0;   // indice della prima traccia dei fader
    mutable juce::CriticalSection lock;
    juce::String last, lastRaw;
    juce::MidiInput* lastSrc = nullptr;
    double lastMs = 0.0;
};

class Panel : public juce::Component, private juce::Timer
{
public:
    explicit Panel (Surface&);
    ~Panel() override { stopTimer(); }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;

private:
    void apply();
    void timerCallback() override;
    Surface& surface;
    juce::Label deviceLbl, status, hint;
    juce::ComboBox devices;
    juce::Array<juce::MidiDeviceInfo> infos;
    bool loading = true;
};

/** Il dispositivo scelto nelle preferenze ("*" = tutti), vuoto se nessuno. */
juce::String savedDevice();
} // namespace fc::surface
