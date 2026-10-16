#pragma once

#include "Model.h"
#include "Limiter.h"
#include "Loudness.h"

namespace fc
{
/** Esegue gli inserti plug-in di una traccia (o del master) in place su un buffer stereo (thread audio o export). */
void runInserts (const TrackLive& live, juce::AudioBuffer<float>& buf, int n, juce::MidiBuffer& midi);

/** Somma delle latenze dei plug-in attivi (pronti e non in bypass) di una catena. realtime: usa la lettura non bloccante. */
int insertsLatency (const TrackLive& live, bool realtime);

/** Mixa una sessione in un buffer stereo. Usato sia dal callback audio sia dall'esportazione offline. */
class Mixer
{
public:
    static constexpr int kChunk = 2048;

    Mixer()
    {
        scratch.setSize (2, kChunk);
        for (auto& b : auxBuf) b.setSize (2, kChunk);
        for (auto& b : groupBuf) b.setSize (2, kChunk);
        lastGains.reserve (512);
        lastSend.reserve (512);
        dryRing[0].assign ((size_t) TrackLive::kPdcCap, 0.0f);
        dryRing[1].assign ((size_t) TrackLive::kPdcCap, 0.0f);
    }

    /** Scrive (sovrascrivendo) n campioni stereo in out[outStart...], leggendo la timeline da pos.
        Applica inserti, compensazione di latenza, fader/pan/mute/solo delle tracce, ma non il master. */
    void render (const Session& s, int64_t pos, juce::AudioBuffer<float>& out, int outStart, int n);

    /** > 0: suona solo il clip con questo id (anche se silenziato), ignorando mute e solo delle tracce: ascolto dei take ADR.
        -1: l'originale, cioe' tutto tranne i take ADR (mute e solo restano validi). 0: normale. */
    int64_t auditionClip = 0;

    /** Compensazione di latenza dei plug-in: le tracce con meno latenza vengono ritardate fino alla piu' lenta. */
    void setPdcEnabled (bool on) noexcept { pdcEnabled = on; }
    /** Latenza (campioni) della traccia piu' lenta nell'ultimo blocco: di tanto il mix esce in ritardo. */
    int getMaxLatency() const noexcept { return maxLatency; }
    /** Come sopra, calcolata su una sessione (per l'esportazione). */
    static int computeLatency (const Session& s);

private:
    void renderClip (const Clip& c, int64_t pos, int n);

    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    bool pdcEnabled = true;
    int maxLatency = 0;
    std::vector<int> latencies;
    // ritorni FX
    juce::AudioBuffer<float> auxBuf[2];
    std::vector<std::array<float, 2>> lastSend;    // guadagno di send precedente per traccia (rampa)
    float auxLastGain[2] { -1.0f, -1.0f };
    // bus di gruppo
    juce::AudioBuffer<float> groupBuf[Session::kNumGroups];
    float groupLastGain[Session::kNumGroups] { -1.0f, -1.0f, -1.0f, -1.0f };
    std::vector<float> dryRing[2];                 // ritardo del segnale "secco" per allinearlo ai ritorni
    int dryPos = 0;
    // stato di rampa per traccia, indicizzato per posizione (solo thread audio / render)
    std::vector<std::array<float, 2>> lastGains;
};

/** Mixdown offline di una sessione (con master) su file WAV/AIFF/FLAC a 24 bit.
    progress riceve 0..1 e restituisce false per annullare. Ritorna true se completato. */
/** Se to > from esporta solo l'intervallo [from, to) (campioni): il file dura esattamente to - from. */
bool renderMixdown (const Session& session, double rate, const juce::File& file,
                    const std::function<bool (double)>& progress, juce::String& message, int64_t from = 0, int64_t to = -1);

//==============================================================================
/** Catena del master: inserti plug-in, fader, limiter e misuratori. Usata dalla riproduzione e dall'esportazione. */
class MasterBus
{
public:
    void prepare (double rate) { limiter.prepare (rate); reset(); }
    void reset() { limiter.reset(); lastGain = -1.0f; limiterWasOn = false; }
    /** Ritardo introdotto dal limiter (campioni), da compensare nell'esportazione. */
    int latency (const TrackLive& master, bool realtime = false) const
    {
        return insertsLatency (master, realtime) + (master.limiterOn.load() ? limiter.latency() : 0);
    }

