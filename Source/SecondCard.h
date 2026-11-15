#pragma once

#include <JuceHeader.h>
#include "Theme.h"

namespace fc
{
/** Buffer fra due schede audio con clock diversi (produttore e consumatore su thread diversi, senza lock).
    Chi legge ricampiona con interpolazione lineare e corregge piano piano il rapporto per tenere il livello di riempimento vicino al valore voluto:
    cosi' la deriva fra i due clock (e l'eventuale differenza di frequenza di campionamento) non produce clic ne un ritardo che cresce. */
class DriftFifo
{
public:
    void prepare (int channels, int capacityFrames = 16384)
    {
        ch = channels; cap = capacityFrames;
        buf.assign ((size_t) ch * (size_t) cap, 0.0f);
        w = 0;
        reset();
    }
    void setTarget (int frames) noexcept { target = juce::jlimit (64, cap / 4, frames); }
    void reset() noexcept { resetFlag = true; }

    /** Thread del produttore: accoda n frame per canale. */
    void write (const float* const* data, int n) noexcept
    {
        const auto wp = w.load (std::memory_order_relaxed);
        for (int c = 0; c < ch; ++c)
        {
            auto* dst = buf.data() + (size_t) c * (size_t) cap;
            for (int i = 0; i < n; ++i) dst[(size_t) ((wp + i) % cap)] = data[c][i];
        }
        w.store (wp + n, std::memory_order_release);
    }

    /** Thread del consumatore: n frame per canale. baseRatio = frequenza del produttore / frequenza del consumatore. Senza dati scrive silenzio. */
    void read (float* const* out, int n, double baseRatio) noexcept
    {
        const auto wp = w.load (std::memory_order_acquire);
        if (resetFlag.exchange (false)) primed = false;
        if (! primed)
        {
            if (wp < (int64_t) target) { silence (out, n); return; }   // aspetta di avere il livello voluto
            pos = (double) (wp - target); smooth = (double) target; primed = true;
        }
        double fill = (double) wp - pos;
        if (fill > (double) cap * 0.75 || fill < 0.0) { pos = (double) (wp - target); fill = (double) target; overruns.fetch_add (1); }   // il produttore e' andato troppo avanti
        if (fill < (double) n * baseRatio * 1.05 + 2.0) { silence (out, n); primed = false; underruns.fetch_add (1); return; }
        smooth += 0.01 * (fill - smooth);
        const double adj = juce::jlimit (-0.005, 0.005, 0.002 * (smooth - (double) target) / (double) target);   // al massimo 0,5 %
        const double ratio = baseRatio * (1.0 + adj);
        for (int c = 0; c < ch; ++c)
        {
            const auto* src = buf.data() + (size_t) c * (size_t) cap;
            for (int i = 0; i < n; ++i)
            {
                const double p = pos + (double) i * ratio;
                const auto i0 = (int64_t) p;
                const float f = (float) (p - (double) i0);
                const float a = src[(size_t) (i0 % cap)], b = src[(size_t) ((i0 + 1) % cap)];
                out[c][i] = a + (b - a) * f;
            }
        }
        pos += (double) n * ratio;
        shownFill.store ((int) fill);
    }

    int getFill() const noexcept { return shownFill.load(); }
    int getTarget() const noexcept { return target; }
    int getUnderruns() const noexcept { return underruns.load(); }
    int getOverruns() const noexcept { return overruns.load(); }
    void clearCounters() noexcept { underruns = 0; overruns = 0; }

private:
    void silence (float* const* out, int n) noexcept { for (int c = 0; c < ch; ++c) juce::FloatVectorOperations::clear (out[c], n); }

    std::vector<float> buf;
    int ch = 2, cap = 16384;
    int target = 1024;
    std::atomic<int64_t> w { 0 };
    std::atomic<bool> resetFlag { true };
    // solo il consumatore
    double pos = 0.0, smooth = 0.0;
    bool primed = false;
    std::atomic<int> underruns { 0 }, overruns { 0 }, shownFill { 0 };
};

/** Seconda scheda audio, indipendente da quella principale: porta le cuffie dell'attore (il mix, i bip, il talkback) e/o riceve il microfono del direttore.
    Lo stato della scheda scelta si salva nelle preferenze; si apre solo quando serve (alla prima richiesta), come gli ingressi della scheda principale. */
class SecondCard : private juce::AudioIODeviceCallback, private juce::ChangeListener
{
public:
    SecondCard();
    ~SecondCard() override;

    juce::AudioDeviceManager manager;

    /** Legge dalle preferenze cosa fa la seconda scheda (cuffie, microfono, uscite, ingresso). */
    void loadSettings();
    void saveSettings() const;
    /** Prepara il gestore (e riapre la scheda salvata). Ritorna un errore, o stringa vuota se c'e' una scheda aperta. Thread dei messaggi. */
    juce::String ensureOpen();
    void shutdown();

    // scelte (qualunque thread)
    std::atomic<bool> outOn { false }, inOn { false };
    std::atomic<int> outPair { 0 };      // coppia di uscite attive della seconda scheda (0 = le prime due)
    std::atomic<int> micChannel { 0 };   // ingresso attivo della seconda scheda

    /** Dal thread audio della scheda principale: frequenza e blocco. */
    void setMainFormat (double rate, int blockSize);
    void pushCue (const float* l, const float* r, int n) noexcept;
    /** Microfono del direttore: n campioni alla frequenza principale; silenzio se la seconda scheda non manda ancora nulla. */
    void pullMic (float* dest, int n) noexcept;

    bool isOpen() const { return manager.getCurrentAudioDevice() != nullptr; }
    juce::String statusText() const;
    int numActiveOutputs() const;
    int numActiveInputs() const;

    DriftFifo cueFifo, micFifo;

private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateTargets();

    std::atomic<double> mainRate { 48000.0 }, cardRate { 48000.0 };
    std::atomic<int> mainBlock { 512 }, cardBlock { 512 };
    bool initialised = false;
};

/** Pannello delle Impostazioni: cosa fa la seconda scheda e quale scheda e' (con il suo selettore). */
class SecondCardPanel : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit SecondCardPanel (SecondCard&);
    ~SecondCardPanel() override;
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshChannels();
    void apply();

    SecondCard& card;
    juce::ToggleButton outBtn { TRANS ("Cuffie dell'attore sulla seconda scheda") }, inBtn { TRANS ("Microfono del direttore dalla seconda scheda") };
    juce::ComboBox pair, mic;
    juce::Label pairLbl, micLbl, hint, status;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
    bool loading = true;
};
} // namespace fc
