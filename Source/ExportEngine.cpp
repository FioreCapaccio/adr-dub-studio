#include "ExportEngine.h"

namespace fc
{
namespace
{
struct Measure
{
    float peak = 0.0f, truePeak = 0.0f;
    double lufs = -100.0, lra = 0.0;
};

/** Mette i plug-in della sessione in modalita' offline e li riporta in tempo reale alla fine. */
struct OfflinePlugins
{
    std::vector<std::shared_ptr<InsertSlot>> plugins;

    explicit OfflinePlugins (const Session& s)
    {
        auto collect = [this] (const TrackLive& l)
        {
            if (auto c = l.getChain()) for (auto& sl : c->slots) if (sl->ready.load() && sl->plugin != nullptr) plugins.push_back (sl);
        };
        for (auto& t : s.tracks) collect (*t.live);
        collect (*s.master);
        for (auto& x : s.aux) collect (*x);
        for (auto& x : s.group) collect (*x);
        for (auto& sl : plugins) { sl->plugin->reset(); sl->plugin->setNonRealtime (true); }
    }
    ~OfflinePlugins() { for (auto& sl : plugins) { sl->plugin->setNonRealtime (false); sl->plugin->reset(); } }
};

std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::String& format, const juce::File& file, double rate, int bits,
                                                     juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
    if (out == nullptr) { error = "Impossibile creare " + file.getFullPathName(); return nullptr; }

    std::unique_ptr<juce::AudioFormat> fmt;
    if (format == "aiff") fmt = std::make_unique<juce::AiffAudioFormat>();
    else if (format == "flac") { fmt = std::make_unique<juce::FlacAudioFormat>(); bits = juce::jmin (bits, 24); }
    else fmt = std::make_unique<juce::WavAudioFormat>();

    std::unique_ptr<juce::AudioFormatWriter> w (fmt->createWriterFor (out.get(), rate, 2, bits, {}, 0));
    if (w == nullptr) { error = "Formato o profondita' di bit non supportati."; return nullptr; }
    out.release();
    // il writer possiede lo stream, ma non il formato: lo teniamo vivo ricreandolo e' superfluo (nessuno stato condiviso)
    return w;
}

/** Fase 1: renderizza (mixer + eventualmente bus master) in un file temporaneo float a 32 bit alla frequenza della sessione. */
bool renderToTemp (const Session& source, double rate, int64_t start, int64_t total, bool withMaster, const juce::File& temp,
                   Measure* measure, const std::function<bool (double)>& progress, juce::String& error)
{
    Session s = source.cloneDetached();
    OfflinePlugins plugins (s);

    std::unique_ptr<juce::AudioFormatWriter> writer = makeWriter ("wav", temp, rate, 32, error);
    if (writer == nullptr) return false;

    Mixer mixer;
    MasterBus bus;
    bus.prepare (rate);
    Loudness::Meter meter;
    meter.prepare (rate);

    const int skip = Mixer::computeLatency (s) + (withMaster ? bus.latency (*s.master) : 0);
    const int64_t all = total + skip;
    juce::AudioBuffer<float> buf (2, Mixer::kChunk);
    float peak = 0.0f;

    for (int64_t i = 0; i < all; i += Mixer::kChunk)
    {
        const int n = (int) juce::jmin<int64_t> (Mixer::kChunk, all - i);
        mixer.render (s, start + i, buf, 0, n);
        if (withMaster) bus.process (*s.master, buf, n);

        const int first = (int) juce::jmax<int64_t> (0, (int64_t) skip - i);
        if (first < n)
        {
            if (measure != nullptr)
            {
                peak = juce::jmax (peak, buf.getMagnitude (first, n - first));
                meter.process (buf.getReadPointer (0) + first, buf.getReadPointer (1) + first, n - first);
            }
            writer->writeFromAudioSampleBuffer (buf, first, n - first);
        }
        if (progress && ! progress ((double) i / (double) all)) { error = "Esportazione annullata."; return false; }
    }
    writer.reset();

    if (measure != nullptr)
    {
        measure->peak = peak;
        measure->truePeak = meter.truePeak.load();
        measure->lufs = (double) meter.integrated.load();
        measure->lra = (double) meter.lra.load();
    }
    return true;
}

/** Fase 2: legge il file temporaneo, applica guadagno / limiter / conversione di frequenza / dither e scrive il file finale. */
bool convertTemp (const juce::File& temp, double rate, const ExportOptions& o, double gainLin, bool useLimiter, double ceilingLin,
                  const juce::File& dest, double progressBase, double progressSpan,
                  const std::function<bool (double)>& progress, juce::String& error, double& limiterGr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (temp));
    if (reader == nullptr) { error = "File temporaneo non leggibile."; return false; }

    const double outRate = o.outRate > 0.0 ? o.outRate : rate;
    auto writer = makeWriter (o.format, dest, outRate, o.bitDepth, error);
    if (writer == nullptr) return false;