    /** buf: 2 canali, n <= Mixer::kChunk. */
    void process (TrackLive& master, juce::AudioBuffer<float>& buf, int n);

private:
    Limiter limiter;
    float lastGain = -1.0f;
    bool limiterWasOn = false;
    juce::MidiBuffer midi;
};

struct TakeSpec { int trackId; juce::File file; int inputMode; };
struct TakeResult { int trackId; juce::File file; int64_t startPos; int64_t latency; int64_t length; };

//==============================================================================
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    bool initialise (const juce::XmlElement* savedState = nullptr);
    void shutdown();

    juce::AudioDeviceManager deviceManager;

    /** Pubblica un nuovo snapshot della sessione (thread messaggi). */
    void publish (std::shared_ptr<const Session> s);

    // trasporto (qualunque thread)
    void play() { playing = true; }
    void stop() { playing = false; }
    bool isPlaying() const noexcept { return playing.load(); }
    void setPosition (int64_t p) { pendingSeek = juce::jmax<int64_t> (0, p); }
    int64_t getPosition() const noexcept
    {
        const auto s = pendingSeek.load();
        return s >= 0 ? s : position.load();
    }
    void setLoop (bool on, int64_t start, int64_t end)
    {
        loopStart = start; loopEnd = end; loopOn = on && end > start;
    }
    bool isLooping() const noexcept { return loopOn.load(); }
    int64_t getLoopStart() const noexcept { return loopStart.load(); }
    int64_t getLoopEnd() const noexcept { return loopEnd.load(); }

    double getSampleRate() const noexcept { return sampleRate.load(); }

    /** Ritardo (campioni) con cui il mix arriva all'uscita rispetto alla testina, per via dei plug-in e del limiter. */
    int getPdcLatency() const noexcept { return pdcLatency.load(); }
    std::atomic<bool> pdcOn { true };

    /** Ultimi n campioni (mono, n <= 16384) dell'uscita master per l'analizzatore di spettro. */
    void readSpectrumSamples (float* dest, int n) const
    {
        n = juce::jmin (n, kSpecCap);
        const uint32_t end = specPos.load();
        for (int i = 0; i < n; ++i) dest[i] = specRing[(end - (uint32_t) n + (uint32_t) i) & (kSpecCap - 1)];
    }

    /** Misuratore di loudness sull'uscita master (post limiter), attivo durante la riproduzione. */
    Loudness::Meter loudness;

