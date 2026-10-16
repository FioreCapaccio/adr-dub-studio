#pragma once

#include <JuceHeader.h>
#include <functional>
#include <vector>

/** Accesso nativo ai video (AVFoundation): informazioni, miniature, e inserimento di una traccia audio nel video. */
namespace fc::video
{
struct Info
{
    bool ok = false;
    double duration = 0.0;   // secondi
    double fps = 25.0;
    int width = 0, height = 0;
    bool hasAudio = false;
    juce::String error;
};

bool isVideoFile (const juce::File&);

Info probe (const juce::File&);

/** Miniature a intervalli regolari: l'immagine i rappresenta il tempo i * interval. Thread-safe. */
std::vector<juce::Image> thumbnails (const juce::File&, double interval, int count, int height, const std::function<void (float)>& progress = {});

/** Crea un .mov con il video originale (senza ricodificarlo) e l'audio del file indicato al posto di quello originale.
    videoOffsetSec > 0 ritarda il video rispetto all'audio, < 0 lo anticipa.
    Se rangeEndSec > rangeStartSec il file audio contiene solo quell'intervallo della timeline (inizia a rangeStartSec): del video si usa la parte corrispondente.
    progress riceve 0..1 e restituisce false per annullare. */
bool muxAudio (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec,
               const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec = 0.0, double rangeEndSec = -1.0);

/** Testo da incidere sul video per la revisione (burn-in): timecode in alto a sinistra, personaggio in alto a destra, battuta in basso. */
struct BurnInLine { double from = 0.0, to = 0.0; juce::String character, text; int number = 0; };   // secondi della timeline
struct BurnIn
{
    bool timecode = true, cues = true;
    std::function<juce::String (double)> timecodeAt;   // secondi della timeline -> timecode
    std::vector<BurnInLine> lines;
};

/** Vero se il backend sa incidere il burn-in (oggi solo macOS con AVFoundation). */
bool burnInSupported();

/** Come muxAudio, ma ricodifica il video (H.264) incidendo il burn-in su ogni fotogramma. */
bool muxAudioBurnIn (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec, const BurnIn&,
                     const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec = 0.0, double rangeEndSec = -1.0);

/** macOS: decodifica l'audio di un video (qualunque contenitore, .mov compreso) con AVFoundation, gia' alla frequenza richiesta,
    mono o stereo. Ritorna false se non e' disponibile (altri sistemi) o se fallisce. progress: 0..1. */
bool decodeAudioNative (const juce::File&, double targetRate, juce::AudioBuffer<float>& out, const std::function<void (float)>& progress, juce::String& error);

/** Regola la velocita' del lettore nativo dentro un juce::VideoComponent (macOS): serve a recuperare piccole differenze di sincronia
    senza saltare. Altrove non fa nulla. */
void setNativePlayerRate (juce::Component& videoComponent, double rate);

/** Solo con il backend ffmpeg (Windows): estrae la prima traccia audio del video in un WAV a virgola mobile, nella frequenza originale. */
bool extractAudio (const juce::File& video, const juce::File& wavOut, juce::String& error);

/** Vero se il backend e' ffmpeg: decodifica, miniature ed esportazione usano ffmpeg/ffprobe (vicino all'app o nel PATH). */
bool usesExternalDecoder();

/** Percorso di ffmpeg/ffprobe oppure stringa vuota se non si trovano (con il backend ffmpeg). */
juce::String ffmpegStatus();
} // namespace fc::video
