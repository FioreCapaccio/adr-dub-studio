#include "ExportEngine.h"
#include "TrackPresets.h"

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
        if (progress && ! progress ((double) i / (double) all)) { error = TRANS ("Esportazione annullata."); return false; }
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
            error = TRANS ("Esportazione annullata.");
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
namespace
{
struct MixInfo
{
    Measure m;
    double gainDb = 0.0, gr = 0.0, peakDb = -120.0, tpDb = -120.0, lufs = -100.0, ceiling = -1.0;
    bool limited = false;
};

/** Mixdown con bus master verso 'dest', con misura, normalizzazione e limiter di true-peak. base/span: parte della barra di avanzamento. */
bool doMixdown (const Session& s, double rate, const ExportOptions& o, int64_t start, int64_t total, const juce::File& dest, double base, double span,
                const std::function<bool (double)>& progress, MixInfo& mi, juce::String& err)
{
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("arotron_mix", ".wav");
    auto p1 = [&] (double p) { return ! progress || progress (base + span * 0.55 * p); };
    if (! renderToTemp (s, rate, start, total, true, temp, &mi.m, p1, err)) { temp.deleteFile(); return false; }

    mi.peakDb = juce::Decibels::gainToDecibels (mi.m.peak, -120.0f);
    mi.tpDb = juce::Decibels::gainToDecibels (mi.m.truePeak, -120.0f);
    if (o.norm == ExportOptions::Norm::Peak && mi.m.peak > 0.0f) mi.gainDb = o.normTarget - mi.peakDb;
    else if (o.norm == ExportOptions::Norm::Lufs && mi.m.lufs > -99.0) mi.gainDb = o.normTarget - mi.m.lufs;

    // tetto: dopo la normalizzazione a LUFS un limiter true-peak impedisce di superare il ceiling
    mi.ceiling = juce::jmin (o.ceilingDb, -0.1);
    mi.limited = o.norm == ExportOptions::Norm::Lufs && (mi.tpDb + mi.gainDb) > mi.ceiling;

    auto p2 = [&] (double p) { return ! progress || progress (p); };
    const bool ok = convertTemp (temp, rate, o, juce::Decibels::decibelsToGain ((float) mi.gainDb), mi.limited,
                                 (double) juce::Decibels::decibelsToGain ((float) mi.ceiling), dest, base + span * 0.55, span * 0.45, p2, err, mi.gr);
    temp.deleteFile();
    if (! ok) return false;
    mi.lufs = mi.m.lufs > -99.0 ? mi.m.lufs + mi.gainDb : -100.0;
    return true;
}

/** Stem gia' preparato (le altre tracce sono in mute) senza bus master, con un guadagno fisso (1 = nessuno). */
bool doStem (const Session& s, double rate, const ExportOptions& o, int64_t start, int64_t total, double gainLin, const juce::File& dest, double base, double span,
             const std::function<bool (double)>& progress, Measure* measure, juce::String& err)
{
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("arotron_stem", ".wav");
    auto sub = [&] (double p) { return ! progress || progress (base + span * 0.6 * p); };
    if (! renderToTemp (s, rate, start, total, false, temp, measure, sub, err)) { temp.deleteFile(); return false; }
    double gr = 0.0;
    auto sub2 = [&] (double p) { return ! progress || progress (p); };
    const bool ok = convertTemp (temp, rate, o, gainLin, false, 1.0, dest, base + span * 0.6, span * 0.4, sub2, err, gr);
    temp.deleteFile();
    return ok;
}

juce::String htmlEsc (const juce::String& s) { return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;"); }

juce::String reportHtml (const ExportOptions& o, const std::vector<ExportItem>& items, const MixInfo& mi, int unrouted, double seconds, const juce::String& baseName)
{
    juce::String h;
    h << TRANS ("<!DOCTYPE html><html lang=\"it\"><head><meta charset=\"utf-8\"><title>Rapporto di consegna</title><style>"
         "body{font-family:-apple-system,Helvetica,Arial,sans-serif;color:#111;margin:24px}h1{font-size:21px;margin:0 0 4px}.meta{color:#555;font-size:12px;margin-bottom:14px}"
         "table{border-collapse:collapse;width:100%;font-size:13px}th,td{border:1px solid #bbb;padding:6px 8px;text-align:left}th{background:#eee}td.r{text-align:right;font-variant-numeric:tabular-nums}"
         ".warn{color:#a30;margin-top:12px;font-size:13px}</style></head><body><h1>Rapporto di consegna - ") << htmlEsc (baseName) << TRANS ("</h1><div class=\"meta\">")
      << htmlEsc (juce::Time::getCurrentTime().formatted ("%d/%m/%Y %H:%M")) << " &middot; " << o.format.toUpperCase() << " " << o.bitDepth << " bit &middot; "
      << (o.outRate > 0.0 ? juce::String (o.outRate / 1000.0, 1) + " kHz" : juce::String (TRANS ("frequenza della sessione"))) << TRANS (" &middot; durata ") << juce::String (seconds, 1) << " s"
      << (o.presetName.isNotEmpty() ? TRANS (" &middot; preset: ") + htmlEsc (o.presetName) : juce::String()) << "</div>"
      << TRANS ("<table><tr><th>File</th><th>Picco (dBFS)</th><th>True peak (dBTP)</th><th>Loudness (LUFS)</th><th>LRA (LU)</th></tr>");
    for (auto& it : items)
        h << "<tr><td>" << htmlEsc (juce::File (it.file).getFileName()) << TRANS ("</td><td class=\"r\">") << juce::String (it.peakDb, 1) << TRANS ("</td><td class=\"r\">") << juce::String (it.truePeakDb, 1)
          << TRANS ("</td><td class=\"r\">") << (it.lufs > -99.0 ? juce::String (it.lufs, 1) : juce::String ("n/d")) << TRANS ("</td><td class=\"r\">") << (it.lra > 0.0 ? juce::String (it.lra, 1) : juce::String ("-")) << "</td></tr>";
    h << "</table>";
    if (o.norm == ExportOptions::Norm::Lufs) h << "<p>Normalizzazione del MIX a " << juce::String (o.normTarget, 1) << " LUFS (guadagno " << (mi.gainDb >= 0 ? "+" : "") << juce::String (mi.gainDb, 1)
                                                  << TRANS (" dB, tetto true-peak ") << juce::String (mi.ceiling, 1) << " dBTP" << (mi.limited ? TRANS (", limiter intervenuto") : "") << TRANS ("). Gli stem hanno lo stesso guadagno.</p>");
    h << TRANS ("<p style=\"font-size:12px;color:#555\">Misura di loudness: programma intero secondo ITU-R BS.1770 / EBU R128 (non limitata al solo dialogo). Gli stem sono presi prima del bus master.</p>");
    if (unrouted > 0) h << TRANS ("<div class=\"warn\">") << unrouted << TRANS (" tracce con clip non sono assegnate a un bus di ruolo (Dialoghi / M&amp;E / Effetti): compaiono solo nel MIX.</div>");
    h << "</body></html>";
    return h;
}

/** Copia della sessione pronta per l'esportazione: la Guida (dialogo originale) resta fuori se richiesto. */
Session prepare (const Session& source, const ExportOptions& o)
{
    Session s = source.cloneDetached();
    if (o.excludeGuide)
        for (auto& t : s.tracks)
            if (t.live->trackType.load() == TrackGuide) t.live->mute = true;
    return s;
}
} // namespace

bool renderTrackProcessed (const Session& source, int trackId, double rate, int64_t total, const juce::File& dest,
                           const std::function<bool (double)>& progress, juce::String& error)
{
    Session one = source.cloneDetached();
    one.tracks.erase (std::remove_if (one.tracks.begin(), one.tracks.end(), [trackId] (const Track& t) { return t.id != trackId; }), one.tracks.end());
    if (one.tracks.empty()) { error = TRANS ("La traccia non c'e' piu'."); return false; }
    auto& t = one.tracks.front();
    t.frozen = nullptr;
    auto& l = *t.live;
    l.volumeDb = 0.0f; l.pan = 0.0f; l.mute = false; l.solo = false; l.invert = false; l.autoMode = 0;   // fader, pan e polarita' restano vivi dopo il freeze
    l.limiterOn = false; l.outBus = 0; l.sendDb[0] = -80.0f; l.sendDb[1] = -80.0f;
    for (auto& x : one.aux) x->volumeDb = -80.0f;
    return renderToTemp (one, rate, 0, total, false, dest, nullptr, progress, error);
}

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
    if (total <= 0) { res.message = TRANS ("L'intervallo da esportare e' vuoto."); return res; }