    // ingressi e registrazione
    bool ensureInput();   // apre l'ingresso predefinito se non ce n'e' uno attivo (thread messaggi)
    int getNumInputChannels() const noexcept { return numInputs.load(); }
    void setNumInputChannelsForTest (int n) { numInputs = n; }
    /** Prepara una registrazione per traccia e poi chiamare play(). Ritorna un messaggio d'errore, o stringa vuota. */
    juce::String startRecording (const std::vector<TakeSpec>&);
    /** Ascolto di un solo clip (0 = normale): vale per la riproduzione, non per l'esportazione. */
    void setAuditionClip (int64_t clipId) { auditionClip = clipId; }
    int64_t getAuditionClip() const noexcept { return auditionClip.load(); }
    /** Chiude i file e restituisce le registrazioni fatte. */
    std::vector<TakeResult> stopRecording();
    bool isRecording() const noexcept { return recording.load(); }
    int64_t getRecordStart() const noexcept { return recStart.load(); }
    /** Ritardo (campioni) tra cio' che l'esecutore sente e cio' che registra: dispositivo in ingresso e uscita piu' plug-in. */
    int64_t recordLatency() const;
    int getDroppedBlocks() const noexcept { return dropped.load(); }
    /** Punch automatico: si scrive solo tra ingresso e uscita (campioni sulla timeline). */
    void setPunch (int64_t in, int64_t out) { punchOut = out; punchIn = in; }
    void clearPunch() { punchIn = -1; punchOut = -1; }
    /** Bip di conteggio ADR alle posizioni indicate (campioni sulla timeline, come si sente il materiale). */
    void setBeeps (const std::vector<int64_t>& positions)
    {
        const int n = (int) juce::jmin<size_t> (positions.size(), (size_t) kMaxBeeps);
        for (int i = 0; i < n; ++i) beepPos[i] = positions[(size_t) i];
        beepCount = n;
    }
    void clearBeeps() { beepCount = 0; }
    /** Mix per le cuffie dell'attore su una seconda coppia di uscite (la prima coppia e' il master): mix, bip e talkback del direttore con livelli propri. */
    struct CueMix
    {
        std::atomic<bool> enabled { false };
        std::atomic<int> pair { 1 };               // coppia di uscite: 1 = uscite 3-4, 2 = 5-6...
        std::atomic<float> mixDb { 0.0f }, beepDb { 0.0f }, talkDb { 0.0f };
        std::atomic<float> mixDuckDb { -12.0f };   // di quanto si abbassa il mix mentre il direttore parla
        std::atomic<int> talkInput { 0 };          // ingresso del microfono del direttore (0 = il primo)
        std::atomic<bool> talkOn { false };        // tasto talkback premuto
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    };
    CueMix cue;
    int getNumOutputChannels() const noexcept { return numOutputs.load(); }
    /** Apre le uscite necessarie alla coppia indicata (thread messaggi). Ritorna un errore, o stringa vuota se va bene. */
    juce::String openCueOutputs (int pair);
    std::atomic<bool> monitorInput { false };
    std::atomic<float> inPeakL { 0.0f }, inPeakR { 0.0f };
    std::function<void (double)> onSampleRateChanged; // thread messaggi

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct Take
    {
        int trackId = 0;
        juce::File file;
        int srcA = 0, srcB = -1;      // canali d'ingresso (srcB = -1: mono)
        int64_t startPos = -1;
        int64_t latency = 0;
        std::atomic<int64_t> written { 0 };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    };

    void processChunk (int n, const float* const* in, int numIn, int inOffset);
    void processChunkMain (int n, const float* const* in, int numIn, int inOffset);
    void finishCue (int n, const float* const* in, int numIn, int inOffset);
    juce::AudioBuffer<float> cueBuf { 2, Mixer::kChunk };
    bool cueReady = false;
    std::atomic<int> numOutputs { 2 };

    static constexpr int kSpecCap = 16384;   // potenza di 2
    std::array<float, kSpecCap> specRing {};
    std::atomic<uint32_t> specPos { 0 };

    static constexpr int kMaxBeeps = 8;
    std::atomic<int64_t> auditionClip { 0 };
    std::atomic<int64_t> punchIn { -1 }, punchOut { -1 };
    std::atomic<int64_t> beepPos[kMaxBeeps];
    std::atomic<int> beepCount { 0 };
    juce::SpinLock recLock;
    std::vector<std::unique_ptr<Take>> takes;
    std::atomic<bool> recording { false };
    std::atomic<int64_t> recStart { -1 };
    std::atomic<int> numInputs { 0 }, dropped { 0 };
    juce::TimeSliceThread writerThread { "Arotron registrazione" };

    juce::SpinLock snapLock;
    std::shared_ptr<const Session> pending, current;
    bool hasPending = false;

    Mixer mixer;
    juce::AudioBuffer<float> master { 2, Mixer::kChunk };
    MasterBus masterBus;
    bool busNeedsReset = true;
    float playGain = 0.0f;

    std::atomic<bool> playing { false };
    std::atomic<int64_t> position { 0 }, pendingSeek { -1 };
    std::atomic<bool> loopOn { false };
    std::atomic<int64_t> loopStart { 0 }, loopEnd { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> pdcLatency { 0 };
};

} // namespace fc
