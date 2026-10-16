// Backend video con ffmpeg/ffprobe (usato su Windows): lettura delle informazioni, miniature, esportazione con il mix,
// estrazione dell'audio. Gli eseguibili si cercano accanto all'app, nella cartella "ffmpeg" e poi nel PATH.
#include "VideoSupport.h"
#include <map>

namespace fc::video
{
namespace
{
juce::String toolCommand (const juce::String& name)
{
    static juce::CriticalSection lock;
    static std::map<juce::String, juce::String> cache;
    const juce::ScopedLock sl (lock);
    if (auto it = cache.find (name); it != cache.end()) return it->second;

#if JUCE_WINDOWS
    const auto exe = name + ".exe";
#else
    const auto exe = name;
#endif
    const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    juce::String found = exe;   // altrimenti si affida al PATH
    for (auto d : { dir, dir.getChildFile ("ffmpeg"), dir.getChildFile ("bin"), dir.getParentDirectory().getChildFile ("Resources"),
                    juce::File ("/opt/homebrew/bin"), juce::File ("/usr/local/bin"), juce::File ("/usr/bin") })
        if (d.getChildFile (exe).existsAsFile()) { found = d.getChildFile (exe).getFullPathName(); break; }
    cache[name] = found;
    return found;
}

/** Esegue uno strumento e restituisce l'output (stdout, oppure stdout+stderr). -1 se non parte. */
int runTool (const juce::String& tool, juce::StringArray args, juce::String& output, bool withStdErr, int timeoutMs = 120000)
{
    args.insert (0, toolCommand (tool));
    juce::ChildProcess p;
    if (! p.start (args, withStdErr ? (juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr) : juce::ChildProcess::wantStdOut)) return -1;
    output = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish (timeoutMs)) { p.kill(); return -1; }
    return (int) p.getExitCode();
}

double parseRate (const juce::String& s)
{
    const int slash = s.indexOfChar ('/');
    if (slash < 0) return s.getDoubleValue();
    const double den = s.substring (slash + 1).getDoubleValue();
    return den > 0.0 ? s.substring (0, slash).getDoubleValue() / den : 0.0;
}
} // namespace

bool isVideoFile (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".mov" || ext == ".mp4" || ext == ".m4v" || ext == ".qt" || ext == ".mkv" || ext == ".avi" || ext == ".mxf";
}

bool usesExternalDecoder() { return true; }

bool decodeAudioNative (const juce::File&, double, juce::AudioBuffer<float>&, const std::function<void (float)>&, juce::String&) { return false; }   // qui si estrae con ffmpeg in un WAV

void setNativePlayerRate (juce::Component&, double) {}

juce::String ffmpegStatus()
{
    juce::String out;
    const bool ok = runTool ("ffmpeg", { "-version" }, out, false, 15000) == 0;
    return ok ? toolCommand ("ffmpeg") : juce::String();
}