    const Session base = prepare (session, o);
    const auto ext = "." + o.format;
    auto isGuide = [&o] (const Track& t) { return o.excludeGuide && t.live->trackType.load() == TrackGuide; };

    //---------------------------------------------------------------- pacchetto di consegna: MIX + stem per ruolo + rapporto
    if (o.delivery)
    {
        target.createDirectory();
        struct Role { int bus; const char* tag; };
        const Role roles[] = { { 1, "DIA" }, { 2, "ME" }, { 3, "FX" } };
        std::vector<int> active;   // ruoli che hanno almeno una traccia con clip
        int unrouted = 0;
        for (auto& t : base.tracks)
        {
            if (t.clips.empty() || isGuide (t)) continue;
            const int b = t.live->outBus.load();
            if (b >= 1 && b <= 3) { if (std::find (active.begin(), active.end(), b) == active.end()) active.push_back (b); }
            else ++unrouted;
        }
        std::sort (active.begin(), active.end());
        const int steps = 1 + (int) active.size();
        const double span = 1.0 / (double) steps;
        juce::String err;

        MixInfo mi;
        const auto mixFile = target.getChildFile (juce::File::createLegalFileName (baseName + "_MIX") + ext);
        if (! doMixdown (base, rate, o, start, total, mixFile, 0.0, span, progress, mi, err)) { res.message = err; return res; }
        const double seconds = (double) total / rate;
        std::vector<ExportItem> items;
        items.push_back ({ mixFile.getFullPathName(), mi.peakDb + mi.gainDb - (mi.limited ? mi.gr : 0.0), juce::jmin (mi.tpDb + mi.gainDb, mi.limited ? mi.ceiling : 1000.0), mi.lufs, mi.m.lra });
        res.files.add (mixFile.getFullPathName());

        const double gainLin = (double) juce::Decibels::decibelsToGain ((float) mi.gainDb);
        int step = 1;
        for (int b : active)
        {
            Session s = base.cloneDetached();
            for (auto& t : s.tracks) { if (t.live->outBus.load() != b) t.live->mute = true; t.live->solo = false; }
            const char* tag = roles[b - 1].tag;
            const auto dest = target.getChildFile (juce::File::createLegalFileName (baseName + "_" + tag) + ext);
            Measure m;
            if (! doStem (s, rate, o, start, total, gainLin, dest, span * step, span, progress, &m, err)) { res.message = err; return res; }
            ++step;
            items.push_back ({ dest.getFullPathName(), juce::Decibels::gainToDecibels (m.peak, -120.0f) + mi.gainDb, juce::Decibels::gainToDecibels (m.truePeak, -120.0f) + mi.gainDb,
                               m.lufs > -99.0 ? m.lufs + mi.gainDb : -100.0, m.lra });
            res.files.add (dest.getFullPathName());
        }

        const auto report = target.getChildFile (juce::File::createLegalFileName (baseName + "_rapporto") + ".html");
        report.replaceWithText (reportHtml (o, items, mi, unrouted, seconds, baseName));
        res.items = items;
        res.reportFile = report.getFullPathName();
        res.ok = true;
        res.gainDb = mi.gainDb; res.limiterGrDb = mi.gr;
        res.peakDb = items.front().peakDb; res.truePeakDb = items.front().truePeakDb; res.lufs = mi.lufs; res.lra = mi.m.lra;
        res.message = TRANS ("Consegna esportata in ") + target.getFullPathName() + "\n" + juce::String ((int) items.size()) + TRANS (" file (MIX") + (active.empty() ? juce::String() : juce::String (" + ") + juce::String ((int) active.size()) + " stem")
                      + TRANS (") e rapporto.\nMIX: ") + (mi.lufs > -99.0 ? juce::String (mi.lufs, 1) + " LUFS" : juce::String ("n/d")) + TRANS (", true-peak ") + juce::String (items.front().truePeakDb, 1) + " dBTP";
        if (unrouted > 0) res.message << "\nATTENZIONE: " << unrouted << TRANS (" tracce non sono sul bus di un ruolo e sono solo nel MIX.");
        return res;
    }

