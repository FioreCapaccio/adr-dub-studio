#pragma once

#include "Model.h"
#include <array>

/** Livellatore automatico del volume: analizza una traccia e scrive sul suo volume un'automazione che rende uniforme il livello percepito
    (come un "vocal rider"). Il loudness e' quello BS.1770 (K-weighting) a finestre di 400 ms con passo di 100 ms. */
namespace fc::level
{
struct Settings
{
    bool autoTarget = true;        // obiettivo = livello mediano del parlato della traccia
    double targetLufs = -23.0;     // altrimenti questo
    double maxBoostDb = 9.0;       // massimo aumento
    double maxCutDb = 9.0;         // massima riduzione
    int reaction = 1;              // 0 lenta, 1 media, 2 rapida
    double gateAbsLufs = -50.0;    // sotto questo livello e' pausa
    double gateRelLu = 30.0;       // e cosi' sotto il parlato forte (90 percentile - questo)
    double floorMarginDb = 20.0;   // senza tono, conta come parlato solo cio' che sta almeno cosi' sopra il fondo (10 percentile)
    double peakCeilingDb = -1.0;   // l'aumento non porta i picchi oltre questo livello
    double deadbandDb = 0.7;       // differenze piu' piccole non muovono il guadagno
    double toleranceDb = 0.4;      // semplificazione della curva
    bool lowerPauses = true;       // nelle pause il volume scende a pauseDb (rispetto al fader); false = resta quello dell'ultima frase
    double pauseDb = -12.0;        // livello nelle pause: abbassa fruscio e rumori senza creare buchi
    double minPauseSec = 0.6;      // solo le pause vere (non i silenzi tra le parole)
    double pauseHoldSec = 0.25;    // attesa dopo la fine della frase (code e riverbero)
    double pauseFallSec = 0.3, pauseRiseSec = 0.25;   // discesa nella pausa e risalita prima della frase
};

inline constexpr double kHopSec = 0.1;     // passo
inline constexpr int kSubPerWindow = 4;    // 4 x 100 ms = finestra di 400 ms

/** Energia K-weighted (media dei canali sommati) e picco di ogni blocco di 100 ms: si alimenta a blocchi, anche da un file. */
class Analyser
{
public:
    explicit Analyser (double sampleRate);
    void process (const float* l, const float* r, int n);
    void finish();
    const std::vector<double>& energy() const { return e; }       // media dei quadrati K-weighted, canali sommati
    const std::vector<float>& peak() const { return pk; }         // picco lineare del blocco
    const std::vector<char>& voiced() const { return vo; }        // nel blocco c'e' una voce (tono 60-500 Hz): il rumore di fondo non ne ha

private:
    int subLen;
    int count = 0;
    double acc = 0.0;
    float blockPeak = 0.0f;
    struct Filt { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0; };
    std::array<std::array<Filt, 2>, 2> filt;   // [canale][shelf, passa-alto]
    std::vector<double> e;
    std::vector<float> pk;
    std::vector<char> vo;
    double rate;
    Filt lp;                       // passa-basso prima di scendere a 16 kHz per il tono
    std::vector<float> blk;        // blocco in corso (mono, filtrato)
    void endBlock();
};

/** Livello a finestre (LUFS) e picco (dBFS) dei frame di 100 ms, dalla finestra di 400 ms centrata sul frame. */
struct Frames
{
    std::vector<float> lufs, peakDb;
    std::vector<char> voiced;
};
Frames framesFrom (const Analyser&);

struct Plan
{
    std::vector<float> gainDb;        // guadagno per frame (liscio, con attacco e rilascio)
    std::vector<char> active;         // frame riconosciuti come parlato (gli altri sono pause)
    double speechMedian = -100.0;     // livello mediano del parlato (LUFS)
    double targetUsed = -23.0;
    double spreadBefore = 0.0, spreadAfter = 0.0;   // deviazione standard del livello dei frame con parlato (dB)
    int activeFrames = 0;
};
Plan planGains (const Frames&, const Settings&);

/** Curva semplificata: posizione in campioni e guadagno in dB. */
std::vector<std::pair<int64_t, float>> curveFrom (const Plan&, double rate, const Settings&);

/** Curva di una traccia, pronta per essere scritta. */
struct TrackCurve
{
    int trackId = 0;
    std::vector<std::pair<int64_t, float>> points;
    Plan plan;
    juce::String trackName;
};

struct Request
{
    Session snapshot;                // copia staccata della sessione
    std::vector<int> trackIds;
    double rate = 48000.0;
    int64_t totalSamples = 0;
    Settings settings;
    int64_t from = -1, to = -1;      // intervallo (campioni); -1 = tutta la traccia
};

struct Outcome
{
    bool ok = false, cancelled = false;
    juce::String message;
    std::vector<TrackCurve> curves;
};

/** Il lavoro (render della traccia dalla sua catena, analisi, curva). setProgress ritorna false per annullare. */
Outcome process (const Request&, const std::function<bool (double)>& setProgress, const std::function<void (const juce::String&)>& setStatus);

/** Scrive le curve sul volume delle tracce (attorno al livello del fader), sostituendo i punti nell'intervallo. Un solo passo di undo.
    Ritorna il numero di punti limitati dal massimo della corsia (+6 dB) in 'clamped'. */
int apply (Project&, const std::vector<TrackCurve>&, int64_t from, int64_t to, int* clamped = nullptr);
} // namespace fc::level