Info probe (const juce::File& f)
{
    Info info;
    juce::String out;
    const int rc = runTool ("ffprobe", { "-v", "error", "-print_format", "json", "-show_streams", "-show_format", f.getFullPathName() }, out, false);
    if (rc < 0) { info.error = TRANS ("Non trovo ffprobe: serve ffmpeg accanto all'app o nel PATH."); return info; }
    const auto json = juce::JSON::parse (out);
    auto* streams = json.getProperty ("streams", {}).getArray();
    if (rc != 0 || streams == nullptr) { info.error = "Impossibile leggere " + f.getFileName() + "."; return info; }

    for (auto& s : *streams)
    {
        const auto type = s.getProperty ("codec_type", {}).toString();
        if (type == "audio") info.hasAudio = true;
        if (type != "video" || info.ok) continue;
        if ((int) s.getProperty ("disposition", {}).getProperty ("attached_pic", 0) == 1) continue;   // copertina
        info.width = (int) s.getProperty ("width", 0);
        info.height = (int) s.getProperty ("height", 0);
        double fps = parseRate (s.getProperty ("avg_frame_rate", "0").toString());
        if (fps < 1.0) fps = parseRate (s.getProperty ("r_frame_rate", "0").toString());
        info.fps = fps > 1.0 ? fps : 25.0;
        info.fpsKnown = fps > 1.0;
        // durata del contenitore (comprende l'eventuale ritardo iniziale del video): e' quella che conta per il montaggio
        double dur = juce::jmax (s.getProperty ("duration", "0").toString().getDoubleValue(),
                                 json.getProperty ("format", {}).getProperty ("duration", "0").toString().getDoubleValue());
        info.duration = dur;
        if (auto* side = s.getProperty ("side_data_list", {}).getArray())
            for (auto& sd : *side)
            {
                const int rot = std::abs ((int) sd.getProperty ("rotation", 0));
                if (rot == 90 || rot == 270) std::swap (info.width, info.height);   // ffmpeg ruota da solo i fotogrammi
            }
        info.ok = info.width > 0 && info.height > 0 && dur > 0.0;
    }
    if (! info.ok) info.error = TRANS ("Nessuna traccia video valida in ") + f.getFileName();
    return info;
}