    //---------------------------------------------------------------- stem per traccia
    if (o.stems)
    {
        target.createDirectory();
        int index = 0, count = 0;
        for (auto& t : base.tracks) if (! t.clips.empty() && ! isGuide (t)) ++count;
        if (count == 0) { res.message = "Nessuna traccia con clip da esportare."; return res; }

        for (size_t ti = 0; ti < base.tracks.size(); ++ti)
        {
            if (base.tracks[ti].clips.empty() || isGuide (base.tracks[ti])) continue;
            Session s = base.cloneDetached();
            for (size_t k = 0; k < s.tracks.size(); ++k) { s.tracks[k].live->mute = (k != ti); s.tracks[k].live->solo = false; }

            const auto name = juce::File::createLegalFileName (baseName + "_" + juce::String (++index).paddedLeft ('0', 2) + "_" + base.tracks[ti].name);
            const auto dest = target.getChildFile (name + ext);
            juce::String err;
            if (! doStem (s, rate, o, start, total, 1.0, dest, (double) (index - 1) / (double) count, 1.0 / (double) count, progress, nullptr, err)) { res.message = err; return res; }
            res.files.add (dest.getFullPathName());
        }
        res.ok = true;
        res.message = juce::String (res.files.size()) + TRANS (" stem esportati in ") + target.getFullPathName();
        return res;
    }

