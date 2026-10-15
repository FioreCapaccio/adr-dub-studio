#include "FfmpegFramePlayer.h"
#include "VideoSupport.h"

namespace fc
{
namespace
{
juce::String ffmpegCommand()
{
#if JUCE_WINDOWS
    const juce::String exe = "ffmpeg.exe";
#else
    const juce::String exe = "ffmpeg";
#endif
    const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    for (auto d : { dir, dir.getChildFile ("ffmpeg"), dir.getChildFile ("bin"), juce::File ("/opt/homebrew/bin"), juce::File ("/usr/local/bin"), juce::File ("/usr/bin") })
        if (d.getChildFile (exe).existsAsFile()) return d.getChildFile (exe).getFullPathName();
    return exe;
}
} // namespace

//==============================================================================
/** Thread che esegue ffmpeg per l'ultima richiesta ricevuta e consegna i fotogrammi decodificati. */
class FfmpegFramePlayer::Decoder : public juce::Thread
{
public:
    Decoder (FfmpegFramePlayer& o, juce::File f, int w, int h, double r) : juce::Thread ("ADR video"), owner (o), file (std::move (f)), width (w), height (h), fps (r) {}
    ~Decoder() override
    {
        signalThreadShouldExit();
        post (false, -1.0, 0);   // interrompe ffmpeg se sta leggendo
        stopThread (3000);
    }

    void post (bool stream, double pos, uint32_t gen)
    {
        {
            const juce::ScopedLock sl (reqLock);
            reqStream = stream; reqPos = pos; reqGen = gen; hasRequest = true;
            if (running != nullptr) running->kill();   // interrompe subito la lettura in corso
        }
        wake.signal();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            bool stream; double pos; uint32_t gen;
            {
                const juce::ScopedLock sl (reqLock);
                if (! hasRequest) { stream = false; pos = 0; gen = 0; }
                else { stream = reqStream; pos = reqPos; gen = reqGen; hasRequest = false; }
                if (gen == 0) { wake.wait (200); continue; }
            }
            decode (stream, pos, gen);
        }
    }

private:
    bool superseded (uint32_t gen) const { const juce::ScopedLock sl (reqLock); return hasRequest || reqGen != gen; }

    void decode (bool stream, double pos, uint32_t gen)
    {
        // prima si prova con l'accelerazione hardware (se c'e' e' molto piu' leggera per i video 4K), poi senza
        juce::StringArray args { ffmpegCommand(), "-v", "error" };
        if (useHardware) args.add ("-hwaccel"), args.add ("auto");
        args.addArray ({ "-ss", juce::String (pos, 4), "-i", file.getFullPathName(), "-an", "-sn",
                         "-vf", "scale=" + juce::String (width) + ":" + juce::String (height) + ":flags=fast_bilinear", "-pix_fmt", "bgra" });
        if (! stream) { args.add ("-frames:v"); args.add ("1"); }
        args.addArray ({ "-f", "rawvideo", "pipe:1" });

        juce::ChildProcess proc;
        {
            const juce::ScopedLock sl (reqLock);
            if (hasRequest) return;   // gia' sostituita
            if (! proc.start (args, juce::ChildProcess::wantStdOut)) return;
            running = &proc;
        }

        const size_t bytes = (size_t) width * (size_t) height * 4;
        std::vector<char> buf (bytes);
        for (int k = 0;; ++k)
        {
            size_t got = 0;
            while (got < bytes)
            {
                const int n = proc.readProcessOutput (buf.data() + got, (int) juce::jmin<size_t> (bytes - got, 1 << 20));
                if (n <= 0) break;
                got += (size_t) n;
            }
            if (got < bytes && k == 0 && useHardware && ! threadShouldExit() && ! superseded (gen))
            {
                useHardware = false;   // l'accelerazione non parte su questo sistema: si riprova in software
                proc.kill();
                { const juce::ScopedLock sl (reqLock); running = nullptr; }
                decode (stream, pos, gen);
                return;
            }
            if (got < bytes || threadShouldExit() || superseded (gen)) break;

            juce::Image img (juce::Image::ARGB, width, height, false);
            {
                juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < height; ++y) std::memcpy (bd.getLinePointer (y), buf.data() + (size_t) y * (size_t) width * 4, (size_t) width * 4);
            }
            // consegna; in streaming si aspetta se il lettore e' avanti di qualche fotogramma
            for (;;)
            {
                {
                    const juce::ScopedLock sl (owner.lock);
                    if (owner.generation != gen) goto finished;
                    if (! stream) owner.queue.clear();
                    if (! stream || owner.queue.size() < 10)
                    {
                        owner.queue.push_back ({ img, pos + (double) k / fps, wallClock() });
                        break;
                    }
                }
                if (threadShouldExit() || superseded (gen)) goto finished;
                juce::Thread::sleep (6);
            }
            if (! stream) break;
        }
    finished:
        {
            const juce::ScopedLock sl (reqLock);
            running = nullptr;
        }
        proc.kill();
    }

    FfmpegFramePlayer& owner;
    juce::File file;
    int width, height;
    double fps;
    mutable juce::CriticalSection reqLock;
    juce::WaitableEvent wake;
    bool reqStream = false, hasRequest = false;
    double reqPos = 0.0;
    uint32_t reqGen = 0;
    juce::ChildProcess* running = nullptr;
    bool useHardware = true;
};