std::vector<juce::Image> thumbnails (const juce::File& f, double interval, int count, int height, const std::function<void (float)>& progress)
{
    std::vector<juce::Image> out;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("adr_thumbs_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    if (! dir.createDirectory()) return out;

    juce::StringArray args { toolCommand ("ffmpeg"), "-v", "error", "-y", "-ss", juce::String (interval * 0.5, 3), "-i", f.getFullPathName(),
                             "-vf", "fps=1/" + juce::String (interval, 4) + ",scale=-2:" + juce::String (height),
                             "-frames:v", juce::String (count), "-q:v", "5", dir.getChildFile ("t_%05d.jpg").getFullPathName() };
    {
        juce::ChildProcess p;
        if (p.start (args, juce::ChildProcess::wantStdErr))
        {
            // l'avanzamento si legge dai file prodotti
            const auto deadline = juce::Time::getMillisecondCounter() + 600000;
            while (p.isRunning() && juce::Time::getMillisecondCounter() < deadline)
            {
                if (progress) progress ((float) juce::jmin (1.0, (double) dir.getNumberOfChildFiles (juce::File::findFiles) / (double) juce::jmax (1, count)));
                juce::Thread::sleep (150);
            }
            p.kill();
        }
    }
    if (progress) progress (1.0f);

    for (int i = 1; i <= count; ++i)
    {
        const auto img = juce::ImageFileFormat::loadFrom (dir.getChildFile ("t_" + juce::String (i).paddedLeft ('0', 5) + ".jpg"));
        out.push_back (img.isValid() ? img : (out.empty() ? juce::Image() : out.back()));
    }
    dir.deleteRecursively();
    return out;
}

bool extractAudio (const juce::File& video, const juce::File& wavOut, juce::String& error)
{
    juce::String log;
    wavOut.deleteFile();
    const int rc = runTool ("ffmpeg", { "-v", "error", "-y", "-i", video.getFullPathName(), "-vn", "-map", "0:a:0", "-c:a", "pcm_f32le", wavOut.getFullPathName() }, log, true, 600000);
    if (rc != 0 || ! wavOut.existsAsFile()) { error = "Non riesco a estrarre l'audio dal video: " + log.trim().upToFirstOccurrenceOf ("\n", false, false); return false; }
    return true;
}

bool detectCuts (const juce::File& video, double sensitivity, const std::function<bool (double)>& progress, std::vector<double>& cuts, juce::String& error)
{
    cuts.clear();
    const double thr = 0.55 - 0.40 * juce::jlimit (0.0, 1.0, sensitivity);
    juce::String output;
    const auto filter = "select='gt(scene," + juce::String (thr, 3) + ")',showinfo";
    if (runTool ("ffmpeg", { "-hide_banner", "-nostats", "-i", video.getFullPathName(), "-vf", filter, "-an", "-f", "null", "-" }, output, true, 3600000) < 0)
    {
        error = TRANS ("Non trovo ffmpeg: serve accanto all'app o nel PATH.");
        return false;
    }
    for (auto line : juce::StringArray::fromLines (output))
        if (line.contains ("pts_time:"))
        {
            const double t = line.fromFirstOccurrenceOf ("pts_time:", false, false).upToFirstOccurrenceOf (" ", false, false).getDoubleValue();
            if (cuts.empty() || t - cuts.back() > 0.5) cuts.push_back (t);
        }
    if (progress) progress (1.0);
    return true;
}

bool burnInSupported() { return false; }

bool muxAudioBurnIn (const juce::File&, const juce::File&, const juce::File&, double, const BurnIn&, const std::function<bool (double)>&, juce::String& error, double, double)
{
    error = TRANS ("Il burn-in e' disponibile solo su macOS.");
    return false;
}

bool muxAudio (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec,
               const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec, double rangeEndSec)
{
    // con un intervallo la composizione parte da rangeStartSec: il tempo nel video e' (tempo timeline - offset)
    const bool ranged = rangeEndSec > rangeStartSec;
    const double rs = ranged ? rangeStartSec : 0.0;
    const double srcStart = juce::jmax (0.0, rs - videoOffsetSec), atSec = juce::jmax (0.0, videoOffsetSec - rs);
    const auto info = probe (video);
    if (! info.ok) { error = info.error; return false; }
    double total = ranged ? rangeEndSec - rangeStartSec : info.duration + atSec;
    if (ranged && srcStart >= info.duration) { error = TRANS ("L'intervallo scelto non contiene video."); return false; }

    juce::StringArray args { toolCommand ("ffmpeg"), "-v", "error", "-y", "-nostats", "-progress", "pipe:1" };
    if (srcStart > 0.0) { args.add ("-ss"); args.add (juce::String (srcStart, 4)); }
    if (atSec > 0.0) { args.add ("-itsoffset"); args.add (juce::String (atSec, 4)); }
    args.addArray ({ "-i", video.getFullPathName(), "-i", audio.getFullPathName(), "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "pcm_s24le" });
    if (ranged) { args.add ("-t"); args.add (juce::String (rangeEndSec - rangeStartSec, 4)); }
    args.addArray ({ "-shortest", out.getFullPathName() });

    out.deleteFile();
    juce::ChildProcess p;
    if (! p.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) { error = TRANS ("Non trovo ffmpeg: serve accanto all'app o nel PATH."); return false; }

    juce::String pending, errors;
    bool cancelled = false;
    char buf[2048];
    for (;;)
    {
        const int n = p.readProcessOutput (buf, (int) sizeof (buf) - 1);
        if (n <= 0) break;
        pending += juce::String::fromUTF8 (buf, n);
        int nl;
        while ((nl = pending.indexOfChar ('\n')) >= 0)
        {
            const auto line = pending.substring (0, nl).trim();
            pending = pending.substring (nl + 1);
            if (line.startsWith ("out_time_us=") || line.startsWith ("out_time_ms="))
            {
                const double sec = line.fromFirstOccurrenceOf ("=", false, false).getDoubleValue() / 1.0e6;
                if (progress && ! progress (juce::jlimit (0.0, 1.0, sec / juce::jmax (0.1, total)))) { cancelled = true; p.kill(); break; }
            }
            else if (! line.contains ("=") || line.startsWithIgnoreCase ("error")) errors << line << "\n";
        }
        if (cancelled) break;
    }
    p.waitForProcessToFinish (10000);
    if (cancelled) { out.deleteFile(); error = TRANS ("Esportazione annullata."); return false; }
    if (p.getExitCode() != 0 || ! out.existsAsFile())
    {
        out.deleteFile();
        error = TRANS ("Esportazione del video non riuscita.") + (errors.isNotEmpty() ? "\n" + errors.trim() : juce::String());
        return false;
    }
    return true;
}
} // namespace fc::video