    //---------------------------------------------------------------- mixdown
    MixInfo mi;
    juce::String err;
    if (! doMixdown (base, rate, o, start, total, target, 0.0, 1.0, progress, mi, err)) { res.message = err; return res; }

    res.ok = true;
    res.files.add (target.getFullPathName());
    res.gainDb = mi.gainDb;
    res.limiterGrDb = mi.gr;
    res.peakDb = mi.peakDb + mi.gainDb - (mi.limited ? mi.gr : 0.0);
    res.truePeakDb = juce::jmin (mi.tpDb + mi.gainDb, mi.limited ? mi.ceiling : 1000.0);
    res.lufs = mi.lufs;
    res.lra = mi.m.lra;

    juce::String msg = TRANS ("Esportato in ") + target.getFullPathName();
    msg << TRANS ("\nLoudness integrato: ") << (res.lufs > -99.0 ? juce::String (res.lufs, 1) + " LUFS" : juce::String ("n/d"))
        << "   LRA: " << juce::String (res.lra, 1) << " LU"
        << "\nPicco: " << juce::String (res.peakDb, 1) << TRANS (" dBFS   True-peak: ") << juce::String (res.truePeakDb, 1) << " dBTP";
    if (std::abs (mi.gainDb) > 0.005) msg << TRANS ("\nGuadagno di normalizzazione: ") << (mi.gainDb >= 0 ? "+" : "") << juce::String (mi.gainDb, 1) << " dB";
    if (mi.limited) msg << TRANS ("\nLimiter true-peak intervenuto (riduzione massima ") << juce::String (mi.gr, 1) << " dB)";
    if (res.peakDb > 0.0) msg << TRANS ("\nATTENZIONE: il picco supera 0 dBFS, il file puo' distorcere.");
    res.message = msg;
    return res;
}

} // namespace fc
