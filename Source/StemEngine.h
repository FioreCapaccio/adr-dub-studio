#pragma once

#include <JuceHeader.h>
#include <array>
#include <functional>

/** Separazione di un audio in Dialoghi / Musica / Effetti con il modello Bandit v2 (pesi CC BY-SA 4.0), eseguito in locale con ONNX Runtime.
    Il modello e la libreria di ONNX Runtime non stanno nell'app: si scaricano una volta (come lo Stem Splitter di Logic) in
    <dati dell'app>/engine e poi si lavora offline.

    Come funziona: l'audio (stereo o mono) va a 48 kHz, a blocchi di 8 s con sovrapposizione. Per ogni blocco il modello, su
    una versione mono (mid), stima i tre stem; da quelle stime si ricavano tre maschere complesse che sommano 1 e si applicano
    a ogni canale originale: cosi' lo stereo si mantiene e Dialoghi + Musica + Effetti = originale. */
namespace fc::stems
{
inline constexpr int kRate = 48000;
inline constexpr int kFft = 2048;
inline constexpr int kHop = 512;
inline constexpr int kBins = kFft / 2 + 1;       // 1025
inline constexpr int kChunk = 8 * kRate;         // 384000 campioni
inline constexpr int kFrames = 1 + kChunk / kHop; // 751
inline constexpr int kStems = 3;                 // 0 = dialoghi, 1 = musica, 2 = effetti

// ─── Trasformata (come torchaudio.Spectrogram normalized=True, center=True, pad reflect) ───
/** Spettro di un blocco di kChunk campioni: spec[(f * kFrames + t) * 2 + {0 re, 1 im}], f < kBins. */
void stftChunk (const float* x, float* spec);
/** Operazione inversa (sovrapposizione e somma dei frame, finestra al quadrato normalizzata): y ha kChunk campioni. */
void istftChunk (const float* spec, float* y);

/** Il "cuore" neurale: spettro [kBins][kFrames][2] -> tre stime [kStems][kBins][kFrames][2]. Iniettabile nei test. */
using CoreFn = std::function<bool (const float* spec, float* est, juce::String& error)>;

struct Options
{
    bool highQuality = false;   // sovrapposizione 50% (4 s di passo) invece di 25% (6 s)
    int workers = 1;            // blocchi elaborati insieme (1..8); il cuore deve poter essere chiamato da piu' thread
};

/** Esegue la separazione di 'channels' (1 o 2 canali, numSamples campioni, srcRate Hz). Scrive tre file WAV a 24 bit, a 48 kHz, con lo stesso numero
    di canali (la lunghezza e' quella dell'ingresso ricampionato). progress(0..1) ritorna false per annullare. */
bool separate (const float* const* channels, int numChannels, int64_t numSamples, double srcRate, const Options& opt,
               const std::array<juce::File, kStems>& outFiles, const CoreFn& core,
               const std::function<bool (float)>& progress, juce::String& error);

// ─── Modello e libreria: scaricamento una tantum ───
juce::File packDir();
bool packReady();
/** Scarica (o completa) i file mancanti e ne controlla l'impronta. progress(0..1, testo) ritorna false per annullare. */
bool downloadPack (const std::function<bool (float, const juce::String&)>& progress, juce::String& error);
/** Dimensione approssimativa da scaricare (MB), per il messaggio all'utente. */
int packDownloadMB();

/** Libreria ONNX Runtime + modello caricati; core() esegue la rete. */
class OnnxCore
{
public:
    OnnxCore();
    ~OnnxCore();
    bool load (juce::String& error);
    CoreFn fn();
    static int suggestedWorkers();   // in base ai processori fisici

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace fc::stems
