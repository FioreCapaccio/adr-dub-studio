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
std::vector<juce::Image> thumbnails (const juce::File&, double interval, int count, int height);

/** Crea un .mov con il video originale (senza ricodificarlo) e l'audio del file indicato al posto di quello originale.
    videoOffsetSec > 0 ritarda il video rispetto all'audio, < 0 lo anticipa.
    Se rangeEndSec > rangeStartSec il file audio contiene solo quell'intervallo della timeline (inizia a rangeStartSec): del video si usa la parte corrispondente.
    progress riceve 0..1 e restituisce false per annullare. */
bool muxAudio (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec,
               const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec = 0.0, double rangeEndSec = -1.0);

/** Solo con il backend ffmpeg (Windows): estrae la prima traccia audio del video in un WAV a virgola mobile, nella frequenza originale. */
bool extractAudio (const juce::File& video, const juce::File& wavOut, juce::String& error);

/** Vero se il backend e' ffmpeg: decodifica, miniature ed esportazione usano ffmpeg/ffprobe (vicino all'app o nel PATH). */
bool usesExternalDecoder();

/** Percorso di ffmpeg/ffprobe oppure stringa vuota se non si trovano (con il backend ffmpeg). */
juce::String ffmpegStatus();
} // namespace fc::video