    const int64_t inTotal = reader->lengthInSamples;
    const bool resample = std::abs (outRate - rate) > 0.5;
    const double ratio = rate / outRate;
    const int64_t outTotal = (int64_t) std::llround ((double) inTotal / ratio);

    Limiter limiter;
    limiter.prepare (rate);
    const int latency = useLimiter ? limiter.latency() : 0;

    juce::WindowedSincInterpolator interp[2];
    const int numOut = 2048;
    const size_t needIn = (size_t) std::ceil ((double) numOut * ratio) + 8;
    int64_t dropOut = resample ? (int64_t) std::llround (interp[0].getBaseLatency() / ratio) : 0;   // latenza del filtro di conversione

    std::vector<float> pending[2];
    juce::Random rng (0x5eed);
    const bool dither = o.dither && o.bitDepth == 16;
    const float lsb = 1.0f / 32768.0f;

    juce::AudioBuffer<float> in (2, 4096), out (2, 4096);
    int64_t readPos = 0, written = 0, fed = 0, dropIn = latency;
    const int64_t inEnd = inTotal + latency;
    double grMax = 0.0;
    bool inputDone = false;

    auto emit = [&] (juce::AudioBuffer<float>& b, int n)
    {
        int from = 0;
        if (dropOut > 0) { from = (int) juce::jmin<int64_t> (dropOut, n); dropOut -= from; }
        const int cnt = (int) juce::jmin<int64_t> (n - from, outTotal - written);
        if (cnt <= 0) return;
        if (dither)
            for (int c = 0; c < 2; ++c)
            {
                float* d = b.getWritePointer (c) + from;
                for (int i = 0; i < cnt; ++i) d[i] += (rng.nextFloat() - rng.nextFloat()) * lsb;
            }
        writer->writeFromAudioSampleBuffer (b, from, cnt);
        written += cnt;
    };

    while (written < outTotal)
    {
        // 1) altro materiale in ingresso (con guadagno e limiter)
        if (! inputDone && (! resample || pending[0].size() < needIn))
        {
            const int avail = (int) juce::jmin<int64_t> (4096, inEnd - fed);
            if (avail <= 0) inputDone = true;
            else
            {
                in.clear();
                const int realAvail = (int) juce::jmax<int64_t> (0, juce::jmin<int64_t> (avail, inTotal - readPos));
                if (realAvail > 0) reader->read (&in, 0, realAvail, readPos, true, true);
                readPos += realAvail;
                fed += avail;
                in.applyGain (0, avail, (float) gainLin);
                if (useLimiter) grMax = juce::jmax (grMax, (double) limiter.process (in, 0, avail, (float) ceilingLin));

                int from = 0, cnt = avail;
                if (dropIn > 0) { from = (int) juce::jmin<int64_t> (dropIn, cnt); dropIn -= from; cnt -= from; }
                if (cnt > 0)
                {
                    if (! resample)
                    {
                        for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, from, cnt);
                        emit (out, cnt);
                    }
                    else
                        for (int c = 0; c < 2; ++c)
                            pending[c].insert (pending[c].end(), in.getReadPointer (c) + from, in.getReadPointer (c) + from + cnt);
                }
            }
        }

        // 2) conversione di frequenza
        if (resample)
        {
            if (pending[0].size() < needIn)
            {
                if (! inputDone) continue;                                           // serve altro ingresso
                for (int c = 0; c < 2; ++c) pending[c].resize (needIn, 0.0f);       // fine: si completa con silenzio
            }
            int used = 0;
            for (int c = 0; c < 2; ++c) used = interp[c].process (ratio, pending[c].data(), out.getWritePointer (c), numOut);
            for (int c = 0; c < 2; ++c) pending[c].erase (pending[c].begin(), pending[c].begin() + juce::jmin ((size_t) used, pending[c].size()));
            emit (out, numOut);
        }
        else if (inputDone)
            break;

        if (progress && ! progress (progressBase + progressSpan * (double) juce::jmin<int64_t> (written, outTotal) / (double) juce::jmax<int64_t> (1, outTotal)))
        {
            error = "Esportazione annullata.";
            writer.reset();
            dest.deleteFile();
            return false;
        }
    }
    writer.reset();
    limiterGr = grMax;
    return true;
}

} // namespace