//==============================================================================
FfmpegFramePlayer::FfmpegFramePlayer (bool) {}

FfmpegFramePlayer::~FfmpegFramePlayer() { stopTimer(); decoder.reset(); }

void FfmpegFramePlayer::loadAsync (const juce::URL& url, std::function<void (const juce::URL&, juce::Result)> done)
{
    closeVideo();
    const auto f = url.getLocalFile();
    juce::Component::SafePointer<FfmpegFramePlayer> safe (this);
    std::thread ([safe, f, url, done = std::move (done)]
    {
        const auto info = video::probe (f);
        juce::MessageManager::callAsync ([safe, f, url, info, done]
        {
            if (safe == nullptr) return;
            if (! info.ok) { if (done) done (url, juce::Result::fail (info.error)); return; }
            auto& p = *safe;
            p.file = f;
            p.fps = info.fps;
            p.duration = info.duration;
            // risoluzione di decodifica: al massimo 1280 di larghezza, dimensioni pari
            const double sc = juce::jmin (1.0, 1280.0 / (double) info.width);
            p.outW = juce::jmax (2, ((int) std::lround (info.width * sc)) & ~1);
            p.outH = juce::jmax (2, ((int) std::lround (info.height * sc)) & ~1);
            p.decoder = std::make_unique<Decoder> (p, f, p.outW, p.outH, p.fps);
            p.decoder->startThread();
            p.loaded = true;
            p.startTimerHz (60);
            if (done) done (url, juce::Result::ok());
        });
    }).detach();
}

void FfmpegFramePlayer::closeVideo()
{
    stopTimer();
    decoder.reset();
    {
        const juce::ScopedLock sl (lock);
        queue.clear();
        ++generation;
    }
    current = {};
    loaded = false;
    playing = false;
    basePos = 0.0;
    lastRequestedPos = -1.0;
    streamStart = -1.0;
    repaint();
}

void FfmpegFramePlayer::request (bool stream, double pos)
{
    if (decoder == nullptr) return;
    uint32_t gen;
    {
        const juce::ScopedLock sl (lock);
        queue.clear();
        gen = ++generation;
    }
    streamStart = stream ? pos : -1.0;
    decoder->post (stream, pos, gen);
}

double FfmpegFramePlayer::getPlayPosition() const
{
    return playing ? juce::jmin (duration, basePos + (wallClock() - baseWall)) : basePos;
}

void FfmpegFramePlayer::play()
{
    if (! loaded || playing) return;
    playing = true;
    baseWall = wallClock();
    playStartWall = baseWall;
    measuringStart = true;
    request (true, basePos + startLead);
}

void FfmpegFramePlayer::stop()
{
    if (! playing) return;
    basePos = getPlayPosition();
    playing = false;
    streamStart = -1.0;
    if (decoder != nullptr)
    {
        const juce::ScopedLock sl (lock);
        queue.clear();
        ++generation;   // ferma lo stream: l'ultimo fotogramma resta a video
    }
    if (decoder != nullptr) decoder->post (false, -1.0, 0);   // gen 0 = nessun lavoro; interrompe ffmpeg
}

void FfmpegFramePlayer::setPlayPosition (double seconds)
{
    if (! loaded) return;
    seconds = juce::jlimit (0.0, duration, seconds);
    if (playing)
    {
        // piccola correzione: si riancora l'orologio; salto grande o all'indietro: nuovo stream
        const double now = getPlayPosition();
        if (seconds < now - 0.3 || seconds > now + 1.0) { basePos = seconds; baseWall = wallClock(); playStartWall = baseWall; measuringStart = true; request (true, seconds + startLead); }
        else { basePos = seconds; baseWall = wallClock(); }
        return;
    }
    basePos = seconds;
    if (std::abs (seconds - lastRequestedPos) > 0.5 / juce::jmax (1.0, fps))
    {
        lastRequestedPos = seconds;
        request (false, seconds);
    }
}

void FfmpegFramePlayer::timerCallback()
{
    const double pos = getPlayPosition();
    bool changed = false;
    {
        const juce::ScopedLock sl (lock);
        if (playing)
        {
            while (queue.size() > 1 && queue[1].pts <= pos + 0.001) queue.pop_front();   // si salta quello che e' gia' passato
            if (! queue.empty() && queue.front().pts <= pos + 0.02)
            {
                current = queue.front();
                queue.pop_front();
                changed = true;
                if (measuringStart)   // ritardo di ffmpeg per il primo fotogramma: la prossima partenza lo anticipa
                {
                    measuringStart = false;
                    startLead = juce::jlimit (0.05, 2.0, 0.5 * startLead + 0.5 * (current.arrival - playStartWall + 0.03));
                }
            }
        }
        else if (! queue.empty()) { current = queue.back(); queue.clear(); changed = true; }
    }
    if (changed) repaint();
}

void FfmpegFramePlayer::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    if (current.image.isValid()) g.drawImage (current.image, getLocalBounds().toFloat(), juce::RectanglePlacement::centred);
}

} // namespace fc
