#pragma once

#include <JuceHeader.h>
#include <deque>
#include <memory>

namespace fc
{
/** Lettore video senza audio basato su ffmpeg, con la stessa interfaccia ridotta di juce::VideoComponent che usa il viewer.
    Un thread legge fotogrammi grezzi da ffmpeg: in riproduzione uno stream continuo a partire dalla posizione richiesta,
    da fermo un singolo fotogramma. La posizione segue l'orologio: il viewer la riallinea all'audio. */
class FfmpegFramePlayer : public juce::Component, private juce::Timer
{
public:
    explicit FfmpegFramePlayer (bool);
    ~FfmpegFramePlayer() override;

    void loadAsync (const juce::URL&, std::function<void (const juce::URL&, juce::Result)> done);
    void closeVideo();
    void setAudioVolume (float) {}
    void setPlaySpeed (double) {}   // l'orologio segue l'audio: nessuna regolazione di velocita'
    void play();
    void stop();
    bool isPlaying() const noexcept { return playing; }
    void setPlayPosition (double seconds);
    double getPlayPosition() const;

    void paint (juce::Graphics&) override;

private:
    struct Frame { juce::Image image; double pts = 0.0, arrival = 0.0; };
    class Decoder;

    void timerCallback() override;
    void request (bool stream, double pos);
    static double wallClock() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    juce::File file;
    int outW = 0, outH = 0;
    double fps = 25.0, duration = 0.0;
    bool playing = false;
    double basePos = 0.0, baseWall = 0.0, lastRequestedPos = -1.0;
    double streamStart = -1.0;   // posizione da cui parte lo stream in corso (-1 = nessuno)
    double startLead = 0.4;      // quanto in anticipo si chiede lo stream: il tempo che ffmpeg impiega a produrre il primo fotogramma (si impara)
    double playStartWall = 0.0;
    bool measuringStart = false;
    Frame current;

    juce::CriticalSection lock;
    std::deque<Frame> queue;      // protetta da 'lock'
    std::unique_ptr<Decoder> decoder;
    uint32_t generation = 0;      // incrementa a ogni nuova richiesta: i fotogrammi vecchi si scartano
    bool loaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FfmpegFramePlayer)
};

} // namespace fc