//==============================================================================
ExportResult exportSession (const Session& session, double rate, const ExportOptions& o, const juce::File& target,
                            const juce::String& baseName, const std::function<bool (double)>& progress)
{
    ExportResult res;
    const int64_t end = session.endSample();
    int64_t start = 0, total = end + (int64_t) (0.5 * rate);
    if (o.loopRange)
    {
        start = juce::jmax<int64_t> (0, o.rangeStart);
        total = o.rangeEnd - start;
    }
    if (total <= 0) { res.message = "L'intervallo da esportare e' vuoto."; return res; }

    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto ext = "." + o.format;

    //---------------------------------------------------------------- stem
    if (o.stems)
    {
        target.createDirectory();
        int index = 0, count = 0;
        for (auto& t : session.tracks) if (! t.clips.empty()) ++count;
        if (count == 0) { res.message = "Nessuna traccia con clip da esportare."; return res; }

        for (size_t ti = 0; ti < session.tracks.size(); ++ti)
        {
            if (session.tracks[ti].clips.empty()) continue;
            Session s = session.cloneDetached();
            for (size_t k = 0; k < s.tracks.size(); ++k) { s.tracks[k].live->mute = (k != ti); s.tracks[k].live->solo = false; }

            const auto temp = tempDir.getNonexistentChildFile ("arotron_stem", ".wav");
            juce::String err;
            const double base = (double) index / (double) count, span = 1.0 / (double) count;
            auto sub = [&] (double p) { return ! progress || progress (base + span * 0.6 * p); };
            if (! renderToTemp (s, rate, start, total, false, temp, nullptr, sub, err)) { temp.deleteFile(); res.message = err; return res; }

            const auto name = juce::File::createLegalFileName (baseName + "_" + juce::String (++index).paddedLeft ('0', 2) + "_" + session.tracks[ti].name);
            const auto dest = target.getChildFile (name + ext);
            double gr = 0.0;
            auto sub2 = [&] (double p) { return ! progress || progress (p); };
            ExportOptions oo = o;
            if (! convertTemp (temp, rate, oo, 1.0, false, 1.0, dest, base + span * 0.6, span * 0.4, sub2, err, gr)) { temp.deleteFile(); res.message = err; return res; }
            temp.deleteFile();
            res.files.add (dest.getFullPathName());
        }
        res.ok = true;
        res.message = juce::String (res.files.size()) + " stem esportati in " + target.getFullPathName();
        return res;
    }

    //---------------------------------------------------------------- mixdown
    const auto temp = tempDir.getNonexistentChildFile ("arotron_mix", ".wav");
    Measure m;
    juce::String err;
    const bool needsMeasure = true;   // le misure servono sempre per il resoconto
    auto p1 = [&] (double p) { return ! progress || progress (p * 0.55); };
    if (! renderToTemp (session, rate, start, total, true, temp, needsMeasure ? &m : nullptr, p1, err))
    {
        temp.deleteFile();
        res.message = err;
        return res;
    }

    // guadagno di normalizzazione
    double gainDb = 0.0;
    const double peakDb = juce::Decibels::gainToDecibels (m.peak, -120.0f);
    const double tpDb = juce::Decibels::gainToDecibels (m.truePeak, -120.0f);
    if (o.norm == ExportOptions::Norm::Peak && m.peak > 0.0f)
        gainDb = o.normTarget - peakDb;
    else if (o.norm == ExportOptions::Norm::Lufs && m.lufs > -99.0)
        gainDb = o.normTarget - m.lufs;

    // tetto: dopo la normalizzazione a LUFS un limiter true-peak impedisce di superare il ceiling
    const double ceiling = juce::jmin (o.ceilingDb, -0.1);
    const bool useLimiter = o.norm == ExportOptions::Norm::Lufs && (tpDb + gainDb) > ceiling;
    double gr = 0.0;

    auto p2 = [&] (double p) { return ! progress || progress (p); };
    if (! convertTemp (temp, rate, o, juce::Decibels::decibelsToGain ((float) gainDb), useLimiter,
                       (double) juce::Decibels::decibelsToGain ((float) ceiling), target, 0.55, 0.45, p2, err, gr))
    {
        temp.deleteFile();
        res.message = err;
        return res;
    }
    temp.deleteFile();

    res.ok = true;
    res.files.add (target.getFullPathName());
    res.gainDb = gainDb;
    res.limiterGrDb = gr;
    res.peakDb = peakDb + gainDb - (useLimiter ? gr : 0.0);
    res.truePeakDb = juce::jmin (tpDb + gainDb, useLimiter ? ceiling : 1000.0);
    res.lufs = m.lufs > -99.0 ? m.lufs + gainDb - (useLimiter ? gr * 0.0 : 0.0) : -100.0;
    res.lra = m.lra;

    juce::String msg = "Esportato in " + target.getFullPathName();
    msg << "\nLoudness integrato: " << (res.lufs > -99.0 ? juce::String (res.lufs, 1) + " LUFS" : juce::String ("n/d"))
        << "   LRA: " << juce::String (res.lra, 1) << " LU"
        << "\nPicco: " << juce::String (res.peakDb, 1) << " dBFS   True-peak: " << juce::String (res.truePeakDb, 1) << " dBTP";
    if (std::abs (gainDb) > 0.005) msg << "\nGuadagno di normalizzazione: " << (gainDb >= 0 ? "+" : "") << juce::String (gainDb, 1) << " dB";
    if (useLimiter) msg << "\nLimiter true-peak intervenuto (riduzione massima " << juce::String (gr, 1) << " dB)";
    if (res.peakDb > 0.0) msg << "\nATTENZIONE: il picco supera 0 dBFS, il file puo' distorcere.";
    res.message = msg;
    return res;
}

} // namespace fc
