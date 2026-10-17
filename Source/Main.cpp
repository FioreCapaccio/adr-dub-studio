#include "MainComponent.h"
#include <iostream>
#include "VideoSupport.h"
#include "AdrImport.h"
#include "TimeStretch.h"
#include "ExportEngine.h"
#include "BuiltinFx.h"
#include "DubWorkflow.h"
#include "CueSheet.h"
#include "AudioAlign.h"
#include "AutoDuck.h"
#include "BoothTools.h"
#include "Relink.h"
#include "Versions.h"
#include "SessionCheck.h"
#include "WorkflowGuide.h"
#include "Conform.h"
#include "ExternalSync.h"
#include "ThemeManager.h"
#include "Lang.h"

/** Processore di prova: ritarda il segnale di N campioni e dichiara quella latenza. */
class DelayTestProcessor : public juce::AudioProcessor
{
public:
    explicit DelayTestProcessor (int latency)
        : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo()).withOutput ("Out", juce::AudioChannelSet::stereo())),
          N (latency)
    {
        setLatencySamples (latency);
        ring[0].assign ((size_t) N, 0.0f);
        ring[1].assign ((size_t) N, 0.0f);
    }
    const juce::String getName() const override { return "DelayTest"; }
    void prepareToPlay (double, int) override { reset(); }
    void releaseResources() override {}
    void reset() override { ring[0].assign ((size_t) N, 0.0f); ring[1].assign ((size_t) N, 0.0f); pos = 0; }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        for (int c = 0; c < 2; ++c)
        {
            int p = pos;
            float* d = b.getWritePointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float out = ring[c][(size_t) p];
                ring[c][(size_t) p] = d[i];
                d[i] = out;
                if (++p == N) p = 0;
            }
            if (c == 1) pos = p;
        }
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    int N, pos = 0;
    std::vector<float> ring[2];
};

/** Verifica non interattiva di modello, mixer ed esportazione: ADR Dub Studio --selftest file1.wav file2.wav ... */
class SelfTest
{
public:
    SelfTest (juce::StringArray f) : files (std::move (f))
    {
        formats.registerBasicFormats();
        project = std::make_unique<fc::Project> (formats, 48000.0);
    }

    void run (std::function<void (int)> finished)
    {
        juce::Array<juce::File> list;
        for (auto& f : files)
        {
            if (f.isEmpty()) continue;
            if (juce::File (f).hasFileExtension ("vst3")) pluginFile = juce::File (f);
            else if (fc::video::isVideoFile (juce::File (f))) videoFile = juce::File (f);
            else list.add (juce::File (f));
        }
        project->importFiles (list, project->getSession().tracks.front().id, 0, [this, finished] (juce::StringArray errs)
        {
            finishCb = finished;
            check (errs);
        });
    }

private:
    int fails = 0;
    void expect (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "[ok]   " : "[FAIL] ") << what << std::endl;
        if (! ok) ++fails;
    }

    std::function<void (int)> finishCb;

    void check (const juce::StringArray& errs)
    {
        stepLimiter();
        stepBuiltin();
        stepTheme();
        stepWorkflow();
        stepDelivery();
        stepSync();
        stepBooth();
        stepConform();
        stepMtc();
        stepLang();
        stepTrackFolders();
        stepEditing();
        stepPdc();
        stepAux();
        stepGroups();
        stepSpectrum();
        stepEq();
        stepAudition();
        stepTrackLimiter();
        stepAutoLanes();
        stepPresets();
        stepTrimAndPeaks();
        stepAutomation();
        stepCrossfade();
        stepTimecode();
        stepAdrImport();
        stepLoudness();
        stepExport();
        auto& s = project->getSession();
        expect (errs.isEmpty(), "import senza errori " + errs.joinIntoString (";"));
        expect (s.tracks.size() >= 3, "tracce: " + juce::String ((int) s.tracks.size()));
        const int64_t len0 = s.tracks[0].clips.empty() ? 0 : s.tracks[0].clips[0].length;
        std::cout << "     lunghezza clip 1 (voce 44.1k, 18 s attesi=864000): " << len0 << std::endl;
        expect (std::abs (len0 - 864000) < 4, "ricampionamento 44.1k -> 48k corretto");

        // mixer: render 1 s da t=2 s
        fc::Mixer mixer;
        juce::AudioBuffer<float> buf (2, 48000);
        mixer.render (s, 96000, buf, 0, 48000);
        const float peakAll = buf.getMagnitude (0, 48000);
        expect (peakAll > 0.05f, "mix non silenzioso, picco " + juce::String (peakAll, 3));

        // mute di tutte le tracce => silenzio
        for (auto& t : s.tracks) t.live->mute = true;
        fc::Mixer m2;
        m2.render (s, 96000, buf, 0, 48000);
        expect (buf.getMagnitude (0, 48000) == 0.0f, "mute => silenzio");
        for (auto& t : s.tracks) t.live->mute = false;

        // solo sulla traccia 2
        s.tracks[1].live->solo = true;
        fc::Mixer m3, m4;
        juce::AudioBuffer<float> solo (2, 48000), only2 (2, 48000);
        m3.render (s, 96000, solo, 0, 48000);
        s.tracks[1].live->solo = false;
        for (size_t i = 0; i < s.tracks.size(); ++i) s.tracks[i].live->mute = (i != 1);
        m4.render (s, 96000, only2, 0, 48000);
        float diff = 0.0f;
        for (int i = 0; i < 48000; ++i) diff = juce::jmax (diff, std::abs (solo.getSample (0, i) - only2.getSample (0, i)));
        expect (diff < 1.0e-6f, "solo == mute degli altri");
        for (auto& t : s.tracks) t.live->mute = false;

        // pan: tutto a sinistra => canale destro silenzioso
        s.tracks[0].live->pan = -1.0f;
        for (size_t i = 1; i < s.tracks.size(); ++i) s.tracks[i].live->mute = true;
        fc::Mixer m5;
        m5.render (s, 96000, buf, 0, 48000);
        expect (buf.getMagnitude (1, 0, 48000) == 0.0f && buf.getMagnitude (0, 0, 48000) > 0.01f, "pan hard left");
        s.tracks[0].live->pan = 0.0f;
        for (auto& t : s.tracks) t.live->mute = false;

        // split / undo / redo
        const size_t before = s.tracks[0].clips.size();
        project->selection.clear();
        project->splitAt (5 * 48000);
        expect (project->getSession().tracks[0].clips.size() == before + 1, "split alla testina");
        auto& cl = project->getSession().tracks[0].clips;
        expect (cl[0].length == 5 * 48000 && cl[1].start == 5 * 48000 && cl[1].offset == 5 * 48000 && cl[0].length + cl[1].length == len0,
                "split: lunghezze e offset coerenti");

        // la somma dei due pezzi suona come l'originale
        fc::Mixer ma, mb;
        juce::AudioBuffer<float> a (2, 4096), b (2, 4096);
        project->undo();
        expect (project->getSession().tracks[0].clips.size() == before, "undo annulla lo split");
        ma.render (project->getSession(), 5 * 48000 - 2048, a, 0, 4096);
        project->redo();
        mb.render (project->getSession(), 5 * 48000 - 2048, b, 0, 4096);
        float d2 = 0.0f;
        for (int i = 0; i < 4096; ++i) d2 = juce::jmax (d2, std::abs (a.getSample (0, i) - b.getSample (0, i)));
        expect (d2 < 1.0e-6f, "il taglio e' trasparente (nessun click)");

        project->addAutoPoint (project->getSession().tracks[0].id, 48000, -12.0f);
        project->addAutoPoint (project->getSession().tracks[0].id, 96000, -3.0f);
        project->addMarker (3 * 48000, "Scena 1");
        project->addMarker (9 * 48000, "Scena 2");

        // salvataggio e caricamento sessione
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest.adb");
        juce::String err;
        project->getSession().group[2]->volumeDb = -3.0f;   // gruppi: i parametri e l'instradamento si salvano (un gruppo a 0 dB non cambia il mix)
        project->getSession().tracks[0].live->outBus = 4;
        project->exportIn = 12000; project->exportOut = 96000; project->adrDialogueSize = 44; project->adrNaming = "{progetto}-{cue}";
        { auto& t3 = project->edit().tracks[3]; fc::AutoPoint a; a.pos = 4800; a.db = -0.5f; t3.panAuto = { a }; a.db = -9.0f; t3.sendAAuto = { a }; t3.live->autoParam = fc::AutoSendA; }   // curve su una traccia vuota
        { auto& t3 = project->edit().tracks[3]; t3.live->trimDb = 7.5f; t3.live->trackType = fc::TrackEffects; t3.live->comp.enabled = true; t3.live->comp.ratio = 5.5f; t3.live->comp.thresholdDb = -33.0f; }
        project->getSession().tracks[3].live->limiterOn = true; project->getSession().tracks[3].live->ceilingDb = -3.0f;   // traccia 4 vuota: il mix non cambia
        { auto& e = project->getSession().tracks[0].live->eq; e.enabled = true; e.band[2].gainDb = 0.0f; e.band[2].freq = 3200.0f; e.band[2].q = 2.5f; e.band[2].type = fc::ParamEq::Bell; }   // EQ acceso ma neutro: non cambia il mix
        expect (project->save (tmp, err), "salva sessione " + err);

        // esportazione offline
        auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_mix.wav");
        juce::String msg;
        const bool ok = fc::renderMixdown (project->getSession(), 48000.0, wav, [] (double) { return true; }, msg);
        expect (ok, "esportazione WAV: " + msg.replaceCharacter ('\n', ' '));
        std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (wav));
        const int64_t expectLen = project->contentEnd() + 24000;
        expect (rd != nullptr && std::abs (rd->lengthInSamples - expectLen) <= 4096 && rd->bitsPerSample == 24 && rd->numChannels == 2,
                "WAV 24 bit stereo, lunghezza " + juce::String (rd ? (juce::int64) rd->lengthInSamples : 0) + " (attesa ~" + juce::String ((juce::int64) expectLen) + ")");

        // ricarica la sessione salvata
        auto proj2 = std::make_unique<fc::Project> (formats, 48000.0);
        reload = std::move (proj2);
        reload->loadSession (tmp, [this] (juce::String e)
        {
            int clips = 0;
            for (auto& t : reload->getSession().tracks) clips += (int) t.clips.size();
            expect (e.isEmpty() && clips == 6, "ricarica sessione (3 clip divise alla testina): " + juce::String (clips) + " clip " + e);
            expect (reload->getSession().tracks[0].live->outBus.load() == 4 && std::abs (reload->getSession().group[2]->volumeDb.load() + 3.0f) < 1.0e-4f,
                    "gruppi: instradamento e fader salvati e ricaricati con la sessione");
            {
                auto& e = reload->getSession().tracks[0].live->eq;
                expect (e.enabled.load() && std::abs (e.band[2].freq.load() - 3200.0f) < 0.5f && std::abs (e.band[2].q.load() - 2.5f) < 0.01f, "EQ: parametri salvati e ricaricati con la sessione");
            }
            {
                auto& t3 = reload->getSession().tracks[3];
                expect (t3.panAuto.size() == 1 && std::abs (t3.panAuto[0].db + 0.5f) < 1.0e-4f && t3.sendAAuto.size() == 1 && std::abs (t3.sendAAuto[0].db + 9.0f) < 1.0e-4f && t3.live->autoParam.load() == fc::AutoSendA,
                        "automazione: curve pan e send salvate e ricaricate con la sessione");
            }
            {
                expect (std::abs (reload->getSession().tracks[3].live->trimDb.load() - 7.5f) < 1.0e-4f, "guadagno in ingresso: salvato e ricaricato con la sessione");
                auto& c3 = reload->getSession().tracks[3].live->comp;
                expect (reload->getSession().tracks[3].live->trackType.load() == fc::TrackEffects && c3.enabled.load() && std::abs (c3.ratio.load() - 5.5f) < 1.0e-4f && std::abs (c3.thresholdDb.load() + 33.0f) < 1.0e-4f,
                        "tipo di traccia e compressore: salvati e ricaricati con la sessione");
            }
            expect (reload->adrNaming == "{progetto}-{cue}", "schema dei nomi dei take: salvato e ricaricato con la sessione");
            expect (reload->getSession().tracks[3].live->limiterOn.load() && ! reload->getSession().tracks[0].live->limiterOn.load() && std::abs (reload->getSession().tracks[3].live->ceilingDb.load() + 3.0f) < 1.0e-4f,
                    "limiter di traccia: salvato e ricaricato (le altre tracce restano senza)");
            expect (reload->exportIn == 12000 && reload->exportOut == 96000 && reload->adrDialogueSize == 44, "intervallo di esportazione e dimensione del dialogo salvati con la sessione");
            expect (reload->getSession().markers.size() == 2 && reload->getSession().markers[0].name == "Scena 1"
                        && reload->getSession().markers[1].pos == 9 * 48000, "marker: salvati e ricaricati con la sessione");
            {
                auto& rt = reload->getSession().tracks[0];
                expect (rt.volAuto.size() == 2 && rt.volAuto[1].pos == 96000 && std::abs (rt.volAuto[1].db + 3.0f) < 1.0e-4f && rt.live->autoMode.load() == 1,
                        "automazione: salvata e ricaricata con la sessione");
            }
            stepStretch ([this] { stepPlugin ([this] { stepReliability ([this] { stepRecording(); }); }); });
        });
    }

    void stepEditing()
    {
        auto asset = project->getSession().tracks[0].clips.empty() ? nullptr : project->getSession().tracks[0].clips[0].asset;
        if (asset == nullptr) { expect (false, "editing: asset di prova mancante"); return; }

        fc::Project ed (formats, 48000.0);
        auto mk = [&] (int64_t id, int64_t start, int64_t len)
        {
            fc::Clip c; c.id = id; c.asset = asset; c.start = start; c.length = len; return c;
        };
        const int64_t S = 48000;
        auto& t = ed.edit().tracks;
        t[0].clips = { mk (1000, 0, 10 * S), mk (1001, 12 * S, 8 * S) };   // id alti: i nuovi id del progetto partono da 1
        t[1].clips = { mk (1002, 0, 15 * S) };
        t[0].name = "A"; t[1].name = "B";

        // lama: divide un solo clip
        ed.splitClip (1000, 4 * S);
        expect (t[0].clips.size() == 3 && t[0].clips[0].length == 4 * S && t[0].clips[2].start == 4 * S
                    && t[0].clips[2].offset == 4 * S && t[0].clips[2].length == 6 * S && t[1].clips.size() == 1,
                "lama: divide solo il clip scelto");

        // ripple delete del primo pezzo: il vuoto si chiude sulla stessa traccia, le altre non si toccano
        ed.selection = { 1000 };
        ed.rippleDeleteSelected();
        bool okRipple = t[0].clips.size() == 2 && t[1].clips[0].start == 0;
        for (auto& c : t[0].clips) okRipple = okRipple && (c.id == 1001 ? c.start == 8 * S : c.start == 0);
        expect (okRipple, "ripple delete: chiude il vuoto sulla traccia");

        // taglia l'inizio alla testina (nessuna selezione: tutti i clip sotto la testina)
        ed.selection.clear();
        ed.trimToPlayhead (true, 2 * S);
        const auto& c3 = t[1].clips[0];
        expect (c3.start == 2 * S && c3.offset == 2 * S && c3.length == 13 * S && t[0].clips.size() == 2, "taglia in: inizio dei clip alla testina");
        ed.trimToPlayhead (false, 7 * S);
        expect (t[1].clips[0].length == 5 * S && t[1].clips[0].end() == 7 * S, "taglia out: fine dei clip alla testina");

        // riordino e duplicazione tracce
        ed.moveTrackTo (0, 1);
        expect (t[0].name == "B" && t[1].name == "A", "riordino delle tracce");
        const size_t n = t.size();
        ed.duplicateTrack (t[0].id);
        expect (t.size() == n + 1 && t[1].name == "B copia" && t[1].live != t[0].live && t[1].clips.size() == t[0].clips.size()
                    && t[1].clips[0].id != t[0].clips[0].id, "duplica traccia");
        ed.undo();
        expect (t.size() == n, "undo della duplicazione");

        // marker
        ed.addMarker (5 * S, "B");
        ed.addMarker (2 * S, "A");
        auto& marks = ed.getSession().markers;
        expect (marks.size() == 2 && marks[0].name == "A" && marks[1].name == "B", "marker: inseriti in ordine di posizione");
        expect (ed.nextMarkerPos (0) == 2 * S && ed.nextMarkerPos (2 * S) == 5 * S && ed.nextMarkerPos (5 * S) == -1
                    && ed.previousMarkerPos (5 * S) == 2 * S && ed.previousMarkerPos (2 * S) == -1, "marker: navigazione precedente/successivo");
        ed.renameMarker (marks[0].id, "Inizio");
        expect (ed.getSession().markers[0].name == "Inizio", "marker: rinomina");
        ed.undo();
        ed.removeMarker (ed.getSession().markers[1].id);
        expect (ed.getSession().markers.size() == 1, "marker: eliminazione");
        ed.undo();
        expect (ed.getSession().markers.size() == 2, "marker: undo dell'eliminazione");
    }

    static float measureFileLufs (juce::AudioFormatManager& fm, const juce::File& f, float* peakDbOut = nullptr)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) return -100.0f;
        Loudness::Meter m;
        m.prepare (r->sampleRate);
        juce::AudioBuffer<float> b (2, 4096);
        for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += 4096)
        {
            const int n = (int) juce::jmin<juce::int64> (4096, r->lengthInSamples - pos);
            r->read (&b, 0, n, pos, true, true);
            m.process (b.getReadPointer (0), b.getReadPointer (1), n);
        }
        if (peakDbOut != nullptr) *peakDbOut = juce::Decibels::gainToDecibels (m.samplePeak.load(), -120.0f);
        return m.integrated.load();
    }

    void stepExport()
    {
        // sessione di prova: un seno a 997 Hz a -30 dBFS (8 s) e un impulso
        auto sine = std::make_shared<fc::Asset>();
        sine->name = "seno";
        const int len = 8 * 48000;
        sine->audio.setSize (2, len);
        for (int i = 0; i < len; ++i)
        {
            const float v = juce::Decibels::decibelsToGain (-30.0f) * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / 48000.0);
            sine->audio.setSample (0, i, v);
            sine->audio.setSample (1, i, v);
        }
        sine->buildPeaks();

        fc::Project p (formats, 48000.0);
        auto& t = p.edit().tracks;
        fc::Clip c; c.id = 7000; c.asset = sine; c.start = 0; c.length = len;
        t[0].clips = { c };
        p.edit().master->limiterOn = false;
        const auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("arotron_export_test");
        tmpDir.deleteRecursively();
        tmpDir.createDirectory();
        auto go = [&] (const fc::ExportOptions& o, const juce::File& f) { return fc::exportSession (p.getSession(), 48000.0, o, f, "prova", [] (double) { return true; }); };

        // 1. normalizzazione a -23 LUFS, WAV 24 bit
        {
            fc::ExportOptions o; o.norm = fc::ExportOptions::Norm::Lufs; o.normTarget = -23.0; o.bitDepth = 24;
            auto f = tmpDir.getChildFile ("lufs.wav");
            auto r = go (o, f);
            float pk = 0;
            const float lufs = measureFileLufs (formats, f, &pk);
            expect (r.ok && std::abs (lufs + 23.0f) < 0.3f, "export: normalizzazione a -23 LUFS, il file misura " + juce::String (lufs, 2) + " LUFS");
        }
        // 2. normalizzazione del picco a -1 dBFS
        {
            fc::ExportOptions o; o.norm = fc::ExportOptions::Norm::Peak; o.normTarget = -1.0; o.bitDepth = 24;
            auto f = tmpDir.getChildFile ("peak.wav");
            auto r = go (o, f);
            float pk = 0;
            measureFileLufs (formats, f, &pk);
            expect (r.ok && std::abs (pk + 1.0f) < 0.05f, "export: normalizzazione del picco a -1 dBFS (file " + juce::String (pk, 2) + ")");
        }
        // 3. limiter true-peak: obiettivo altissimo, tetto -1 dB
        {
            fc::ExportOptions o; o.norm = fc::ExportOptions::Norm::Lufs; o.normTarget = -0.5; o.ceilingDb = -1.0; o.bitDepth = 24;
            auto f = tmpDir.getChildFile ("limit.wav");
            auto r = go (o, f);
            float pk = 0;
            measureFileLufs (formats, f, &pk);
            expect (r.ok && pk <= -0.95f && r.limiterGrDb >= 0.0, "export: il limiter rispetta il tetto -1 dB (picco " + juce::String (pk, 2) + ")");
        }
        // 4. 16 bit a 44,1 kHz con dither: frequenza, lunghezza, livello
        {
            fc::ExportOptions o; o.bitDepth = 16; o.outRate = 44100.0; o.dither = true;
            auto f = tmpDir.getChildFile ("cd.wav");
            auto r = go (o, f);
            std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (f));
            const bool okFmt = rd != nullptr && std::abs (rd->sampleRate - 44100.0) < 0.5 && rd->bitsPerSample == 16;
            const double expectLen = (double) (len + 24000) * 44100.0 / 48000.0;
            float pk = 0;
            measureFileLufs (formats, f, &pk);
            expect (r.ok && okFmt && rd != nullptr && std::abs ((double) rd->lengthInSamples - expectLen) < 3.0 && std::abs (pk + 30.0f) < 0.3f,
                    "export: 16 bit / 44,1 kHz, " + juce::String (rd ? (juce::int64) rd->lengthInSamples : 0) + " campioni, picco " + juce::String (pk, 2));
        }
        // 5. intervallo (regione) e FLAC
        {
            fc::ExportOptions o; o.format = "flac"; o.bitDepth = 24; o.loopRange = true; o.rangeStart = 2 * 48000; o.rangeEnd = 5 * 48000;
            auto f = tmpDir.getChildFile ("range.flac");
            auto r = go (o, f);
            std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (f));
            expect (r.ok && rd != nullptr && rd->lengthInSamples == 3 * 48000, "export: regione di 3 s in FLAC, " + juce::String (rd ? (juce::int64) rd->lengthInSamples : 0) + " campioni");
        }
        // 6. stem: una traccia in piu' con un clip
        {
            auto s2 = p.getSession();
            fc::Clip c2 = c; c2.id = 7001; c2.gainDb = -6.0f;
            p.edit().tracks[1].clips = { c2 };
            fc::ExportOptions o; o.stems = true; o.bitDepth = 24;
            auto r = go (o, tmpDir.getChildFile ("stems"));
            expect (r.ok && r.files.size() == 2, "export: stem, " + juce::String (r.files.size()) + " file (attesi 2)");
            if (r.files.size() == 2)
            {
                float pk0 = 0, pk1 = 0;
                measureFileLufs (formats, juce::File (r.files[0]), &pk0);
                measureFileLufs (formats, juce::File (r.files[1]), &pk1);
                expect (std::abs (pk0 + 30.0f) < 0.2f && std::abs (pk1 + 36.0f) < 0.2f, "stem: livelli per traccia " + juce::String (pk0, 1) + " e " + juce::String (pk1, 1) + " dBFS");
            }
        }
        tmpDir.deleteRecursively();
    }

    void stepLoudness()
    {
        Loudness::Meter m;
        m.prepare (48000.0);
        const float amp = juce::Decibels::decibelsToGain (-23.0f);
        std::vector<float> l (4800), r (4800);
        double phase = 0.0;
        for (int blk = 0; blk < 150; ++blk)   // 15 s di seno a 997 Hz, poi 5 s di silenzio (non deve cambiare l'integrato)
        {
            for (int i = 0; i < 4800; ++i, phase += 2.0 * juce::MathConstants<double>::pi * 997.0 / 48000.0)
                l[(size_t) i] = r[(size_t) i] = blk < 150 ? amp * (float) std::sin (phase) : 0.0f;
            m.process (l.data(), r.data(), 4800);
        }
        const float integ15 = m.integrated.load(), lra15 = m.lra.load();
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        for (int blk = 0; blk < 50; ++blk) m.process (l.data(), r.data(), 4800);

        expect (std::abs (integ15 + 23.0f) < 0.15f, "loudness: seno 997 Hz a -23 dBFS = " + juce::String (integ15, 2) + " LUFS (atteso -23,0)");
        expect (std::abs (m.integrated.load() - integ15) < 0.05f, "loudness: il silenzio non altera l'integrato (gating)");
        expect (lra15 < 1.0f, "loudness: segnale stazionario, LRA " + juce::String (lra15, 2));
        const float tp = juce::Decibels::gainToDecibels (m.truePeak.load());
        expect (tp > -23.2f && tp < -22.5f, "true-peak del seno: " + juce::String (tp, 2) + " dBTP");
        m.requestReset();
        m.process (l.data(), r.data(), 100);
        expect (m.integrated.load() < -90.0f && m.truePeak.load() == 0.0f, "reset della misura");
    }

    void stepAdrImport()
    {
        // 1. elenco marker di Pro Tools ("Export Session Info as Text")
        const juce::String pt =
            "SESSION NAME:\tFilm ADR\r\n"
            "SAMPLE RATE:\t48000.000000\r\n"
            "BIT DEPTH:\t24-bit\r\n"
            "SESSION START TIMECODE:\t01:00:00:00\r\n"
            "TIMECODE FORMAT:\t25 Frame\r\n"
            "# OF AUDIO TRACKS:\t4\r\n"
            "\r\n"
            "M A R K E R S  L I S T I N G\r\n"
            "#   \tLOCATION    \tTIME REFERENCE    \tUNITS    \tNAME    \tCOMMENTS\r\n"
            "1   \t01:00:12:05\t586800\tSamples\tADR 1 - MARCO\tCHARACTER: Marco | SCENE: 12 | OUT: 01:00:15:00 | DIALOGUE: Non ci credo\r\n"
            "2   \t01:01:30:10\t4368000\tSamples\tGIULIA SC14\tNon e' possibile\r\n"
            "3   \t01:02:00:00\t5760000\tSamples\tADR 3\t\r\n";
        auto r1 = fc::adr::parse (pt);
        expect (r1.format.startsWith ("Pro Tools") && r1.cues.size() == 3 && r1.sessionStartTc == "01:00:00:00" && r1.tcFormatIndex == 2 && std::abs (r1.sampleRate - 48000.0) < 1.0,
                "ADR: riconosciuto l'elenco marker di Pro Tools (" + juce::String ((int) r1.cues.size()) + " cue, inizio " + r1.sessionStartTc + ")");
        if (r1.cues.size() == 3)
        {
            expect (r1.cues[0].character == "Marco" && r1.cues[0].scene == "12" && r1.cues[0].tcOut == "01:00:15:00" && r1.cues[0].dialogue == "Non ci credo",
                    "ADR: i commenti con CHARACTER/SCENE/OUT/DIALOGUE vengono letti");
            expect (r1.cues[1].dialogue == "Non e' possibile" && r1.cues[1].character == "GIULIA SC14", "ADR: commento libero = dialogo, nome = personaggio");
        }

        fc::Project p (formats, 48000.0);
        p.tcFormat = 2;
        p.tcStartSec = 3600.0;
        juce::StringArray warn;
        auto cues = fc::adr::toCues (r1, p, 48000.0, 4.0, warn);
        expect (cues.size() == 3 && cues[0].inPos == (int64_t) std::llround (12.2 * 48000.0) && cues[0].outPos == 15 * 48000
                    && cues[1].outPos - cues[1].inPos == 4 * 48000, "ADR: posizioni = timecode assoluto meno inizio sessione, durata predefinita 4 s");

        // timecode prima dell'inizio sessione: scartato con avviso
        p.tcStartSec = 3613.0;
        juce::StringArray warn2;
        auto cues2 = fc::adr::toCues (r1, p, 48000.0, 4.0, warn2);
        expect (cues2.size() == 2 && warn2.size() == 1, "ADR: i cue prima dell'inizio sessione vengono scartati con un avviso");
        p.tcStartSec = 3600.0;

        // CSV con timecode a 30 fps importato in una sessione a 23,976: i fotogrammi oltre 23 non sono validi
        {
            const juce::String csv = "N,TC IN,TC OUT,Personaggio,Dialogo\n1,00:01:15:13,00:01:17:29,Sam,\"Ciao, mondo\"\n2,00:01:20:00,00:01:22:05,Sam,Ok\n";
            const auto rc = fc::adr::parse (csv);
            const int fmt = fc::adr::suggestedTcFormat (rc, 0);
            expect (rc.cues.size() == 2 && rc.maxFrameField == 29 && fmt == 5, "ADR CSV: rilevati fotogrammi fino a " + juce::String (rc.maxFrameField) + ", formato proposto " + juce::String (fc::tcFormats()[fmt].name));
            expect (fc::adr::suggestedTcFormat (rc, 5) == 5 && fc::adr::suggestedTcFormat (fc::adr::parse ("N,TC IN\n1,00:00:01:05\n"), 0) == 0, "ADR CSV: formato attuale mantenuto se i fotogrammi ci stanno");
            fc::Project pc (formats, 48000.0);
            pc.tcFormat = 0;
            juce::StringArray w;
            const auto bad = fc::adr::toCues (rc, pc, 48000.0, 4.0, w);
            expect (w.size() == 1 && bad.size() == 2 && bad[0].outPos - bad[0].inPos == 4 * 48000, "ADR CSV: con il formato sbagliato l'uscita non valida e' segnalata una volta sola (" + juce::String (w.size()) + " avvisi)");
            pc.tcFormat = fmt;
            w.clear();
            const auto good = fc::adr::toCues (rc, pc, 48000.0, 4.0, w);
            expect (w.isEmpty() && good.size() == 2 && good[0].outPos == (int64_t) std::llround ((77.0 + 29.0 / 30.0) * 48000.0)
                        && good[0].inPos == (int64_t) std::llround ((75.0 + 13.0 / 30.0) * 48000.0), "ADR CSV: con 30 fps entrata 1:15:13 e uscita 1:17:29 corrette");
        }

        // 2. EDL
        const juce::String edl =
            "TITLE: ADR SPOTTING\n"
            "FCM: NON-DROP FRAME\n\n"
            "001  AX       V     C        01:00:00:00 01:00:03:10 01:00:12:05 01:00:15:15\n"
            "* FROM CLIP NAME: MARCO SC12\n"
            "* COMMENT: CHARACTER: Marco | DIALOGUE: Non ci credo\n"
            "002  AX       V     C        01:00:00:00 01:00:02:00 01:01:30:10 01:01:32:10\n"
            "* FROM CLIP NAME: GIULIA SC14\n"
            "* LOC: 01:03:00:00 RED  LUCA - tosse\n";
        auto r2 = fc::adr::parse (edl);
        expect (r2.format.startsWith ("EDL") && r2.cues.size() == 3 && r2.cues[0].tcIn == "01:00:12:05" && r2.cues[0].tcOut == "01:00:15:15"
                    && r2.cues[0].name == "MARCO SC12" && r2.cues[0].character == "Marco" && r2.cues[2].tcIn == "01:03:00:00",
                "ADR: riconosciuta l'EDL (eventi + locatori), " + juce::String ((int) r2.cues.size()) + " cue");

        // 3. tabella (cue sheet) con punto e virgola, intestazioni in italiano
        const juce::String csv =
            "Cue;Personaggio;Scena;Rullo;TC IN;TC OUT;Dialogo;Note\n"
            "1;Marco;12;R1;01:00:12:05;01:00:15:00;Non ci credo;Piu' forte\n"
            "2;Giulia;14;R1;01:01:30:10;01:01:33:00;\"Non e' possibile, davvero\";\n";
        auto r3 = fc::adr::parse (csv);
        expect (r3.format.startsWith ("Tabella") && r3.cues.size() == 2 && r3.cues[0].character == "Marco" && r3.cues[0].reel == "R1" && r3.cues[0].notes == "Piu' forte"
                    && r3.cues[1].dialogue == "Non e' possibile, davvero" && r3.cues[1].tcOut == "01:01:33:00", "ADR: riconosciuta la tabella CSV con intestazioni italiane");

        // tabella con tabulazioni e intestazioni inglesi
        const juce::String tsv = "#\tCharacter\tScene\tIn\tOut\tLine\n7\tBob\t3\t00:00:10:00\t00:00:12:00\tHello there\n";
        auto r4 = fc::adr::parse (tsv);
        expect (r4.cues.size() == 1 && r4.cues[0].number == "7" && r4.cues[0].dialogue == "Hello there" && r4.cues[0].tcOut == "00:00:12:00", "ADR: tabella TSV con intestazioni inglesi");

        // file non riconosciuto
        expect (fc::adr::parse ("ciao a tutti\nnessun timecode qui").cues.empty(), "ADR: un file non riconosciuto non produce cue");

        // operazioni sui cue e sui take
        auto& pr = p;
        pr.addCues (cues, true);
        expect (pr.getSession().cues.size() == 3 && pr.getSession().cues[0].number == 1 && pr.getSession().cues[2].number == 3, "ADR: cue aggiunti e numerati");
        const int cueId = pr.getSession().cues[0].id;
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "take";
        asset->audio.setSize (2, 1000);
        asset->audio.clear();
        asset->buildPeaks();
        for (int tk = 1; tk <= 3; ++tk)
        {
            fc::Clip c; c.id = 10000 + tk; c.asset = asset; c.start = pr.getSession().cues[0].inPos; c.length = 1000; c.cueId = cueId; c.takeNo = tk; c.muted = tk != 3;
            pr.edit().tracks[0].clips.push_back (c);
        }
        expect (pr.takeCount (cueId) == 3 && pr.nextTakeNumber (cueId) == 4, "ADR: conteggio e numerazione dei take");
        pr.setActiveTake (cueId, 2);
        bool onlyTwo = true;
        for (auto& c : pr.getSession().tracks[0].clips) if (c.cueId == cueId) onlyTwo = onlyTwo && (c.muted == (c.takeNo != 2));
        expect (onlyTwo && pr.findCue (cueId)->activeTake == 2, "ADR: attivare un take silenzia gli altri");
        pr.undo();
        bool restored = pr.findCue (cueId)->activeTake == 0;
        for (auto& c : pr.getSession().tracks[0].clips) if (c.cueId == cueId) restored = restored && (c.muted == (c.takeNo != 3));
        expect (restored, "ADR: undo dell'attivazione ripristina il take precedente");
        // riesportazione nel formato Pro Tools e rilettura: i cue devono tornare uguali
        pr.editCue (cueId, [] (fc::AdrCue& c) { c.character = "Marco"; c.scene = "12"; c.dialogue = "Non ci credo | davvero"; });
        {
            const auto txt = fc::adrToProToolsText (pr, "Test");
            auto back = fc::adr::parse (txt);
            juce::StringArray w3;
            auto again = fc::adr::toCues (back, pr, 48000.0, 4.0, w3);
            const auto& orig = pr.getSession().cues;
            bool same = back.cues.size() == orig.size() && again.size() == orig.size() && back.sessionStartTc == pr.formatTc (0.0);
            for (size_t i = 0; same && i < orig.size(); ++i)
                same = again[i].inPos == orig[i].inPos && again[i].outPos == orig[i].outPos && again[i].character == orig[i].character
                    && again[i].scene == orig[i].scene && again[i].dialogue == orig[i].dialogue.replaceCharacter ('|', ' ');
            expect (same, "ADR: l'export in formato Pro Tools si rilegge con gli stessi cue (" + juce::String ((int) back.cues.size()) + " cue)");
        }

        // take attivi come BWF con timecode
        pr.setActiveTake (cueId, 3);
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("arotron-adr-test");
            dir.deleteRecursively();
            juce::StringArray errs;
            const int n = fc::exportAdrTakes (pr, dir, errs);
            expect (n == 1 && errs.isEmpty(), "ADR: esportato un file BWF per ogni cue con take attivo");
            auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
            bool refOk = false;
            if (files.size() == 1)
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (files[0].createInputStream().release(), true));
                if (rd != nullptr)
                {
                    const auto expected = (juce::int64) std::llround (pr.tcStartSec * 48000.0) + pr.getSession().cues[0].inPos;
                    refOk = rd->metadataValues.getValue (juce::WavAudioFormat::bwavTimeReference, "").getLargeIntValue() == expected && rd->lengthInSamples == 1000;
                }
            }
            expect (refOk, "ADR: il BWF porta il timecode (time reference) della posizione sulla timeline");
            dir.deleteRecursively();
        }

        // eliminazione di un take: l'attivo passa al piu' recente rimasto
        pr.deleteTake (cueId, 3);
        expect (pr.takeCount (cueId) == 2 && pr.findCue (cueId)->activeTake == 2, "ADR: eliminando il take attivo diventa attivo il precedente");

        pr.removeCue (cueId);
        expect (pr.findCue (cueId) == nullptr, "ADR: eliminazione del cue");
    }

    void stepTimecode()
    {
        const auto& f25 = fc::tcFormats()[2];
        const auto& f2398 = fc::tcFormats()[0];
        const auto& fdf = fc::tcFormats()[4];
        const auto& fndf = fc::tcFormats()[3];
        expect (fc::tcFormatForFps (24000.0 / 1001.0) == 0 && fc::tcFormatForFps (23.976) == 0 && fc::tcFormatForFps (29.97) == 3 && fc::tcFormatForFps (24.0) == 1 && fc::tcFormatForFps (30.0) == 5 && fc::tcFormatForFps (50.0) == 6 && fc::tcFormatForFps (59.94) == 7,
                "fps del video -> formato di timecode piu' vicino (23,976 / 24 / 29,97 / 30 / 50 / 59,94)");
        expect (fc::secondsToTimecode (3600.0, f25) == "01:00:00:00", "timecode 25 fps: 3600 s = 01:00:00:00");
        double sec = 0.0;
        expect (fc::timecodeToSeconds ("01:00:10:12", f25, sec) && std::abs (sec - 3610.48) < 1.0e-6, "timecode 25 fps: lettura di 01:00:10:12");
        expect (fc::timecodeToSeconds ("01:00:00:00", f2398, sec) && std::abs (sec - 3603.6) < 1.0e-6, "timecode 23,976: 01:00:00:00 = 3603,6 s (86400 fotogrammi)");
        expect (fc::framesToTimecode (1800, fdf) == "00:01:00;02" && fc::framesToTimecode (17982, fdf) == "00:10:00;00" && fc::framesToTimecode (1799, fdf) == "00:00:59;29",
                "timecode drop-frame: salta i numeri 00 e 01 a ogni minuto tranne il decimo");
        bool roundTrip = true;
        for (int64_t fr = 0; fr < 400000; fr += 997)
        {
            int64_t back = -1;
            roundTrip = roundTrip && fc::timecodeToFrames (fc::framesToTimecode (fr, fdf), fdf, back) && back == fr;
            roundTrip = roundTrip && fc::timecodeToFrames (fc::framesToTimecode (fr, fndf), fndf, back) && back == fr;
        }
        expect (roundTrip, "timecode: andata e ritorno su 29,97 drop e non-drop");
        int64_t dummy = 0;
        expect (! fc::timecodeToFrames ("01:00:00", f25, dummy) && ! fc::timecodeToFrames ("01:00:00:30", f25, dummy) && ! fc::timecodeToFrames ("ab:cd:ef:gh", f25, dummy),
                "timecode: i testi non validi vengono rifiutati");
        fc::Project p (formats, 48000.0);
        p.tcFormat = 2;
        p.tcStartSec = 3600.0;
        double tl = 0.0;
        expect (p.formatTc (10.0) == "01:00:10:00" && p.parseTc ("01:00:12:05", tl) && std::abs (tl - 12.2) < 1.0e-6, "sessione con inizio 01:00:00:00: posizione <-> timecode");
    }

    void stepCrossfade()
    {
        auto mk = [] (float v)
        {
            auto a = std::make_shared<fc::Asset>();
            a->name = "dc";
            a->audio.setSize (2, 8000);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 8000; ++i) a->audio.setSample (c, i, v);
            a->buildPeaks();
            return a;
        };
        auto renderAt = [] (fc::Session s, int64_t at)
        {
            s.computeCrossfades();
            fc::Mixer m;
            juce::AudioBuffer<float> buf (2, 2048);
            m.render (s, at - 2048 + 1, buf, 0, 2048);   // l'ultimo campione e' proprio 'at'
            return buf.getSample (0, 2047);
        };
        fc::Project p (formats, 48000.0);
        p.edit().master->limiterOn = false;
        auto& t = p.edit().tracks;

        // stesso materiale continuo (offset coerente): a meta' sovrapposizione il livello resta 0.5
        auto one = mk (0.5f);
        fc::Clip a; a.id = 9000; a.asset = one; a.start = 0; a.length = 3000;
        fc::Clip b; b.id = 9001; b.asset = one; b.start = 2000; b.offset = 2000; b.length = 3000;
        t[0].clips = { a, b };
        expect (std::abs (renderAt (p.getSession(), 2500) - 0.5f) < 1.0e-3f, "crossfade: stesso materiale, guadagno costante a meta' (0.5)");
        expect (std::abs (renderAt (p.getSession(), 1500) - 0.5f) < 1.0e-4f && std::abs (renderAt (p.getSession(), 3500) - 0.5f) < 1.0e-4f, "crossfade: fuori dalla sovrapposizione il livello e' intatto");

        // materiali diversi: potenza costante, a meta' 0.5 * (cos 45 + sin 45) = 0.707
        fc::Clip c2 = b; c2.asset = mk (0.5f); c2.offset = 0;
        t[0].clips = { a, c2 };
        const float mid = renderAt (p.getSession(), 2500);
        expect (std::abs (mid - 0.5f * 1.41421356f * 0.5f * 2.0f * 0.5f) < 0.01f || std::abs (mid - 0.7071f * 0.5f * 2.0f) < 0.01f, "crossfade: materiali diversi, potenza costante (" + juce::String (mid, 3) + ")");

        // niente sovrapposizione: nessuna dissolvenza implicita
        fc::Clip d = b; d.start = 3000;
        t[0].clips = { a, d };
        auto s2 = p.getSession();
        s2.computeCrossfades();
        expect (s2.tracks[0].clips[0].xfadeOut == 0 && s2.tracks[0].clips[1].xfadeIn == 0, "crossfade: nessuno senza sovrapposizione");
    }

    void stepAutomation()
    {
        auto dc = std::make_shared<fc::Asset>();
        dc->name = "dc";
        dc->audio.setSize (2, 5 * 48000);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < dc->audio.getNumSamples(); ++i) dc->audio.setSample (c, i, 0.5f);
        dc->buildPeaks();

        fc::Project p (formats, 48000.0);
        auto& t = p.edit().tracks;
        fc::Clip c; c.id = 8000; c.asset = dc; c.start = 0; c.length = 5 * 48000;
        t[0].clips = { c };
        p.edit().master->limiterOn = false;
        const int tid = t[0].id;
        p.addAutoPoint (tid, 0, -20.0f);
        p.addAutoPoint (tid, 2 * 48000, 0.0f);

        auto level = [&] (int64_t at)
        {
            auto s = p.getSession().cloneDetached();
            fc::Mixer m;
            juce::AudioBuffer<float> buf (2, 2048);
            // prima si scalda la rampa del guadagno
            m.render (s, at - 4096, buf, 0, 2048);
            m.render (s, at - 2048, buf, 0, 2048);
            return buf.getSample (0, 2047);
        };
        expect (t[0].live->autoMode.load() == 1, "automazione: il primo punto attiva la modalita' lettura");
        const float mid = level (48000 + 2048), end = level (3 * 48000);
        expect (std::abs (mid - 0.5f * juce::Decibels::decibelsToGain (fc::evalAutomationDb (t[0].volAuto, 48000 + 2048))) < 0.003f, "automazione: la rampa -20 -> 0 dB e' interpolata (" + juce::String (mid, 3) + ")");
        expect (std::abs (end - 0.5f) < 0.002f, "automazione: dopo l'ultimo punto 0 dB (" + juce::String (end, 3) + ")");
        t[0].live->autoMode = 0;
        expect (std::abs (level (48000 + 2048) - 0.5f) < 0.002f, "automazione spenta: vale il fader");
        t[0].live->autoMode = 1;

        // scrittura "touch": sostituisce la regione toccata, un solo undo
        const size_t before = t[0].volAuto.size();
        p.writeVolumeAuto (tid, 10000, -6.0f);
        p.writeVolumeAuto (tid, 20000, -8.0f);
        p.writeVolumeAuto (tid, 30000, -10.0f);
        p.endAutoTouch();
        expect (t[0].volAuto.size() == before + 3 && std::is_sorted (t[0].volAuto.begin(), t[0].volAuto.end(),
                    [] (const fc::AutoPoint& a, const fc::AutoPoint& b) { return a.pos < b.pos; }), "automazione: scrittura touch, punti ordinati");
        p.undo();
        expect (t[0].volAuto.size() == before, "automazione: un solo undo per tutto il touch");

        // eliminazione di un punto e cancellazione totale
        p.removeAutoPoint (tid, t[0].volAuto.front().id);
        expect (t[0].volAuto.size() == before - 1, "automazione: elimina punto");
        p.clearAutomation (tid);
        expect (t[0].volAuto.empty(), "automazione: cancella tutto");
    }

    void stepGroups()
    {
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "impulso";
        asset->audio.setSize (2, 6000);
        asset->audio.clear();
        asset->audio.setSample (0, 1000, 1.0f);
        asset->audio.setSample (1, 1000, 1.0f);
        asset->buildPeaks();

        fc::Project p (formats, 48000.0);
        p.addTrack();
        expect (p.getSession().tracks.size() >= 2, "gruppi: due tracce di prova");
        for (int i = 0; i < 2; ++i)
        {
            fc::Clip c; c.id = 7000 + i; c.asset = asset; c.start = 0; c.length = 6000;
            p.edit().tracks[(size_t) i].clips = { c };
        }
        p.edit().master->limiterOn = false;
        auto& tr = p.edit().tracks;
        tr[1].live->mute = true;   // all'inizio suona solo la traccia 0

        auto render = [&] (bool pdc)
        {
            auto s = p.getSession().cloneDetached();
            fc::Mixer m;
            m.setPdcEnabled (pdc);
            juce::AudioBuffer<float> buf (2, 2048);
            std::vector<float> out;
            for (int pos = 0; pos < 8192; pos += 2048)
            {
                m.render (s, pos, buf, 0, 2048);
                for (int i = 0; i < 2048; ++i) out.push_back (buf.getSample (0, i));
            }
            return out;
        };

        tr[0].live->outBus = 1;
        expect (std::abs (render (true)[1000] - 1.0f) < 1.0e-5f, "gruppi: traccia su Gruppo 1 arriva al master");

        p.getSession().group[0]->volumeDb = -6.0f;
        expect (std::abs (render (true)[1000] - juce::Decibels::decibelsToGain (-6.0f)) < 1.0e-3f, "gruppi: il fader del gruppo agisce sulla traccia");

        p.getSession().group[0]->mute = true;
        expect (std::abs (render (true)[1000]) < 1.0e-6f, "gruppi: gruppo in mute silenzia le sue tracce");
        p.getSession().group[0]->mute = false;
        p.getSession().group[0]->volumeDb = 0.0f;

        // una traccia su un gruppo spento non sente il gruppo: il mute del gruppo non tocca le altre
        tr[1].live->mute = false;
        p.getSession().group[0]->mute = true;
        expect (std::abs (render (true)[1000] - 1.0f) < 1.0e-5f, "gruppi: il mute del gruppo non tocca le tracce dirette");
        p.getSession().group[0]->mute = false;

        // send FX da una traccia su un gruppo: il send e' post-fader della traccia, quindi non passa dal gruppo
        tr[1].live->mute = true;
        tr[0].live->sendDb[0] = 0.0f;
        expect (std::abs (render (true)[1000] - 2.0f) < 1.0e-4f, "gruppi: il send FX resta attivo con la traccia su un gruppo");
        tr[0].live->sendDb[0] = -80.0f;

        // plug-in con 480 campioni sul gruppo: traccia diretta e traccia sul gruppo restano allineate
        tr[1].live->mute = false;
        auto slot = std::make_shared<fc::InsertSlot>();
        slot->name = "DelayTest";
        slot->plugin = std::make_unique<DelayTestProcessor> (480);
        slot->plugin->setRateAndBufferSizeDetails (48000.0, 2048);
        slot->plugin->prepareToPlay (48000.0, 2048);
        slot->ready = true;
        auto chain = std::make_shared<fc::InsertChain>();
        chain->slots.push_back (slot);
        p.getSession().group[0]->setChain (chain);
        slot->plugin->reset();
        auto aligned = render (true);
        expect (fc::Mixer::computeLatency (p.getSession()) == 480, "gruppi: latenza del gruppo inclusa nel PDC");
        expect (std::abs (aligned[1480] - 2.0f) < 1.0e-4f && std::abs (aligned[1000]) < 1.0e-5f, "gruppi: diretta e gruppo allineate a 1480 (" + juce::String (aligned[1480], 3) + ")");
        slot->plugin->reset();
        auto unaligned = render (false);
        expect (std::abs (unaligned[1000] - 1.0f) < 1.0e-4f && std::abs (unaligned[1480] - 1.0f) < 1.0e-4f, "gruppi: con PDC spento sono sfasate");

    }

    // frequenza (Hz) di una sinusoide stimata dai passaggi per lo zero in salita tra a e b campioni
    static double zeroCrossHz (const juce::AudioBuffer<float>& b, int a, int e, double rate)
    {
        int n = 0;
        for (int i = a + 1; i < e; ++i) if (b.getSample (0, i - 1) <= 0.0f && b.getSample (0, i) > 0.0f) ++n;
        return (double) n * rate / (double) (e - a);
    }

    void stepStretch (std::function<void()> next)
    {
        juce::AudioBuffer<float> sine (2, 48000);
        for (int i = 0; i < 48000; ++i)
        {
            const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / 48000.0);
            sine.setSample (0, i, v);
            sine.setSample (1, i, -v);
        }
        for (double ratio : { 1.5, 0.7 })
        {
            auto out = fc::timeStretch (sine, 0, 48000, ratio, 48000.0);
            const int len = out.getNumSamples();
            expect (len == (int) std::llround (48000 * ratio) && out.getNumChannels() == 2, "time-stretch " + juce::String (ratio) + ": lunghezza " + juce::String (len));
            const double hz = zeroCrossHz (out, len / 4, len * 3 / 4, 48000.0);
            expect (std::abs (hz - 440.0) < 440.0 * 0.03, "time-stretch " + juce::String (ratio) + ": il tono resta 440 Hz (" + juce::String (hz, 1) + ")");
            const float rms = out.getRMSLevel (0, len / 4, len / 2);
            expect (std::abs (rms - 0.3536f) < 0.04f, "time-stretch " + juce::String (ratio) + ": il livello resta (RMS " + juce::String (rms, 3) + ")");
            expect (out.getSample (1, len / 3) * out.getSample (0, len / 3) <= 0.0f, "time-stretch: i canali restano coerenti");
        }
        {
            auto out = fc::varispeed (sine, 0, 48000, 2.0);
            const double hz = zeroCrossHz (out, 10000, 80000, 48000.0);
            expect (out.getNumSamples() == 96000 && std::abs (hz - 220.0) < 5.0, "varispeed x2: durata doppia e tono un'ottava sotto (" + juce::String (hz, 1) + " Hz)");
        }

        // operazione sul progetto: sostituisce l'asset, un solo undo la annulla
        stretchProject = std::make_unique<fc::Project> (formats, 48000.0);
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "sine";
        asset->audio = sine;
        asset->buildPeaks();
        fc::Clip c; c.id = 9100; c.asset = asset; c.start = 24000; c.length = 48000; c.fadeIn = 4800;
        stretchProject->edit().tracks[0].clips = { c };
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("arotron-stretch-test");
        folder.deleteRecursively();
        stretchProject->stretchClip (9100, 1.5, true, folder, [this, next, folder] (juce::String err)
        {
            auto* cl = stretchProject->findClip (9100);
            expect (err.isEmpty() && cl != nullptr && cl->length == 72000 && cl->start == 12000 && cl->fadeIn == 7200 && cl->asset->file.existsAsFile(),
                    "time-stretch del clip: durata 72000, nuovo inizio (stretch dal bordo sinistro) e dissolvenza scalate " + err);
            stretchProject->undo();
            auto* back = stretchProject->findClip (9100);
            expect (back != nullptr && back->length == 48000 && back->fadeIn == 4800 && back->asset->file == juce::File(), "time-stretch: l'undo ripristina il clip originale");
            folder.deleteRecursively();
            next();
        }, 12000);
    }

    std::unique_ptr<fc::Project> stretchProject;

    void stepEq()
    {
        static constexpr double rate = 48000.0;
        auto sine = [] (double hz, int n) { juce::AudioBuffer<float> b (2, n); for (int i = 0; i < n; ++i) { const float v = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate); b.setSample (0, i, v); b.setSample (1, i, v); } return b; };
        auto gainDbAt = [&] (fc::ParamEq& eq, double hz)
        {
            auto b = sine (hz, 24000);
            eq.process (b.getWritePointer (0), b.getWritePointer (1), 24000, rate);
            return (double) juce::Decibels::gainToDecibels (b.getRMSLevel (0, 12000, 12000) / 0.17678f);
        };

        fc::ParamEq off;
        auto src = sine (1000.0, 4096), cp = src;
        off.process (cp.getWritePointer (0), cp.getWritePointer (1), 4096, rate);
        bool same = true;
        for (int i = 0; i < 4096; ++i) same = same && cp.getSample (0, i) == src.getSample (0, i);
        expect (same, "EQ: spento il segnale passa invariato");

        fc::ParamEq bell;
        bell.enabled = true;
        bell.band[1].gainDb = 12.0f; bell.band[1].freq = 1000.0f; bell.band[1].q = 1.0f;
        expect (std::abs (bell.responseDb (1000.0, rate) - 12.0) < 0.05, "EQ: la curva teorica a 1 kHz e' +12 dB (" + juce::String (bell.responseDb (1000.0, rate), 2) + ")");
        const double g1k = gainDbAt (bell, 1000.0);
        fc::ParamEq bell2; bell2.copyFrom (bell);
        const double g100 = gainDbAt (bell2, 100.0);
        expect (std::abs (g1k - 12.0) < 0.3 && std::abs (g100) < 0.8, "EQ: campana +12 dB a 1 kHz misurata " + juce::String (g1k, 2) + " dB, a 100 Hz " + juce::String (g100, 2) + " dB");

        fc::ParamEq hp;
        hp.enabled = true;
        hp.band[0].type = fc::ParamEq::HighPass; hp.band[0].freq = 1000.0f; hp.band[0].q = 0.707f;
        const double gHp = gainDbAt (hp, 100.0);
        fc::ParamEq hp2; hp2.copyFrom (hp);
        const double gHpPass = gainDbAt (hp2, 8000.0);
        expect (gHp < -30.0 && std::abs (gHpPass) < 0.5, "EQ: passa-alto a 1 kHz: 100 Hz " + juce::String (gHp, 1) + " dB, 8 kHz " + juce::String (gHpPass, 2) + " dB");

        fc::ParamEq sh;
        sh.enabled = true;
        sh.band[3].gainDb = -9.0f; sh.band[3].freq = 4000.0f;
        const double gSh = gainDbAt (sh, 16000.0);
        fc::ParamEq sh2; sh2.copyFrom (sh);
        const double gShLow = gainDbAt (sh2, 200.0);
        expect (std::abs (gSh + 9.0) < 1.2 && std::abs (gShLow) < 0.3, "EQ: shelf alto -9 dB: 16 kHz " + juce::String (gSh, 2) + " dB, 200 Hz " + juce::String (gShLow, 2) + " dB");

        // nel mixer: traccia con EQ, stesso risultato e canale destro uguale al sinistro
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "sin1k";
        asset->audio = sine (1000.0, 24000);
        asset->buildPeaks();
        fc::Project p (formats, rate);
        fc::Clip c; c.id = 9500; c.asset = asset; c.start = 0; c.length = 24000;
        p.edit().tracks[0].clips = { c };
        p.edit().master->limiterOn = false;
        auto& eq = p.edit().tracks[0].live->eq;
        eq.enabled = true; eq.band[1].gainDb = 6.0f; eq.band[1].freq = 1000.0f;
        auto s = p.getSession().cloneDetached();
        fc::Mixer m;
        juce::AudioBuffer<float> buf (2, 2048), full (2, 24576);
        for (int pos = 0; pos < 24576; pos += 2048) { m.render (s, pos, buf, 0, 2048); for (int ch = 0; ch < 2; ++ch) full.copyFrom (ch, pos, buf, ch, 0, 2048); }
        const double mixDb = (double) juce::Decibels::gainToDecibels (full.getRMSLevel (0, 12000, 8000) / 0.17678f);
        expect (std::abs (mixDb - 6.0) < 0.3 && std::abs (full.getSample (1, 15000) - full.getSample (0, 15000)) < 1.0e-6f, "EQ: nel mixer +6 dB a 1 kHz (" + juce::String (mixDb, 2) + " dB), canali uguali");
    }

    void stepAudition()
    {
        auto make = [] (float v) { auto a = std::make_shared<fc::Asset>(); a->name = "t"; a->audio.setSize (2, 3000); a->audio.clear(); a->audio.setSample (0, 1000, v); a->audio.setSample (1, 1000, v); a->buildPeaks(); return a; };
        fc::Project p (formats, 48000.0);
        fc::Clip c1; c1.id = 9800; c1.asset = make (1.0f); c1.length = 3000; c1.cueId = 1; c1.takeNo = 1; c1.muted = true;
        fc::Clip c2 = c1; c2.id = 9801; c2.asset = make (0.5f); c2.takeNo = 2; c2.muted = false;
        fc::Clip c3 = c1; c3.id = 9802; c3.asset = make (0.25f); c3.cueId = 0; c3.takeNo = 0; c3.muted = false;   // audio di produzione, non e' un take
        p.edit().tracks[0].clips = { c1, c2 };
        p.edit().tracks[1].clips = { c3 };
        p.edit().master->limiterOn = false;
        auto s = p.getSession().cloneDetached();
        auto at1000 = [&] (int64_t audition)
        {
            fc::Mixer m;
            m.auditionClip = audition;
            juce::AudioBuffer<float> buf (2, 2048);
            m.render (s, 0, buf, 0, 2048);
            return buf.getSample (0, 1000);
        };
        expect (std::abs (at1000 (0) - 0.75f) < 1.0e-5f, "ascolto: normalmente suona il take attivo piu' l'audio di produzione");
        expect (std::abs (at1000 (-1) - 0.25f) < 1.0e-5f, "ascolto dell'originale: solo l'audio di produzione, nessun take ADR");
        expect (std::abs (at1000 (9800) - 1.0f) < 1.0e-5f, "ascolto: un take silenziato si ascolta da solo");
        s.tracks[0].live->mute = true;
        expect (std::abs (at1000 (9800) - 1.0f) < 1.0e-5f && std::abs (at1000 (0) - 0.25f) < 1.0e-5f, "ascolto: ignora il mute della traccia, il mix normale lo rispetta");
    }

    void stepTrackLimiter()
    {
        auto make = [] (int n, std::function<float (int)> f) { auto a = std::make_shared<fc::Asset>(); a->name = "t"; a->audio.setSize (2, n); for (int i = 0; i < n; ++i) { a->audio.setSample (0, i, f (i)); a->audio.setSample (1, i, f (i)); } a->buildPeaks(); return a; };
        auto renderAll = [] (fc::Session s, bool pdc, int n)
        {
            fc::Mixer m;
            m.setPdcEnabled (pdc);
            juce::AudioBuffer<float> buf (2, 2048);
            std::vector<float> out;
            for (int pos = 0; pos < n; pos += 2048) { m.render (s, pos, buf, 0, 2048); for (int i = 0; i < 2048; ++i) out.push_back (buf.getSample (0, i)); }
            return out;
        };
        fc::Project p (formats, 48000.0);
        expect (! p.getSession().tracks[0].live->limiterOn.load(), "limiter di traccia: spento di default");

        // un seno a 1,5 (sopra lo 0 dBFS) con tetto a -6 dB non supera 0,501
        fc::Clip c; c.id = 9900; c.asset = make (48000, [] (int i) { return 1.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / 48000.0); }); c.length = 48000;
        p.edit().tracks[0].clips = { c };
        p.edit().master->limiterOn = false;
        auto& live = *p.edit().tracks[0].live;
        live.limiterOn = true; live.ceilingDb = -6.0f;
        auto out = renderAll (p.getSession().cloneDetached(), true, 49152);
        float peak = 0.0f, rms = 0.0f;
        for (int i = 2000; i < 45000; ++i) { peak = juce::jmax (peak, std::abs (out[(size_t) i])); rms += out[(size_t) i] * out[(size_t) i]; }
        rms = std::sqrt (rms / 43000.0f);
        expect (peak <= 0.5013f && rms > 0.30f, "limiter di traccia: picco " + juce::String (peak, 4) + " (tetto 0,5012), RMS " + juce::String (rms, 3));
        expect (fc::Mixer::computeLatency (p.getSession()) == fc::Limiter::latencyFor (48000.0), "limiter di traccia: la latenza entra nel PDC (" + juce::String (fc::Mixer::computeLatency (p.getSession())) + " campioni)");

        // allineamento con una traccia senza limiter: due impulsi sotto il tetto arrivano insieme
        auto imp = make (6000, [] (int i) { return i == 1000 ? 0.1f : 0.0f; });
        c.id = 9901; c.asset = imp; c.length = 6000;
        p.edit().tracks[0].clips = { c };
        c.id = 9902;
        p.edit().tracks[1].clips = { c };
        live.ceilingDb = 0.0f;
        const int d = fc::Limiter::latencyFor (48000.0);
        auto al = renderAll (p.getSession().cloneDetached(), true, 8192);
        expect (std::abs (al[(size_t) (1000 + d)] - 0.2f) < 1.0e-4f && std::abs (al[1000]) < 1.0e-5f, "limiter di traccia: allineata alle altre tracce dal PDC (" + juce::String (al[(size_t) (1000 + d)], 3) + ")");

        // la traccia disattivata non passa dal limiter e non ritarda
        live.limiterOn = false;
        auto off = renderAll (p.getSession().cloneDetached(), true, 8192);
        expect (std::abs (off[1000] - 0.2f) < 1.0e-4f, "limiter di traccia spento: nessun ritardo");
    }

    void stepAutoLanes()
    {
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "dc";
        asset->audio.setSize (2, 30000);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 30000; ++i) asset->audio.setSample (ch, i, 0.5f);
        asset->buildPeaks();
        fc::Project p (formats, 48000.0);
        fc::Clip c; c.id = 9950; c.asset = asset; c.length = 30000;
        p.edit().tracks[0].clips = { c };
        p.edit().master->limiterOn = false;
        auto& tr = p.edit().tracks[0];
        auto render = [&]
        {
            fc::Mixer m;
            auto s = p.getSession().cloneDetached();
            juce::AudioBuffer<float> buf (2, 2048);
            std::array<std::vector<float>, 2> out;
            for (int pos = 0; pos < 30720; pos += 2048) { m.render (s, pos, buf, 0, 2048); for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 2048; ++i) out[(size_t) ch].push_back (buf.getSample (ch, i)); }
            return out;
        };
        auto mk = [] (int64_t pos, float v) { fc::AutoPoint a; a.pos = pos; a.db = v; return a; };

        // pan: da tutto a sinistra (-1) a tutto a destra (+1) in 24000 campioni
        tr.panAuto = { mk (0, -1.0f), mk (24000, 1.0f) };
        tr.live->autoMode = 1;
        auto o = render();
        const float lStart = o[0][10], rStart = o[1][10], lEnd = o[0][26000], rEnd = o[1][26000];
        expect (lStart > 0.49f && rStart < 0.12f && rEnd > 0.49f && lEnd < 0.01f, "automazione pan: sinistra all'inizio, destra alla fine (L " + juce::String (lStart, 2) + " R " + juce::String (rStart, 2) + " -> L " + juce::String (lEnd, 2) + " R " + juce::String (rEnd, 2) + ")");
        expect (std::abs (o[0][12000] - 0.5f) < 0.03f && std::abs (o[1][12000] - 0.5f) < 0.03f, "automazione pan: al centro della rampa entrambi i canali sono al massimo");
        tr.live->autoMode = 0;
        o = render();
        expect (std::abs (o[0][10] - 0.5f) < 1.0e-4f && std::abs (o[1][10] - 0.5f) < 1.0e-4f, "automazione pan spenta: vale il controllo");
        tr.panAuto.clear();

        // send A: da spento a 0 dB dopo 6000 campioni, il ritorno FX somma lo stesso segnale
        tr.sendAAuto = { mk (0, -80.0f), mk (6000, 0.0f) };
        tr.live->autoMode = 1;
        o = render();
        expect (std::abs (o[0][100] - 0.5f) < 0.02f && std::abs (o[0][12000] - 1.0f) < 0.02f, "automazione send A: ritorno assente poi a 0 dB (" + juce::String (o[0][100], 3) + " -> " + juce::String (o[0][12000], 3) + ")");

        // curva, cancellazione e undo lavorano sulla curva scelta
        tr.live->autoMode = 0;
        const int tid = p.getSession().tracks[0].id;
        p.getSession().tracks[0].live->autoParam = fc::AutoPan;
        const int id1 = p.addAutoPoint (tid, 1000, 0.5f);
        const int id2 = p.addAutoPoint (tid, 2000, 3.0f);   // oltre il limite: viene riportato a +1
        (void) id1;
        expect (p.getSession().tracks[0].panAuto.size() == 2 && std::abs (p.getSession().tracks[0].panAuto[1].db - 1.0f) < 1.0e-5f && p.getSession().tracks[0].volAuto.empty(), "automazione: i punti vanno nella curva scelta e sono limitati al suo intervallo");
        p.removeAutoPoint (tid, id2);
        expect (p.getSession().tracks[0].panAuto.size() == 1, "automazione: elimina un punto della curva scelta");
        p.writeAuto (tid, fc::AutoSendB, 500, -12.0f);
        p.endAutoTouch();
        expect (p.getSession().tracks[0].sendBAuto.size() == 1 && p.getSession().tracks[0].panAuto.size() == 1, "automazione: la scrittura touch di un send non tocca le altre curve");
    }

    void stepPresets()
    {
        constexpr double rate = 48000.0;
        auto sine = [] (float amp, int n) { juce::AudioBuffer<float> b (2, n); for (int i = 0; i < n; ++i) { const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate); b.setSample (0, i, v); b.setSample (1, i, v); } return b; };
        auto peakAfter = [] (juce::AudioBuffer<float>& b, int from) { return b.getMagnitude (0, from, b.getNumSamples() - from); };

        // compressore: seno a -6 dBFS, soglia -20, 4:1 -> uscita a -20 + 14/4 = -16,5 dBFS
        fc::TrackComp k;
        k.enabled = true; k.thresholdDb = -20.0f; k.ratio = 4.0f; k.attackMs = 1.0f; k.releaseMs = 50.0f; k.kneeDb = 0.0f; k.makeupDb = 0.0f;
        auto b = sine (0.5f, 48000);
        k.process (b.getWritePointer (0), b.getWritePointer (1), 48000, rate);
        const float outDb = juce::Decibels::gainToDecibels (peakAfter (b, 24000));
        expect (std::abs (outDb + 16.5f) < 0.6f, "compressore: -6 dBFS con soglia -20 e 4:1 esce a " + juce::String (outDb, 2) + " dBFS (attesi -16,5)");
        auto quiet = sine (0.05f, 24000);
        k.process (quiet.getWritePointer (0), quiet.getWritePointer (1), 24000, rate);
        expect (std::abs (juce::Decibels::gainToDecibels (peakAfter (quiet, 8000)) + 26.0f) < 0.3f, "compressore: sotto la soglia il segnale non cambia");
        k.makeupDb = 6.0f;
        auto up = sine (0.05f, 24000);
        k.process (up.getWritePointer (0), up.getWritePointer (1), 24000, rate);
        expect (std::abs (juce::Decibels::gainToDecibels (peakAfter (up, 8000)) + 20.0f) < 0.4f, "compressore: il guadagno di compensazione si somma (+6 dB)");
        k.enabled = false;
        auto off = sine (0.5f, 4096);
        auto offCopy = off;
        k.process (off.getWritePointer (0), off.getWritePointer (1), 4096, rate);
        bool same = true;
        for (int i = 0; i < 4096; ++i) same = same && off.getSample (0, i) == offCopy.getSample (0, i);
        expect (same, "compressore spento: passa invariato");

        // preset dei tipi di traccia
        fc::TrackLive l;
        fc::applyTrackPreset (l, fc::TrackDialogue);
        expect (l.trackType.load() == fc::TrackDialogue && l.eq.enabled.load() && l.eq.band[0].type.load() == fc::ParamEq::HighPass && std::abs (l.eq.band[0].freq.load() - 80.0f) < 0.1f
                    && l.comp.enabled.load() && std::abs (l.comp.ratio.load() - 3.0f) < 0.01f && l.limiterOn.load() && std::abs (l.ceilingDb.load() + 3.0f) < 0.01f,
                "preset dialogo: passa-alto 80 Hz, compressore 3:1, limiter a -3 dB");
        fc::applyTrackPreset (l, fc::TrackME);
        expect (l.trackType.load() == fc::TrackME && std::abs (l.volumeDb.load() + 6.0f) < 0.01f && std::abs (l.comp.ratio.load() - 2.0f) < 0.01f, "preset M&E: livello -6 dB, compressore 2:1");
        fc::applyTrackPreset (l, fc::TrackEffects);
        expect (l.trackType.load() == fc::TrackEffects && std::abs (l.comp.ratio.load() - 4.0f) < 0.01f && std::abs (l.volumeDb.load() + 3.0f) < 0.01f, "preset effetti: compressore 4:1, livello -3 dB");
        fc::applyTrackPreset (l, fc::TrackNone);
        expect (l.trackType.load() == 0 && ! l.eq.enabled.load() && ! l.comp.enabled.load() && ! l.limiterOn.load() && std::abs (l.volumeDb.load()) < 0.01f, "preset nessuno: tutto spento");

        // l'EQ del preset dialogo fa quello che dice: a 100 Hz attenua, a 3,5 kHz esalta
        fc::applyTrackPreset (l, fc::TrackDialogue);
        expect (l.eq.responseDb (40.0, rate) < -8.0 && std::abs (l.eq.responseDb (3500.0, rate) - 2.5) < 1.5, "preset dialogo: taglia i bassi e valorizza la presenza (40 Hz " + juce::String (l.eq.responseDb (40.0, rate), 1) + " dB, 3,5 kHz " + juce::String (l.eq.responseDb (3500.0, rate), 1) + " dB)");

        // nuova sessione: una sola traccia; un video si inserisce in cima
        fc::Project p (formats, rate);
        p.newSession();
        expect (p.getSession().tracks.size() == 1, "nuova sessione: una sola traccia");
        // setTrackType su una traccia del progetto
        p.setTrackType (p.getSession().tracks[0].id, fc::TrackME);
        expect (p.getSession().tracks[0].live->trackType.load() == fc::TrackME && p.getSession().tracks[0].live->comp.enabled.load(), "setTrackType applica la catena alla traccia");
    }

    void stepTrimAndPeaks()
    {
        // picchi per canale: sinistro forte, destro quasi muto
        fc::Asset a;
        a.audio.setSize (2, 4096);
        a.audio.clear();
        for (int i = 0; i < 4096; ++i) a.audio.setSample (0, i, 0.8f * (float) std::sin (i * 0.05));
        a.audio.setSample (1, 100, 0.1f);
        a.buildPeaks();
        float maxL = 0.0f, maxR = 0.0f;
        for (auto v : a.levels.front().mx) maxL = juce::jmax (maxL, v);
        for (auto v : a.levels.front().mx1) maxR = juce::jmax (maxR, v);
        expect (maxL > 0.7f && maxR > 0.09f && maxR < 0.11f && ! a.levels.front().mn1.empty(), "picchi per canale: sinistro " + juce::String (maxL, 2) + ", destro " + juce::String (maxR, 2));
        fc::Asset mono;
        mono.audio.setSize (1, 1024);
        mono.audio.clear();
        mono.buildPeaks();
        expect (mono.levels.front().mn1.empty(), "audio mono: un solo canale di picchi");

        // guadagno in ingresso della traccia: +6 dB raddoppia, -6 dB dimezza, salvato con la sessione (vedi il test di ricarica)
        fc::Project p (formats, 48000.0);
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "dc";
        asset->audio.setSize (2, 8192);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 8192; ++i) asset->audio.setSample (ch, i, 0.25f);
        asset->buildPeaks();
        fc::Clip c; c.id = 9990; c.asset = asset; c.length = 8192;
        p.edit().tracks[0].clips = { c };
        p.edit().master->limiterOn = false;
        float lastMeter = 0.0f;
        auto level = [&]
        {
            fc::Mixer m;
            auto sess = p.getSession().cloneDetached();
            juce::AudioBuffer<float> buf (2, 2048);
            for (int pos = 0; pos < 4096; pos += 2048) m.render (sess, pos, buf, 0, 2048);   // si legge il secondo blocco, a regime
            lastMeter = sess.tracks[0].live->meterL.load();
            return buf.getSample (0, 1500);
        };
        expect (std::abs (level() - 0.25f) < 1.0e-4f, "guadagno in ingresso: 0 dB lascia il livello");
        expect (std::abs (lastMeter - 0.25f) < 1.0e-3f, "vu-meter di traccia a 0 dB: " + juce::String (lastMeter, 3));
        p.getSession().tracks[0].live->trimDb = 6.0f;
        expect (std::abs (level() - 0.25f * juce::Decibels::decibelsToGain (6.0f)) < 1.0e-3f, "guadagno in ingresso: +6 dB");
        expect (std::abs (lastMeter - 0.25f * juce::Decibels::decibelsToGain (6.0f)) < 2.0e-3f, "vu-meter di traccia: misura dopo il guadagno (+6 dB) = " + juce::String (lastMeter, 3));
        p.getSession().tracks[0].live->trimDb = -6.0f;
        expect (std::abs (level() - 0.25f * juce::Decibels::decibelsToGain (-6.0f)) < 1.0e-3f, "guadagno in ingresso: -6 dB");
    }

    void stepSpectrum()
    {
        // sinusoide a -6 dBFS esattamente sul bin 170 (996,09 Hz a 48 kHz): il picco deve cadere li' e valere -6 dB
        std::vector<float> sig ((size_t) fc::SpectrumView::kSize);
        const double f = 170.0 * 48000.0 / (double) fc::SpectrumView::kSize;
        for (size_t i = 0; i < sig.size(); ++i) sig[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * f * (double) i / 48000.0);
        std::vector<float> bins;
        fc::SpectrumView::magnitudesDb (sig.data(), bins);
        size_t best = 0;
        for (size_t i = 1; i < bins.size(); ++i) if (bins[i] > bins[best]) best = i;
        expect (best == 170 && std::abs (bins[best] + 6.02f) < 0.15f, "spettro: sinusoide a -6 dBFS = bin " + juce::String ((int) best) + ", " + juce::String (bins[best], 2) + " dB");
        expect (bins[60] < -70.0f && bins[1000] < -70.0f, "spettro: lontano dal picco il livello e' basso (" + juce::String (bins[60], 1) + " / " + juce::String (bins[1000], 1) + " dB)");

        std::fill (sig.begin(), sig.end(), 0.0f);
        fc::SpectrumView::magnitudesDb (sig.data(), bins);
        expect (bins[170] <= -89.0f, "spettro: il silenzio sta sul fondo scala");
    }

    void stepAux()
    {
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "impulso";
        asset->audio.setSize (2, 6000);
        asset->audio.clear();
        asset->audio.setSample (0, 1000, 1.0f);
        asset->audio.setSample (1, 1000, 1.0f);
        asset->buildPeaks();

        fc::Project p (formats, 48000.0);
        auto& t = p.edit().tracks;
        fc::Clip c; c.id = 6000; c.asset = asset; c.start = 0; c.length = 6000;
        t[0].clips = { c };
        p.edit().master->limiterOn = false;

        auto render = [&] (bool pdc)
        {
            auto s = p.getSession().cloneDetached();
            fc::Mixer m;
            m.setPdcEnabled (pdc);
            juce::AudioBuffer<float> buf (2, 2048);
            std::vector<float> out;
            for (int pos = 0; pos < 8192; pos += 2048)
            {
                m.render (s, pos, buf, 0, 2048);
                for (int i = 0; i < 2048; ++i) out.push_back (buf.getSample (0, i));
            }
            return out;
        };

        // 1. senza send: solo il segnale secco
        auto dry = render (true);
        expect (std::abs (dry[1000] - 1.0f) < 1.0e-5f, "aux: senza send passa solo il secco");

        // polarita': la traccia invertita cancella la copia non invertita
        t[0].live->invert = true;
        auto inverted = render (true);
        expect (std::abs (inverted[1000] + 1.0f) < 1.0e-5f, "polarita': traccia invertita = " + juce::String (inverted[1000], 3));
        t[0].live->invert = false;

        // 2. send A a 0 dB senza plug-in sul ritorno: l'impulso raddoppia (secco + ritorno)
        t[0].live->sendDb[0] = 0.0f;
        auto both = render (true);
        expect (std::abs (both[1000] - 2.0f) < 1.0e-4f, "aux: send a 0 dB, secco + ritorno = " + juce::String (both[1000], 3));

        // 3. send a -6 dB e fader del ritorno a -6 dB: 1 + 0.25
        t[0].live->sendDb[0] = -6.0f;
        p.getSession().aux[0]->volumeDb = -6.0f;
        auto lower = render (true);
        const float g6 = juce::Decibels::decibelsToGain (-6.0f);
        expect (std::abs (lower[1000] - (1.0f + g6 * g6)) < 1.0e-3f, "aux: send e fader del ritorno -6 dB = " + juce::String (lower[1000], 3));

        // 4. mute del ritorno
        p.getSession().aux[0]->mute = true;
        auto muted = render (true);
        expect (std::abs (muted[1000] - 1.0f) < 1.0e-4f, "aux: ritorno in mute");
        p.getSession().aux[0]->mute = false;

        // 5. plug-in con 480 campioni di latenza sul ritorno: secco e ritorno restano allineati
        p.getSession().aux[0]->volumeDb = 0.0f;
        t[0].live->sendDb[0] = 0.0f;
        auto slot = std::make_shared<fc::InsertSlot>();
        slot->name = "DelayTest";
        slot->plugin = std::make_unique<DelayTestProcessor> (480);
        slot->plugin->setRateAndBufferSizeDetails (48000.0, 2048);
        slot->plugin->prepareToPlay (48000.0, 2048);
        slot->ready = true;
        auto chain = std::make_shared<fc::InsertChain>();
        chain->slots.push_back (slot);
        p.getSession().aux[0]->setChain (chain);
        slot->plugin->reset();
        auto aligned = render (true);
        expect (fc::Mixer::computeLatency (p.getSession()) == 480, "aux: latenza del ritorno inclusa nel PDC (480 campioni)");
        expect (std::abs (aligned[1480] - 2.0f) < 1.0e-4f && std::abs (aligned[1000]) < 1.0e-5f, "aux: secco e ritorno allineati a 1480 (picco " + juce::String (aligned[1480], 3) + ")");
        slot->plugin->reset();
        auto unaligned = render (false);
        expect (std::abs (unaligned[1000] - 1.0f) < 1.0e-4f && std::abs (unaligned[1480] - 1.0f) < 1.0e-4f, "aux: con PDC spento secco e ritorno sono sfasati");
    }

    void stepPdc()
    {
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "impulso";
        asset->audio.setSize (2, 6000);
        asset->audio.clear();
        asset->audio.setSample (0, 1000, 1.0f);
        asset->audio.setSample (1, 1000, 1.0f);
        asset->buildPeaks();

        fc::Project p (formats, 48000.0);
        auto& t = p.edit().tracks;
        fc::Clip c; c.id = 5000; c.asset = asset; c.start = 0; c.length = 6000; c.gainDb = -12.0f;
        t[0].clips = { c };
        c.id = 5001;
        t[1].clips = { c };
        p.edit().master->limiterOn = false;

        auto slot = std::make_shared<fc::InsertSlot>();
        slot->name = "DelayTest";
        slot->plugin = std::make_unique<DelayTestProcessor> (480);
        slot->plugin->setRateAndBufferSizeDetails (48000.0, 2048);
        slot->plugin->prepareToPlay (48000.0, 2048);
        slot->ready = true;
        auto chain = std::make_shared<fc::InsertChain>();
        chain->slots.push_back (slot);
        t[0].live->setChain (chain);

        expect (fc::Mixer::computeLatency (p.getSession()) == 480, "PDC: latenza della sessione rilevata (480 campioni)");

        auto renderAll = [&] (bool pdc)
        {
            slot->plugin->reset();
            auto s = p.getSession().cloneDetached();
            fc::Mixer m;
            m.setPdcEnabled (pdc);
            juce::AudioBuffer<float> buf (2, 2048);
            std::vector<float> out;
            for (int pos = 0; pos < 8192; pos += 2048)
            {
                m.render (s, pos, buf, 0, 2048);
                for (int i = 0; i < 2048; ++i) out.push_back (buf.getSample (0, i));
            }
            return out;
        };

        const auto on = renderAll (true), off = renderAll (false);
        const float amp = 2.0f * juce::Decibels::decibelsToGain (-12.0f);
        expect (std::abs (on[1480] - amp) < 1.0e-4f && std::abs (on[1000]) < 1.0e-6f && std::abs (on[1481]) < 1.0e-6f,
                "PDC acceso: le due tracce arrivano allineate (picco " + juce::String (on[1480], 3) + " a 1480)");
        const float single = juce::Decibels::decibelsToGain (-12.0f);
        expect (std::abs (off[1000] - single) < 1.0e-4f && std::abs (off[1480] - single) < 1.0e-4f,
                "PDC spento: le tracce sono sfasate di 480 campioni");

        // esportazione: il file e' allineato alla timeline (il ritardo e' compensato)
        slot->plugin->reset();
        auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_pdc.wav");
        juce::String msg;
        const bool ok = fc::renderMixdown (p.getSession(), 48000.0, wav, [] (double) { return true; }, msg);
        std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (wav));
        int peakAt = -1;
        float peakV = 0.0f;
        if (ok && rd != nullptr)
        {
            juce::AudioBuffer<float> b (2, 8000);
            rd->read (&b, 0, 8000, 0, true, true);
            for (int i = 0; i < 8000; ++i) if (std::abs (b.getSample (0, i)) > peakV) { peakV = std::abs (b.getSample (0, i)); peakAt = i; }
        }
        expect (ok && peakAt == 1000 && std::abs (peakV - amp) < 0.01f, "export: impulso al campione " + juce::String (peakAt) + " (atteso 1000)");
    }

    // sessioni affidabili: media mancanti non distruttivi, ricollegamento, versioni, controllo, flusso
    void stepReliability (std::function<void()> next)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("arotron_reliab");
        dir.deleteRecursively();
        dir.createDirectory();
        const auto wav = dir.getChildFile ("voce.wav");
        {
            juce::WavAudioFormat wf;
            std::unique_ptr<juce::FileOutputStream> os (wav.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (wf.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0));
            if (w != nullptr) os.release();
            juce::AudioBuffer<float> b (1, 48000);
            for (int i = 0; i < 48000; ++i) b.setSample (0, i, 0.5f * std::sin (0.06f * (float) i));
            if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, 48000);
        }
        juce::String err;
        auto asset = fc::loadAsset (formats, wav, 48000.0, err);
        expect (asset != nullptr, "media mancanti: file di prova creato " + err);
        if (asset == nullptr) { next(); return; }

        fc::Project p (formats, 48000.0);
        fc::dub::applyTemplate (p, fc::dub::templates()[0]);
        {
            fc::Clip c; c.id = 1; c.asset = asset; c.start = 24000; c.offset = 4800; c.length = 30000; c.gainDb = -3.0f;
            p.edit().tracks[3].clips = { c };
        }
        p.setTrackGroup (p.getSession().tracks[1].id, p.addGroup ("Dialoghi"));
        p.setGroupCollapsed (p.getSession().groups[0].id, true);
        const auto sess = dir.getChildFile ("sessione.adb");
        expect (p.save (sess, err), "media mancanti: sessione salvata " + err);

        // il file sparisce
        const auto parked = dir.getChildFile ("altrove").getChildFile ("voce.wav");
        parked.getParentDirectory().createDirectory();
        wav.moveFileTo (parked);

        project2 = std::make_unique<fc::Project> (formats, 48000.0);
        project2->loadSession (sess, [this, next, dir, sess, parked, wav] (juce::String e)
        {
            auto& s2 = project2->getSession();
            const fc::Clip* c = s2.tracks.size() == 4 && ! s2.tracks[3].clips.empty() ? &s2.tracks[3].clips[0] : nullptr;
            expect (c != nullptr && c->asset->offline && c->start == 24000 && c->offset == 4800 && c->length == 30000 && std::abs (c->gainDb + 3.0f) < 1.0e-4f,
                    "media mancanti: il clip resta nella sessione (offline) con posizione, offset, lunghezza e guadagno");
            expect (e.contains ("File non trovato") && project2->hasOfflineMedia(), "media mancanti: segnalati all'apertura");
            expect (s2.groups.size() == 1 && s2.groups[0].name == "Dialoghi" && s2.groups[0].collapsed && s2.tracks[1].groupId == s2.groups[0].id && s2.tracks[0].groupId == 0,
                    "Gruppi: nome, compresso e appartenenza si ritrovano dopo il salvataggio");

            fc::Mixer m;
            juce::AudioBuffer<float> buf (2, 512);
            m.render (s2, 24000, buf, 0, 512);
            expect (buf.getMagnitude (0, 512) == 0.0f, "media mancanti: il clip offline non suona (e non manda in crash il mixer)");

            // risalvando il percorso non si perde
            const auto again = dir.getChildFile ("risalvata.adb");
            juce::String er;
            project2->saveCopy (again, er);
            expect (juce::JSON::parse (again).toString().isEmpty() == false || again.loadFileAsString().contains ("voce.wav"), "media mancanti: il percorso del file resta nella sessione risalvata");

            // controllo sessione
            auto issues = fc::check::checkSession (*project2);
            bool hasMissing = false;
            for (auto& i : issues) hasMissing = hasMissing || (i.level == fc::check::Issue::Error && i.text.contains ("File mancante"));
            expect (hasMissing && issues.front().level == fc::check::Issue::Error, "controllo sessione: il file mancante e' un errore, in cima all'elenco");

            // ricollegamento da una cartella
            const auto found = fc::relink::search (formats, s2, dir, 48000.0, {}, [] (double) { return true; });
            expect (found.found == 1 && found.notFound.isEmpty() && found.replacements.size() == 1, "ricollegamento: file trovato nelle sottocartelle (" + juce::String (found.found) + ")");
            project2->replaceAssets (found.replacements);
            auto& s3 = project2->getSession();
            expect (! project2->hasOfflineMedia() && s3.tracks[3].clips[0].start == 24000 && ! s3.tracks[3].clips[0].asset->offline, "ricollegamento: il clip torna con audio, stessa posizione");
            m.render (s3, 24000, buf, 0, 512);
            expect (buf.getMagnitude (0, 512) > 0.1f, "ricollegamento: il clip suona di nuovo");
            project2->undo();
            expect (project2->hasOfflineMedia(), "ricollegamento: un solo undo torna offline");

            // un file troppo corto viene rifiutato
            {
                const auto shortDir = dir.getChildFile ("corti");
                shortDir.createDirectory();
                juce::WavAudioFormat wf;
                std::unique_ptr<juce::FileOutputStream> os (shortDir.getChildFile ("voce.wav").createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w (wf.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0));
                if (w != nullptr) { os.release(); juce::AudioBuffer<float> b (1, 1000); b.clear(); w->writeFromAudioSampleBuffer (b, 0, 1000); }
                w.reset();
                const auto sh = fc::relink::search (formats, project2->getSession(), shortDir, 48000.0, {}, [] (double) { return true; });
                expect (sh.found == 0 && sh.tooShort.size() == 1, "ricollegamento: un file piu' corto di quanto servono i clip e' rifiutato");
            }

            // versioni
            fc::Project vp (formats, 48000.0);
            vp.setFile (sess);
            juce::String ve;
            const auto v1 = fc::versions::save (vp, "prima del montaggio", ve);
            const auto v2 = fc::versions::save (vp, "", ve);
            const auto list = fc::versions::list (sess);
            expect (v1.existsAsFile() && v2.existsAsFile() && list.size() == 2 && list[0].number == 2 && list[1].number == 1 && list[1].note == "prima del montaggio" && list[0].note.isEmpty(),
                    "versioni: due copie numerate (" + v1.getFileName() + "), la piu' recente per prima, con la nota");
            expect (fc::versions::remove (list[1]) && fc::versions::list (sess).size() == 1 && ! v1.withFileExtension ("txt").existsAsFile(), "versioni: eliminazione della versione e della sua nota");
            fc::Project unsaved (formats, 48000.0);
            juce::String ue;
            expect (fc::versions::save (unsaved, "", ue) == juce::File() && ue.isNotEmpty(), "versioni: senza sessione salvata spiega cosa fare");

            // controllo sessione su una sessione costruita ad hoc
            fc::Project cp (formats, 48000.0);
            fc::dub::applyTemplate (cp, fc::dub::templates()[0]);
            cp.edit().tracks[3].clips = { fc::Clip() };
            cp.edit().tracks[3].clips[0].asset = std::make_shared<fc::Asset>();
            cp.edit().tracks[3].clips[0].asset->audio.setSize (1, 100);
            cp.edit().tracks[3].clips[0].length = 100;
            cp.edit().tracks[2].live->trackType = fc::TrackNone;
            cp.edit().tracks[2].clips = cp.edit().tracks[3].clips;
            std::vector<fc::AdrCue> cues (3);
            cues[0].id = 1; cues[0].number = 1; cues[0].status = 0;
            cues[1].id = 2; cues[1].number = 2; cues[1].status = 3;
            cues[2].id = 3; cues[2].number = 3; cues[2].status = 2;
            cp.addCues (cues, true);
            const auto iss = fc::check::checkSession (cp);
            int warn = 0; bool noRole = false, toRecord = false, redo = false;
            for (auto& i : iss) { warn += i.level == fc::check::Issue::Warning; noRole = noRole || i.text.contains ("nessun ruolo"); toRecord = toRecord || i.text.contains ("da registrare"); redo = redo || i.text.contains ("da rifare"); }
            expect (noRole && toRecord && redo && warn >= 3, "controllo sessione: traccia senza ruolo, cue da registrare e da rifare (" + juce::String (warn) + " avvisi)");
            expect (fc::check::checkSession (fc::Project (formats, 48000.0)).front().text.contains ("vuota"), "controllo sessione: sessione vuota segnalata");

            // flusso di lavoro
            const auto steps = fc::workflow::steps (cp, false, false);
            expect (steps.size() == 7 && ! steps[0].done && steps[1].done && ! steps[3].done && ! steps[4].done && ! steps[5].done && ! steps[6].done, "flusso: video mancante e copione presente, il resto da fare");
            const auto stepsDone = fc::workflow::steps (cp, true, true);
            expect (stepsDone[5].done && stepsDone[6].done, "flusso: controllo e consegna spuntati quando fatti");

            dir.deleteRecursively();
            next();
        });
    }

    // ReConform: nuovo montaggio da EDL vecchio e nuovo
    void stepConform()
    {
        namespace cf = fc::conform;
        const juce::String oldEdl =
            "TITLE: VECCHIO\nFCM: NON-DROP FRAME\n\n"
            "001  R1  V  C        01:00:00:00 01:00:10:00 01:00:00:00 01:00:10:00\n"
            "002  R2  V  C        02:00:00:00 02:00:10:00 01:00:10:00 01:00:20:00\n"
            "* FROM CLIP NAME: SCENA DUE\n"
            "003  R3  V  C        03:00:00:00 03:00:10:00 01:00:20:00 01:00:30:00\n"
            "004  AX  A  C        00:00:00:00 00:00:10:00 01:00:00:00 01:00:10:00\n";
        const juce::String newEdl =
            "TITLE: NUOVO\nFCM: NON-DROP FRAME\n\n"
            "001  R3  V  C        03:00:00:00 03:00:10:00 01:00:00:00 01:00:10:00\n"
            "002  R1  V  C        01:00:02:00 01:00:10:00 01:00:10:00 01:00:18:00\n"
            "003  R9  V  C        09:00:00:00 09:00:03:00 01:00:18:00 01:00:21:00\n"
            "004  R2  V  C        02:00:00:00 02:00:10:00 01:00:21:00 01:00:31:00\n"
            "* FROM CLIP NAME: SCENA DUE\n";
        const auto po = cf::parseEdl (oldEdl, 2), pn = cf::parseEdl (newEdl, 2);
        expect (po.events.size() == 3 && pn.events.size() == 4 && po.events[1].clipName == "SCENA DUE" && std::abs (po.events[1].srcIn - 7200.0) < 1.0e-6,
                "ReConform: eventi video letti (3 e 4), l'evento solo audio e' ignorato, nome del clip letto");
        const auto map = cf::buildMap (po.events, pn.events);
        expect (map.segments.size() == 3 && map.removed.size() == 1 && map.inserted.size() == 1
                    && std::abs (map.removed[0].first - 3600.0) < 1.0e-6 && std::abs (map.removed[0].second - 3602.0) < 1.0e-6
                    && std::abs (map.inserted[0].first - 3618.0) < 1.0e-6 && std::abs (map.inserted[0].second - 3621.0) < 1.0e-6,
                "ReConform: mappa con 3 pezzi, 2 s tolti all'inizio di R1 e 3 s di materiale nuovo (" + juce::String ((int) map.segments.size()) + " pezzi)");

        fc::Project p (formats, 48000.0);
        p.tcFormat = 2;            // 25 fps
        p.tcStartSec = 3600.0;
        fc::dub::applyTemplate (p, fc::dub::templates()[2]);   // Dialoghi, M&E, Effetti
        auto asset = std::make_shared<fc::Asset>();
        asset->name = "lungo";
        asset->audio.setSize (1, 30 * 48000);
        for (int i = 0; i < 30 * 48000; ++i) asset->audio.setSample (0, i, (float) i / 1.0e6f);   // rampa: si riconosce la posizione nel file
        asset->buildPeaks();
        {
            fc::Clip c; c.id = 1; c.asset = asset; c.start = 5 * 48000; c.offset = 5 * 48000; c.length = 20 * 48000; c.fadeIn = 480;
            p.edit().tracks[0].clips = { c };
            fc::Clip tail; tail.id = 2; tail.asset = asset; tail.start = 31 * 48000; tail.offset = 0; tail.length = 48000;   // dopo la fine del vecchio montaggio: non si muove
            p.edit().tracks[1].clips = { tail };
        }
        std::vector<fc::AdrCue> cues (3);
        cues[0].id = 1; cues[0].number = 1; cues[0].inPos = 1 * 48000; cues[0].outPos = 2 * 48000;     // nel pezzo di R1 che viene tolto
        cues[1].id = 2; cues[1].number = 2; cues[1].inPos = 12 * 48000; cues[1].outPos = 14 * 48000;   // dentro R2
        cues[2].id = 3; cues[2].number = 3; cues[2].inPos = 25 * 48000; cues[2].outPos = 27 * 48000;   // dentro R3
        p.addCues (cues, true);
        p.addMarker (15 * 48000, "scena");

        const auto plan = cf::makePlan (map, p.tcStartSec, 48000.0, true);
        const auto before = p.getSession().tracks[0].clips[0].start;
        const auto dry = p.conform (plan, false);
        expect (p.getSession().tracks[0].clips.size() == 1 && p.getSession().tracks[0].clips[0].start == before && dry.clipPieces == 3 && dry.cuesOut == 1,
                "ReConform: l'anteprima calcola il rapporto senza cambiare nulla");
        const auto st = p.conform (plan, true);
        auto clips = p.getSession().tracks[0].clips;
        std::sort (clips.begin(), clips.end(), [] (const fc::Clip& a, const fc::Clip& b) { return a.start < b.start; });
        expect (clips.size() == 3 && clips[0].start == 0 && clips[0].length == 5 * 48000 && clips[0].offset == 20 * 48000
                    && clips[1].start == 13 * 48000 && clips[1].length == 5 * 48000 && clips[1].offset == 5 * 48000
                    && clips[2].start == 21 * 48000 && clips[2].length == 10 * 48000 && clips[2].offset == 10 * 48000,
                "ReConform: il clip e' diviso in 3 pezzi nelle nuove posizioni (R3 in testa, R1 a 13 s, R2 a 21 s) con gli offset giusti");
        expect (clips[0].fadeIn >= 200 && clips[0].fadeOut == 0 && clips[1].fadeIn == 480 && clips[1].fadeOut >= 200 && clips[2].fadeIn >= 200, "ReConform: breve dissolvenza sui nuovi tagli, il bordo esterno mantiene la sua");
        expect (p.getSession().tracks[1].clips[0].start == 31 * 48000, "ReConform: l'audio fuori dal vecchio montaggio resta dov'era");
        const auto& cs = p.getSession().cues;
        expect (cs[0].status == 3 && cs[0].notes.contains ("Fuori dal nuovo montaggio") && cs[0].inPos == 48000, "ReConform: cue nel materiale tolto segnato Da rifare, non spostato");
        expect (cs[1].inPos == 23 * 48000 && cs[1].outPos == 25 * 48000 && cs[2].inPos == 5 * 48000 && cs[2].outPos == 7 * 48000, "ReConform: gli altri cue seguono il loro materiale (R2 +11 s, R3 -20 s)");
        bool sceneMoved = false, insertedMarker = false;
        for (auto& m : p.getSession().markers) { sceneMoved = sceneMoved || (m.name == "scena" && m.pos == 26 * 48000); insertedMarker = insertedMarker || (m.name.startsWith ("Inserito 1") && m.pos == 18 * 48000); }
        expect (sceneMoved && insertedMarker && st.insertedMarkers == 1 && st.cuesMoved == 2 && st.cuesOut == 1, "ReConform: marker spostato (+11 s) e marker sul materiale nuovo a 18 s");
        p.undo();
        expect (p.getSession().tracks[0].clips.size() == 1 && p.getSession().cues[0].status == 0 && p.getSession().cues[1].inPos == 12 * 48000, "ReConform: un solo undo ripristina clip, cue e marker");
        expect (cf::describe (map, st, 48000.0).contains ("3 pezzi in comune"), "ReConform: il rapporto descrive la modifica");

        // fotogrammi incompatibili e cambi di velocita'
        const auto bad = cf::parseEdl ("001  R1  V  C        01:00:00:29 01:00:10:00 01:00:00:00 01:00:10:00\n002  R1  V  C        01:00:10:00 01:00:20:00 01:00:10:00 01:00:20:00\nM2   R1  050.0  01:00:10:00\n", 2);
        bool warnFrames = false, warnRetime = false;
        for (auto& w : bad.warnings) { warnFrames = warnFrames || w.contains ("timecode non valido"); warnRetime = warnRetime || w.contains ("M2"); }
        expect (bad.events.size() == 1 && warnFrames && warnRetime && bad.events[0].retimed, "ReConform: timecode a 29 fotogrammi a 25 fps saltato con avviso, evento M2 riconosciuto");
        const auto dis = cf::parseEdl ("001  R1  V  D  030  01:00:00:00 01:00:10:00 01:00:00:00 01:00:10:00\n", 2);
        expect (! dis.events.empty() && dis.warnings.joinIntoString (" ").contains ("dissolvenze"), "ReConform: dissolvenze segnalate e trattate come tagli");
        const auto none = cf::buildMap (po.events, cf::parseEdl ("001  ZZ  V  C        01:00:00:00 01:00:05:00 01:00:00:00 01:00:05:00\n", 2).events);
        expect (none.segments.empty() && ! none.warnings.isEmpty(), "ReConform: nessun materiale in comune segnalato");
        const auto dup = cf::buildMap (po.events, cf::parseEdl ("001  R1  V  C        01:00:00:00 01:00:05:00 01:00:00:00 01:00:05:00\n002  R1  V  C        01:00:00:00 01:00:05:00 01:00:05:00 01:00:10:00\n", 2).events);
        bool warnDup = false;
        for (auto& w : dup.warnings) warnDup = warnDup || w.contains ("piu' volte");
        expect (warnDup && dup.segments.size() == 1, "ReConform: materiale usato due volte, si segue la prima occorrenza");
    }

    // gruppi di tracce (cartelle)
    void stepTrackFolders()
    {
        fc::Project p (formats, 48000.0);
        for (int i = 0; i < 4; ++i) p.addTrack();
        auto id = [&p] (int i) { return p.getSession().tracks[(size_t) i].id; };
        auto grp = [&p] (int i) { return p.getSession().tracks[(size_t) i].groupId; };
        const int t0 = id (0), t1 = id (1), t2 = id (2), t3 = id (3);

        const int g = p.addGroup ("Dialoghi");
        expect (g > 0 && p.addGroup ("dialoghi") == g && p.getSession().groups.size() == 1, "Gruppi: stesso nome = stesso gruppo");
        p.setTrackGroup (t3, g);
        p.setTrackGroup (t1, g);
        expect (p.getSession().tracks[2].id == t3 && p.getSession().tracks[3].id == t1 && grp (2) == g && grp (3) == g && grp (0) == 0,
                "Gruppi: una traccia entra nel gruppo accanto alle altre");

        p.moveTrackTo (3, 2);   // t1 e t3 si scambiano: restano nel gruppo
        expect (grp (2) == g && grp (3) == g, "Gruppi: spostare dentro il gruppo lo mantiene");
        p.moveTrackTo (0, 3);   // t0 (senza gruppo) in mezzo/fine: prende il gruppo se i vicini coincidono
        expect (p.getSession().tracks[3].id == t0, "Gruppi: sposta la traccia");
        p.moveTrackTo (3, 0);
        expect (grp (0) == 0, "Gruppi: fuori dal gruppo resta senza gruppo");

        p.setGroupCollapsed (g, true);
        expect (p.findGroup (g)->collapsed, "Gruppi: compresso");
        p.setGroupMuteSolo (g, false, true);
        int muted = 0;
        for (auto& t : p.getSession().tracks) muted += t.live->mute.load() ? 1 : 0;
        expect (muted == 2, "Gruppi: muto del gruppo = solo le sue tracce (" + juce::String (muted) + ")");

        // selezione di piu' tracce e gruppo con un solo passo di undo
        {
            fc::Project m (formats, 48000.0);
            for (int i = 0; i < 5; ++i) m.addTrack();
            const auto ids = [&m] { std::vector<int> v; for (auto& t : m.getSession().tracks) v.push_back (t.id); return v; }();
            m.selectOnlyTrack (ids[0]);
            m.toggleTrackSelection (ids[2]);
            m.toggleTrackSelection (ids[4]);
            expect (m.selectedTracks() == std::vector<int> { ids[0], ids[2], ids[4] }, "Selezione tracce: Cmd+clic ne aggiunge");
            m.toggleTrackSelection (ids[2]);
            expect (m.selectedTracks() == std::vector<int> { ids[0], ids[4] } && ! m.isTrackSelected (ids[2]), "Selezione tracce: Cmd+clic su una selezionata la toglie");
            m.selectOnlyTrack (ids[1]);
            m.selectTrackRange (ids[3]);
            expect (m.selectedTracks() == std::vector<int> { ids[1], ids[2], ids[3] }, "Selezione tracce: Maiusc+clic seleziona l'intervallo");
            m.selectedTrackId = ids[0];   // un'altra traccia scelta da fuori: la selezione multipla decade
            expect (m.selectedTracks() == std::vector<int> { ids[0] }, "Selezione tracce: cambiando traccia principale resta quella sola");

            const int gm = m.addGroup ("Dialoghi");
            m.setGroupCollapsed (gm, true);
            m.setTracksGroup ({ ids[4], ids[1], ids[3] }, gm);
            int members = 0;
            for (auto& t : m.getSession().tracks) members += t.groupId == gm ? 1 : 0;
            const auto& tr = m.getSession().tracks;
            expect (members == 3 && tr[0].id == ids[0] && tr[1].id == ids[1] && tr[2].id == ids[3] && tr[3].id == ids[4] && tr[4].id == ids[2] && ! m.findGroup (gm)->collapsed,
                    "Gruppo con tracce selezionate: tutte dentro, accanto tra loro, gruppo aperto");
            m.undo();
            int after = 0;
            for (auto& t : m.getSession().tracks) after += t.groupId != 0 ? 1 : 0;
            expect (after == 0 && m.getSession().tracks[1].id == ids[1], "Gruppo con tracce selezionate: un solo passo di undo le riporta com'erano");
        }

        // salvataggio e ricaricamento
        const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_gruppi.adb");
        juce::String err;
        expect (p.save (f, err), "Gruppi: sessione salvata " + err);
        expect (f.loadFileAsString().contains ("\"collapsed\"") && f.loadFileAsString().contains ("Dialoghi"), "Gruppi: il file della sessione contiene i gruppi");
        f.deleteFile();

        p.removeGroup (g);
        expect (p.getSession().groups.empty() && grp (2) == 0 && grp (3) == 0, "Gruppi: sciogliere il gruppo lascia le tracce");
        p.undo();
        expect (p.getSession().groups.size() == 1 && grp (2) == g, "Gruppi: undo dello scioglimento");
        (void) t2;
    }

    // sincronia esterna MTC e scorciatoie
    void stepLang()
    {
        namespace lg = fc::lang;
        const auto en = lg::catalogText ("en");
        expect (en.isNotEmpty() && lg::catalogText ("it").isEmpty(), "Lingua: catalogo inglese incorporato, italiano = testi nel codice");
        expect (lg::translateTo ("en", "Annulla") != "Annulla" || lg::translateTo ("en", "Annulla") == "Cancel", "Lingua: una voce comune e' tradotta");
        expect (lg::translateTo ("en", "Testo che non esiste nel catalogo xyz") == "Testo che non esiste nel catalogo xyz", "Lingua: senza traduzione resta l'italiano");
        expect (lg::translateTo ("it", "Salva") == "Salva", "Lingua: l'italiano non cambia nulla");
        expect (lg::translateTo ("en", "Nessun cue").isNotEmpty(), "Lingua: chiave non vuota");

        // voci con a capo e virgolette (escape del catalogo) e nessuna chiave duplicata
        juce::StringArray lines; lines.addLines (en);
        juce::StringArray keys;
        int dup = 0, entries = 0;
        for (auto& l : lines)
        {
            auto t = l.trim();
            if (! t.startsWithChar ('"')) continue;
            ++entries;
            auto key = t.fromFirstOccurrenceOf ("\"", false, false).upToFirstOccurrenceOf ("\" = \"", false, false);
            if (keys.contains (key)) ++dup;
            keys.add (key);
        }
        expect (entries > 500 && dup == 0, "Lingua: " + juce::String (entries) + " voci, " + juce::String (dup) + " duplicate");
        const auto nl = lg::translateTo ("en", "Non ho trovato tagli. Prova ad aumentare la sensibilita'.");
        expect (nl.startsWith ("I found no cuts"), "Lingua: voce con apostrofo tradotta");
        const auto multi = lg::translateTo ("en", "Vuoi salvare le modifiche alla sessione?");
        expect (multi.startsWith ("Do you want"), "Lingua: domanda di salvataggio tradotta");
        const auto lf = lg::translateTo ("en", "SCORCIATOIE DA TASTIERA\n\n");
        expect (lf == "KEYBOARD SHORTCUTS\n\n", "Lingua: a capo nel testo chiave e traduzione");
    }

    void stepMtc()
    {
        namespace mt = fc::mtc;
        auto qf = [] (int piece, int value) { return juce::MidiMessage::quarterFrame (piece, value); };
        auto feedTc = [&] (mt::Decoder& d, int h, int m, int s, int f, int rateCode, double startMs)
        {
            const int vals[8] = { f & 15, f >> 4, s & 15, s >> 4, m & 15, m >> 4, h & 15, ((h >> 4) & 1) | (rateCode << 1) };
            for (int i = 0; i < 8; ++i) d.feed (qf (i, vals[i]), startMs + 10.0 * i);
        };

        mt::Decoder d;
        feedTc (d, 1, 2, 3, 4, 1, 0.0);   // 01:02:03:04 a 25 fps, un quarter-frame ogni 10 ms
        auto st = d.state (70.0);
        expect (st.valid && st.running && st.text == "01:02:03:06" && st.fmtIndex == 2, "MTC: 8 quarter-frame danno 01:02:03:04 + 2 fotogrammi = " + st.text);
        st = d.state (110.0);
        expect (std::abs (st.seconds - (3723.0 + 6.0 / 25.0 + 0.040)) < 1.0e-6 && st.running, "MTC: fra un messaggio e l'altro il timecode avanza con l'orologio");
        expect (! d.state (400.0).running && d.state (400.0).valid, "MTC: senza quarter-frame per 150 ms il timecode e' fermo");

        mt::Decoder d2;   // 30 fps, ore a 10+: il bit alto dell'ora e il codice di frequenza
        feedTc (d2, 17, 59, 58, 28, 3, 0.0);
        st = d2.state (70.0);
        expect (st.text == "17:59:59:00" && st.fmtIndex == 5, "MTC: 17:59:58:28 a 30 fps (+2 fotogrammi) = " + st.text);

        mt::Decoder d3;   // un buco nella sequenza: si riparte dal primo quarter-frame
        d3.feed (qf (0, 4), 0.0); d3.feed (qf (1, 0), 10.0); d3.feed (qf (4, 2), 20.0);   // manca il 2, 3
        feedTc (d3, 0, 0, 10, 0, 1, 30.0);
        expect (d3.state (100.0).text == "00:00:10:02", "MTC: dopo un buco ricomincia dal quarter-frame 0 (" + d3.state (100.0).text + ")");

        mt::Decoder d4;   // full-frame SysEx a 29,97 drop: F0 7F 7F 01 01 hh mm ss ff F7
        const juce::uint8 sx[] = { 0xF0, 0x7F, 0x7F, 0x01, 0x01, (juce::uint8) ((2 << 5) | 1), 10, 0, 2, 0xF7 };
        d4.feed (juce::MidiMessage (sx, (int) sizeof (sx)), 0.0);
        st = d4.state (10.0);
        expect (st.valid && ! st.running && st.fmtIndex == 4 && st.text.startsWith ("01:10:00"), "MTC: messaggio full-frame a 29,97 drop letto (" + st.text + ")");

        // decisioni
        mt::Memory mem;
        mt::Decoder run;
        feedTc (run, 1, 0, 10, 0, 1, 0.0);                       // 01:00:10:00 (+2 fotogrammi) a 25 fps
        const auto rs = run.state (70.0);
        const double target = 3610.0 + 2.0 / 25.0;
        auto dec = mt::decide (rs, 3600.0, 0.0, 48000.0, false, 0, mem);
        expect (dec.action == mt::Action::LocateAndPlay && std::abs ((double) dec.position - target * 48000.0 + 3600.0 * 48000.0) < 1.0, "MTC: al via si posiziona e parte (" + juce::String ((juce::int64) dec.position) + ")");
        dec = mt::decide (rs, 3600.0, 0.0, 48000.0, true, dec.position + 100, mem);
        expect (dec.action == mt::Action::None, "MTC: dentro la tolleranza non fa nulla");
        const int64_t far = dec.position;
        dec = mt::decide (rs, 3600.0, 0.0, 48000.0, true, far + 48000 / 5, mem);
        const auto first = dec.action;
        dec = mt::decide (rs, 3600.0, 0.0, 48000.0, true, far + 48000 / 5, mem);
        expect (first == mt::Action::None && dec.action == mt::Action::Locate, "MTC: la deriva oltre 2 fotogrammi riposiziona solo al secondo controllo di fila");
        dec = mt::decide (rs, 3600.0, 0.5, 48000.0, false, 0, mem);
        expect (std::abs ((double) dec.position - (target - 3600.0 + 0.5) * 48000.0) < 1.0, "MTC: lo spostamento si somma alla posizione");
        const auto stopped = run.state (1000.0);
        dec = mt::decide (stopped, 3600.0, 0.0, 48000.0, true, 1000, mem);
        expect (dec.action == mt::Action::Stop, "MTC: se il timecode si ferma, il trasporto che era partito da MTC si ferma");
        mt::Memory manual;
        expect (mt::decide (stopped, 3600.0, 0.0, 48000.0, true, 1000, manual).action == mt::Action::None, "MTC: un trasporto avviato a mano non viene fermato dall'assenza di MTC");

        // scorciatoie: salvataggio e ripristino della mappa dei tasti
        struct Dummy : juce::ApplicationCommandTarget
        {
            ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
            void getAllCommands (juce::Array<juce::CommandID>& c) override { c.add (1); c.add (2); }
            void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& i) override
            {
                i.setInfo (id == 1 ? "Uno" : "Due", "", "Prova", 0);
                if (id == 1) i.addDefaultKeypress ('a', juce::ModifierKeys::commandModifier);
            }
            bool perform (const InvocationInfo&) override { return true; }
        } keyTarget;
        juce::ApplicationCommandManager cm1;
        cm1.registerAllCommandsForTarget (&keyTarget);
        auto* km = cm1.getKeyMappings();
        expect (km->getKeyPressesAssignedToCommand (1).size() == 1, "scorciatoie: tasto predefinito presente");
        km->clearAllKeyPresses (1);
        km->addKeyPress (2, juce::KeyPress ('k', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0));
        const auto xml = km->createXml (true);
        juce::ApplicationCommandManager cm2;
        cm2.registerAllCommandsForTarget (&keyTarget);
        if (xml != nullptr) cm2.getKeyMappings()->restoreFromXml (*xml);
        expect (xml != nullptr && cm2.getKeyMappings()->getKeyPressesAssignedToCommand (1).isEmpty() && cm2.getKeyMappings()->getKeyPressesAssignedToCommand (2).size() == 1,
                "scorciatoie: le modifiche (tasto tolto e tasto nuovo) tornano identiche dopo il salvataggio");
        cm2.getKeyMappings()->resetToDefaultMappings();
        expect (cm2.getKeyMappings()->getKeyPressesAssignedToCommand (1).size() == 1 && cm2.getKeyMappings()->getKeyPressesAssignedToCommand (2).isEmpty(), "scorciatoie: ripristino delle predefinite");
    }

    // cuffie dell'attore: mix su una seconda coppia di uscite, bip con livello proprio, talkback del direttore
    void stepBooth()
    {
        fc::Project p (formats, 48000.0);
        auto a = std::make_shared<fc::Asset>();
        a->name = "tono";
        a->audio.setSize (2, 96000);
        for (int i = 0; i < 96000; ++i) { const float v = 0.4f * std::sin (0.05f * (float) i); a->audio.setSample (0, i, v); a->audio.setSample (1, i, v); }
        a->buildPeaks();
        auto& t = p.edit().tracks[0];
        fc::Clip c; c.id = 1; c.asset = a; c.start = 0; c.length = 96000;
        t.clips = { c };
        t.live->limiterOn = false; t.live->eq.enabled = false; t.live->comp.enabled = false; t.live->volumeDb = 0.0f;
        p.edit().master->limiterOn = false;

        constexpr int block = 256;
        auto run = [&] (fc::AudioEngine& eng, int numOut, bool withInput, int blocks, std::vector<std::vector<float>>& outs, float inLevel = 0.2f)
        {
            outs.assign ((size_t) numOut, std::vector<float> ((size_t) block, 0.0f));
            std::vector<float> in ((size_t) block, inLevel);
            const float* ins[1] = { in.data() };
            for (int b = 0; b < blocks; ++b)
            {
                std::vector<float*> ptr;
                for (auto& o : outs) ptr.push_back (o.data());
                eng.audioDeviceIOCallbackWithContext (withInput ? ins : nullptr, withInput ? 1 : 0, ptr.data(), numOut, block, juce::AudioIODeviceCallbackContext());
            }
        };
        auto peakOf = [] (const std::vector<float>& v) { float m = 0.0f; for (auto x : v) m = juce::jmax (m, std::abs (x)); return m; };

        // lettore copione: quale cue mostrare, conto alla rovescia, avanzamento
        {
            std::vector<fc::AdrCue> cues (3);
            const int64_t sr48 = 48000;
            cues[0].number = 2; cues[0].inPos = 20 * sr48; cues[0].outPos = 24 * sr48;
            cues[1].number = 1; cues[1].inPos = 10 * sr48; cues[1].outPos = 14 * sr48;
            cues[2].number = 3; cues[2].inPos = 30 * sr48; cues[2].outPos = 31 * sr48;
            const auto sc = fc::script::sorted (cues);
            expect (sc[0].number == 1 && sc[1].number == 2 && sc[2].number == 3, "copione: cue ordinati per posizione");
            auto st = fc::script::stateAt (sc, 5 * sr48, 48000.0, 3.0);
            expect (st.valid && st.current == 0 && ! st.active && std::abs (st.secondsToStart - 5.0) < 1.0e-6, "copione: prima del primo cue mostra il prossimo, non ancora attivo");
            st = fc::script::stateAt (sc, 8 * sr48, 48000.0, 3.0);
            expect (st.current == 0 && st.active && std::abs (st.secondsToStart - 2.0) < 1.0e-6 && st.progress == 0.0, "copione: nei 3 s prima dell'ingresso e' attivo con il conto alla rovescia");
            st = fc::script::stateAt (sc, 12 * sr48, 48000.0, 3.0);
            expect (st.current == 0 && st.active && std::abs (st.progress - 0.5) < 1.0e-6, "copione: a meta' del cue l'avanzamento e' 50%");
            st = fc::script::stateAt (sc, 15 * sr48, 48000.0, 3.0);
            expect (st.current == 1 && ! st.active, "copione: finito un cue passa al successivo");
            st = fc::script::stateAt (sc, 40 * sr48, 48000.0, 3.0);
            expect (st.valid && st.current == 2 && st.progress == 1.0, "copione: dopo l'ultimo resta l'ultimo, concluso");
            expect (! fc::script::stateAt ({}, 0, 48000.0, 3.0).valid, "copione: nessun cue = niente da mostrare");
        }

        // 1. cuffie spente: la seconda coppia resta muta
        {
            fc::AudioEngine eng;
            eng.setNumInputChannelsForTest (1);
            eng.publish (std::make_shared<const fc::Session> (p.getSession()));
            eng.play();
            std::vector<std::vector<float>> o;
            run (eng, 4, true, 8, o);
            expect (peakOf (o[0]) > 0.3f && peakOf (o[2]) == 0.0f && peakOf (o[3]) == 0.0f, "cuffie spente: master suona (" + juce::String (peakOf (o[0]), 2) + "), uscite 3-4 mute");
        }
        // 2. cuffie accese: stessa musica sulle uscite 3-4 con il livello del mix
        {
            fc::AudioEngine eng;
            eng.setNumInputChannelsForTest (1);
            eng.publish (std::make_shared<const fc::Session> (p.getSession()));
            eng.cue.enabled = true; eng.cue.pair = 1; eng.cue.mixDb = -6.0f;
            eng.play();
            std::vector<std::vector<float>> o;
            run (eng, 4, true, 8, o);
            const float m = peakOf (o[0]), cu = peakOf (o[2]);
            expect (std::abs (cu / m - 0.5f) < 0.02f && std::abs (peakOf (o[3]) - cu) < 0.01f, "cuffie: mix a -6 dB sulle uscite 3-4 (rapporto " + juce::String (cu / m, 3) + ")");
            run (eng, 2, true, 2, o);
            expect (peakOf (o[0]) > 0.3f, "cuffie accese con una scheda a sole 2 uscite: il master non ne risente");
            // 3. talkback: il microfono del direttore va solo nelle cuffie e il mix si abbassa
            eng.cue.talkOn = true; eng.cue.talkDb = 0.0f; eng.cue.mixDuckDb = -12.0f; eng.cue.mixDb = 0.0f; eng.cue.talkInput = 0;
            run (eng, 4, true, 8, o, 0.2f);
            const float masterPk = peakOf (o[0]);
            const float expectCue = 0.4f * std::pow (10.0f, -12.0f / 20.0f) + 0.2f;   // mix abbassato + microfono
            expect (masterPk > 0.35f && masterPk < 0.45f && std::abs (peakOf (o[2]) - expectCue) < 0.05f, "talkback: nelle cuffie mix -12 dB + microfono (" + juce::String (peakOf (o[2]), 3) + ", atteso " + juce::String (expectCue, 3) + "), nel master niente (" + juce::String (masterPk, 3) + ")");
        }
        // 4. motore fermo: il talkback funziona lo stesso
        {
            fc::AudioEngine eng;
            eng.setNumInputChannelsForTest (1);
            eng.publish (std::make_shared<const fc::Session> (p.getSession()));
            eng.cue.enabled = true; eng.cue.talkOn = true; eng.cue.talkDb = -6.0f;
            std::vector<std::vector<float>> o;
            run (eng, 4, true, 4, o, 0.4f);
            expect (peakOf (o[0]) == 0.0f && std::abs (peakOf (o[2]) - 0.2f) < 0.01f, "talkback a motore fermo: solo nelle cuffie (" + juce::String (peakOf (o[2]), 3) + ")");
        }
        // 5. bip: stesso istante, livello proprio nelle cuffie
        {
            fc::Project q (formats, 48000.0);
            q.edit().master->limiterOn = false;
            fc::AudioEngine eng;
            eng.publish (std::make_shared<const fc::Session> (q.getSession()));
            eng.cue.enabled = true; eng.cue.beepDb = -20.0f;
            eng.setBeeps ({ 0 });
            eng.play();
            std::vector<std::vector<float>> o;
            float mainPk = 0.0f, cuePk = 0.0f;
            for (int b = 0; b < 12; ++b) { run (eng, 4, false, 1, o); mainPk = juce::jmax (mainPk, peakOf (o[0])); cuePk = juce::jmax (cuePk, peakOf (o[2])); }
            expect (mainPk > 0.2f && std::abs (cuePk / mainPk - 0.1f) < 0.02f, "bip: nelle cuffie a -20 dB rispetto al master (" + juce::String (cuePk / juce::jmax (0.001f, mainPk), 3) + ")");
        }
    }

    // sincronia: allineamento alla guida e ducking automatico
    void stepSync()
    {
        namespace dub = fc::dub;
        constexpr double sr = 48000.0;
        // "parlato" sintetico: rumore con un inviluppo a sillabe; due voci diverse hanno lo stesso inviluppo ma portante diversa
        auto speech = [&] (int seconds, int seed, float amp)
        {
            auto a = std::make_shared<fc::Asset>();
            a->name = "voce";
            const int n = seconds * (int) sr;
            a->audio.setSize (1, n);
            juce::Random rng (seed), env (77);
            std::vector<float> syl;
            for (int i = 0; i < seconds * 6; ++i) syl.push_back (env.nextFloat() < 0.2f ? 0.0f : 0.3f + 0.7f * env.nextFloat());   // sillabe di ~160 ms, qualcuna di pausa
            float lp = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const double pos = (double) i / sr * 6.0;
                const int k = (int) pos;
                const float frac = (float) (pos - k), e0 = syl[(size_t) k], e1 = syl[(size_t) juce::jmin ((int) syl.size() - 1, k + 1)];
                const float e = e0 + (e1 - e0) * frac;
                lp += 0.3f * ((rng.nextFloat() * 2.0f - 1.0f) - lp);   // rumore passa-basso: la portante
                a->audio.setSample (0, i, amp * e * lp);
            }
            a->buildPeaks();
            return a;
        };

        fc::Project p (formats, sr);
        dub::applyTemplate (p, dub::templates()[0]);   // Guida, M&E, Effetti, Dialoghi
        auto& tr = p.edit().tracks;
        const int len = 6 * (int) sr;
        fc::Clip g; g.id = 100; g.asset = speech (6, 1, 0.5f); g.start = 4 * (int) sr; g.length = len;
        tr[0].clips = { g };
        fc::Clip t; t.id = 101; t.asset = speech (6, 2, 0.4f); t.start = 4 * (int) sr + (int) std::llround (0.137 * sr); t.length = len;   // in ritardo di 137 ms
        tr[3].clips = { t };
        fc::Clip other; other.id = 102; other.asset = speech (6, 3, 0.4f); other.start = 4 * (int) sr; other.length = len;
        {
            // un take con un altro "testo": inviluppo diverso -> somiglianza bassa, non va spostato
            auto a = std::make_shared<fc::Asset>(); a->name = "altro";
            a->audio.setSize (1, len);
            juce::Random r (9);
            for (int i = 0; i < len; ++i) a->audio.setSample (0, i, (((i / 24000) % 3) == 1 ? 0.0f : 0.4f) * (r.nextFloat() * 2.0f - 1.0f));
            other.asset = a;
        }
        tr[2].clips = { other };

        const auto items = fc::align::alignToGuide (p, { 101 }, 1.5, 0.3, false);
        expect (items.size() == 1 && items[0].applied && std::abs (items[0].shiftMs + 137.0) < 3.0 && items[0].confidence > 0.5,
                "allineamento: take in ritardo di 137 ms riportato sulla guida (spostamento " + juce::String (items.empty() ? 0.0 : items[0].shiftMs, 1) + " ms, somiglianza " + juce::String (items.empty() ? 0.0 : items[0].confidence, 2) + ")");
        expect (std::abs ((double) p.findClip (101)->start - 4.0 * sr) < 0.003 * sr, "allineamento: il clip e' tornato in posizione (" + juce::String ((double) (p.findClip (101)->start - 4 * (int) sr) / sr * 1000.0, 1) + " ms)");
        p.undo();
        expect (p.findClip (101)->start == 4 * (int) sr + (int) std::llround (0.137 * sr), "allineamento: un solo passo di undo lo ripristina");

        const auto bad = fc::align::alignToGuide (p, { 102 }, 1.5, 0.6, false);
        expect (bad.size() == 1 && ! bad[0].applied, "allineamento: un take diverso dalla guida non viene spostato (somiglianza " + juce::String (bad.empty() ? 0.0 : bad[0].confidence, 2) + ")");
        const auto onGuide = fc::align::alignToGuide (p, { 100 }, 1.5, 0.3, false);
        expect (onGuide.size() == 1 && ! onGuide[0].applied, "allineamento: i clip della traccia Guida sono ignorati");

        // inizio e fine del parlato
        {
            juce::AudioBuffer<float> b (1, 48000);
            b.clear();
            for (int i = 12000; i < 36000; ++i) b.setSample (0, i, 0.3f * std::sin (0.2f * (float) i));
            const auto sp = fc::align::speechBounds (b, sr);
            expect (sp.ok && std::abs ((double) sp.start - 12000.0) <= 960 && std::abs ((double) sp.end - 36000.0) <= 960, "parlato: inizio e fine individuati (" + juce::String ((juce::int64) sp.start) + " - " + juce::String ((juce::int64) sp.end) + ")");
        }

        // modalita' adattamento durata: il take e' piu' lungo del 10%: ratio ~0,91
        {
            auto slow = std::make_shared<fc::Asset>(); slow->name = "lento";
            const auto src = speech (6, 2, 0.4f);
            const int n2 = (int) (src->length() * 1.10);
            slow->audio.setSize (1, n2);
            for (int i = 0; i < n2; ++i) slow->audio.setSample (0, i, src->audio.getSample (0, (int) ((double) i / 1.10)));
            slow->buildPeaks();
            p.edit().tracks[3].clips[0].asset = slow;
            p.edit().tracks[3].clips[0].length = n2;
            p.edit().tracks[3].clips[0].start = 4 * (int) sr;
            const auto it = fc::align::alignToGuide (p, { 101 }, 1.5, 0.3, true);
            expect (it.size() == 1 && it[0].applied && it[0].stretchRatio > 0.85 && it[0].stretchRatio < 0.96, "allineamento con durata: rapporto " + juce::String (it.empty() ? 0.0 : it[0].stretchRatio, 3) + " (atteso ~0,91)");
        }

        // rilevamento dei tagli del video (solo se accanto al video di prova ci sono cuts.mp4 e smooth.mp4: tagli a 2,0 / 3,5 / 6,0 s)
        {
            const auto cutsFile = videoFile.getSiblingFile ("cuts.mp4"), smoothFile = videoFile.getSiblingFile ("smooth.mp4");
            if (cutsFile.existsAsFile() && smoothFile.existsAsFile())
            {
                std::vector<double> cuts, none;
                juce::String err;
                const bool ok1 = fc::video::detectCuts (cutsFile, 0.5, [] (double) { return true; }, cuts, err);
                const bool ok2 = fc::video::detectCuts (smoothFile, 0.5, [] (double) { return true; }, none, err);
                juce::String found;
                for (auto c : cuts) found << juce::String (c, 2) << " ";
                expect (ok1 && cuts.size() == 3 && std::abs (cuts[0] - 2.0) < 0.1 && std::abs (cuts[1] - 3.5) < 0.1 && std::abs (cuts[2] - 6.0) < 0.1, "tagli video: trovati a " + found + "(attesi 2,0 3,5 6,0) " + err);
                expect (ok2 && none.empty(), "tagli video: nessun falso taglio in un video senza stacchi (" + juce::String ((int) none.size()) + ")");
            }
            else
                std::cout << "     (cuts.mp4 / smooth.mp4 non presenti: salto il test dei tagli)" << std::endl;
        }

        // ducking: i dialoghi parlano da 2 a 4 s e da 4,2 a 5 s (pausa breve) e poi da 9 a 10 s
        {
            fc::Project q (formats, sr);
            dub::applyTemplate (q, dub::templates()[0]);
            auto& qt = q.edit().tracks;
            auto tone = [&] (int seconds, float amp) { auto a = std::make_shared<fc::Asset>(); a->name = "t"; a->audio.setSize (1, seconds * (int) sr); for (int i = 0; i < seconds * (int) sr; ++i) a->audio.setSample (0, i, amp * std::sin (0.1f * (float) i)); a->buildPeaks(); return a; };
            auto put = [&] (size_t ti, int id, int startSec, int secs, float amp) { fc::Clip c; c.id = id; c.asset = tone (secs, amp); c.start = startSec * (int64_t) sr; c.length = secs * (int64_t) sr; qt[ti].clips.push_back (c); };
            put (3, 1, 2, 2, 0.1f); put (3, 2, 4, 1, 0.0f); put (3, 3, 9, 1, 0.1f);   // clip 2: silenzio (pausa tra le battute)
            put (1, 4, 0, 12, 0.3f);                                                    // M&E sotto tutto
            qt[1].live->volumeDb = -6.0f;
            fc::duck::Settings st;
            const auto iv = fc::duck::speechIntervals (q.getSession(), sr, st);
            expect (iv.size() == 2 && std::abs ((double) iv[0].first - 2.0 * sr) < 0.03 * sr && std::abs ((double) iv[0].second - 4.0 * sr) < 0.03 * sr && std::abs ((double) iv[1].first - 9.0 * sr) < 0.03 * sr,
                    "ducking: due zone di parlato trovate (" + juce::String ((int) iv.size()) + ")");
            const auto curve = fc::duck::duckCurve (iv, sr, st);
            expect (curve.size() == 8 && curve[0].second == 0.0f && curve[1].second == -12.0f && curve[2].second == -12.0f && curve[3].second == 0.0f
                        && curve[1].first < iv[0].first && curve[3].first > iv[0].second, "ducking: curva scende prima del parlato e risale dopo (" + juce::String ((int) curve.size()) + " punti)");
            const int touched = fc::duck::apply (q, st, -1);
            auto& lane = q.getSession().tracks[1].volAuto;
            expect (touched == 3 + 0 || touched == 2, "ducking: applicato alle tracce M&E ed Effetti (" + juce::String (touched) + ")");
            bool dips = false, base = false;
            for (auto& a : lane) { dips = dips || std::abs (a.db + 18.0f) < 0.01f; base = base || std::abs (a.db + 6.0f) < 0.01f; }
            expect (lane.size() == 8 && dips && base && q.getSession().tracks[1].live->autoMode.load() == 1, "ducking: il volume del M&E scende di 12 dB attorno al suo fader (-6 -> -18 dB), automazione in lettura");
            q.undo();
            expect (q.getSession().tracks[1].volAuto.empty(), "ducking: un solo passo di undo");
        }
    }

    // consegna: pacchetto MIX + stem per ruolo + rapporto, esclusione della Guida
    void stepDelivery()
    {
        namespace dub = fc::dub;
        auto makeAsset = [] (float amp)
        {
            auto a = std::make_shared<fc::Asset>();
            a->name = "t";
            const int n = 2 * 48000;
            a->audio.setSize (2, n);
            for (int i = 0; i < n; ++i)
            {
                const float v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 48000.0);
                a->audio.setSample (0, i, v);
                a->audio.setSample (1, i, v);
            }
            a->buildPeaks();
            return a;
        };
        fc::Project p (formats, 48000.0);
        dub::applyTemplate (p, dub::templates()[0]);   // Guida, M&E, Effetti, Dialoghi
        p.addTrack();                                   // quinta traccia: nessun bus di ruolo
        const float amps[] = { 0.5f, 0.2f, 0.05f, 0.1f, 0.03f };
        auto& tr = p.edit().tracks;
        for (size_t i = 0; i < tr.size(); ++i)
        {
            tr[i].live->eq.enabled = false; tr[i].live->comp.enabled = false; tr[i].live->limiterOn = false; tr[i].live->volumeDb = 0.0f;
            fc::Clip c; c.id = 9000 + (int) i; c.asset = makeAsset (amps[i]); c.start = 0; c.length = 2 * 48000;
            tr[i].clips = { c };
        }
        p.edit().master->limiterOn = false;
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("arotron_delivery_test");
        dir.deleteRecursively();
        auto go = [&] (const fc::ExportOptions& o, const juce::File& f) { return fc::exportSession (p.getSession(), 48000.0, o, f, "prova", [] (double) { return true; }); };
        auto peakDb = [&] (const juce::File& f) { float pk = 0; measureFileLufs (formats, f, &pk); return pk; };

        fc::ExportOptions o;
        o.bitDepth = 24; o.delivery = true; o.excludeGuide = true; o.presetName = "Prova";
        const auto r = go (o, dir);
        const auto mix = dir.getChildFile ("prova_MIX.wav"), dia = dir.getChildFile ("prova_DIA.wav"), me = dir.getChildFile ("prova_ME.wav"), fx = dir.getChildFile ("prova_FX.wav");
        expect (r.ok && r.items.size() == 4 && mix.existsAsFile() && dia.existsAsFile() && me.existsAsFile() && fx.existsAsFile() && juce::File (r.reportFile).existsAsFile(),
                "consegna: MIX, DIA, ME, FX e rapporto creati (" + juce::String ((int) r.items.size()) + " file) " + r.message);
        auto near = [] (float db, float amp) { return std::abs (db - juce::Decibels::gainToDecibels (amp)) < 0.3f; };
        expect (near (peakDb (mix), 0.38f), "consegna: il MIX non contiene la Guida (picco " + juce::String (peakDb (mix), 2) + " dB, atteso " + juce::String (juce::Decibels::gainToDecibels (0.38f), 2) + ")");
        expect (near (peakDb (dia), 0.1f) && near (peakDb (me), 0.2f) && near (peakDb (fx), 0.05f), "consegna: ogni stem contiene solo il suo bus (" + juce::String (peakDb (dia), 1) + " / " + juce::String (peakDb (me), 1) + " / " + juce::String (peakDb (fx), 1) + " dB)");
        const auto report = juce::File (r.reportFile).loadFileAsString();
        expect (report.contains ("prova_MIX.wav") && report.contains ("prova_DIA.wav") && report.contains ("1 tracce con clip non sono assegnate") && report.contains ("Prova"),
                "consegna: il rapporto elenca i file e segnala la traccia senza bus di ruolo");

        // normalizzazione: il MIX arriva all'obiettivo e gli stem hanno lo stesso guadagno
        fc::ExportOptions n = o; n.norm = fc::ExportOptions::Norm::Lufs; n.normTarget = -23.0; n.ceilingDb = -1.0;
        const auto dir2 = dir.getChildFile ("norm");
        const auto rn = go (n, dir2);
        const float lufs = measureFileLufs (formats, dir2.getChildFile ("prova_MIX.wav"), nullptr);
        expect (rn.ok && std::abs (lufs + 23.0f) < 0.4f && std::abs ((peakDb (dir2.getChildFile ("prova_DIA.wav")) - peakDb (dia)) - (float) rn.gainDb) < 0.2f,
                "consegna: MIX a -23 LUFS (" + juce::String (lufs, 2) + ") e stem con lo stesso guadagno (" + juce::String (rn.gainDb, 1) + " dB)");

        // senza esclusione la Guida entra nel mix; mixdown normale e stem per traccia la rispettano
        fc::ExportOptions plain; plain.bitDepth = 24; plain.excludeGuide = false;
        go (plain, dir.getChildFile ("con_guida.wav"));
        plain.excludeGuide = true;
        go (plain, dir.getChildFile ("senza_guida.wav"));
        expect (near (peakDb (dir.getChildFile ("con_guida.wav")), 0.88f) && near (peakDb (dir.getChildFile ("senza_guida.wav")), 0.38f), "export: l'opzione Escludi la Guida cambia il mix (" + juce::String (peakDb (dir.getChildFile ("con_guida.wav")), 1) + " / " + juce::String (peakDb (dir.getChildFile ("senza_guida.wav")), 1) + " dB)");
        fc::ExportOptions st; st.bitDepth = 24; st.stems = true;
        const auto rs = go (st, dir.getChildFile ("stems"));
        expect (rs.ok && rs.files.size() == 4, "export: gli stem per traccia saltano la Guida (" + juce::String (rs.files.size()) + " file)");
        dir.deleteRecursively();
    }

    // flusso di doppiaggio: modelli di sessione, ruoli, tracce per personaggio, nomi dei take, foglio cue
    void stepWorkflow()
    {
        namespace dub = fc::dub;
        fc::Project p (formats, 48000.0);
        const auto& tpls = dub::templates();
        expect (tpls.size() == 3, "modelli di sessione: " + juce::String ((int) tpls.size()) + " disponibili");
        dub::applyTemplate (p, tpls[0]);
        {
            auto& tr = p.getSession().tracks;
            expect (tr.size() == 4 && tr[0].live->trackType.load() == fc::TrackGuide && tr[0].live->outBus.load() == 4 && tr[1].live->trackType.load() == fc::TrackME && tr[1].live->outBus.load() == 2
                        && tr[2].live->outBus.load() == 3 && tr[3].live->trackType.load() == fc::TrackDialogue && tr[3].live->outBus.load() == 1,
                    "modello Doppiaggio: Guida/M&E/Effetti/Dialoghi sui bus 4/2/3/1");
            expect (std::abs (tr[0].live->volumeDb.load() + 6.0f) < 0.01f && ! tr[0].live->eq.enabled.load() && tr[3].live->eq.enabled.load() && tr[3].live->comp.enabled.load(),
                    "ruoli: la Guida resta pulita a -6 dB, i Dialoghi hanno la catena voce");
            expect (juce::String (fc::groupBusName (0)) == "DIALOGHI" && fc::roleOutBus (fc::TrackGuide) == 4, "bus di gruppo con i nomi dei ruoli");
        }
        dub::applyTemplate (p, tpls[1]);
        expect (p.getSession().tracks.size() == 3 && p.getSession().tracks[1].name == "ADR", "modello ADR: tre tracce, la seconda e' ADR");

        // tracce per personaggio da un CSV
        const juce::String csv = "N,TC IN,TC OUT,Personaggio,Scena,Dialogo\n1,00:01:15:13,00:01:17:20,Sam,12,\"Ciao, <mondo> & amici\"\n2,00:01:20:00,00:01:22:05,Anna,12,Ok\n3,00:01:30:00,00:01:31:00,sam,13,Ancora\n";
        const auto rc = fc::adr::parse (csv);
        juce::StringArray w;
        p.tcFormat = 2;
        const auto cues = fc::adr::toCues (rc, p, 48000.0, 4.0, w);
        expect (cues.size() == 3, "CSV: tre cue letti");
        dub::applyTemplate (p, tpls[2]);   // Dialoghi, M&E, Effetti
        const int created = dub::createCharacterTracks (p, cues);
        expect (created == 2 && p.getSession().tracks.size() == 5 && dub::createCharacterTracks (p, cues) == 0, "una traccia per personaggio (Sam, Anna), senza duplicati (" + juce::String (created) + " create)");
        expect (dub::trackForCue (p, cues[0]) >= 0 && dub::trackForCue (p, cues[0]) == dub::trackForCue (p, cues[2]) && dub::trackForCue (p, cues[0]) != dub::trackForCue (p, cues[1]),
                "il cue va sulla traccia del suo personaggio (anche con maiuscole diverse)");
        {
            const auto* t = p.findTrack (dub::trackForCue (p, cues[1]));
            expect (t != nullptr && t->live->trackType.load() == fc::TrackDialogue && t->live->outBus.load() == 1, "traccia personaggio: ruolo Dialoghi sul bus Dialoghi");
        }

        // tracce per personaggio con un gruppo Dialoghi gia' compresso: le nuove entrano nel gruppo e restano visibili
        {
            fc::Project q (formats, 48000.0);
            q.addTrack();
            const int gq = q.addGroup (juce::translate ("Dialoghi"));
            q.setTrackGroup (q.getSession().tracks[0].id, gq);
            q.setGroupCollapsed (gq, true);
            const int tracksBeforeQ = (int) q.getSession().tracks.size();
            const int made = dub::createCharacterTracks (q, cues);
            int inG = 0;
            for (auto& t : q.getSession().tracks) inG += t.groupId == gq ? 1 : 0;
            expect (made == 2 && inG == 3 && (int) q.getSession().tracks.size() == tracksBeforeQ + 2 && ! q.findGroup (gq)->collapsed,
                    "tracce per personaggio: " + juce::String (made) + " create, " + juce::String (inG) + " nel gruppo Dialoghi, gruppo aperto");
        }

        // schema dei nomi
        fc::AdrCue c;
        c.character = "Sam"; c.scene = ""; c.reel = "R2"; c.number = 7;
        expect (dub::expandNaming ("{personaggio}_{scena}_{cue}_T{take}", c, "Serie") == "Sam_7_T{take}", "nome take: campi vuoti senza trattini doppi (" + dub::expandNaming ("{personaggio}_{scena}_{cue}_T{take}", c, "Serie") + ")");
        expect (dub::expandNaming ("{progetto}-{bobina}-{cue}/{take}", c, "Serie") == "Serie-R2-7{take}" || dub::expandNaming ("{progetto}-{bobina}-{cue}/{take}", c, "Serie").startsWith ("Serie-R2-7"),
                "nome take: segnaposto progetto/bobina/cue");
        expect (dub::expandNaming ("", c, "Serie") == "Sam_7_T{take}", "nome take: schema vuoto = predefinito");

        // foglio cue
        {
            auto pp = std::make_unique<fc::Project> (formats, 48000.0);
            pp->addCues (cues, true);
            const auto html = fc::cuesheet::buildHtml (*pp, "Prova");
            expect (html.contains ("Ciao, &lt;mondo&gt; &amp; amici") && html.contains ("<h2>Sam</h2>") && html.contains ("<h2>Anna</h2>") && html.contains ("3 cue: 3 da registrare")
                        && html.contains (pp->formatTc (75.0 + 13.0 / 24.0)),
                    "foglio cue: riepilogo, copione per personaggio, testo con caratteri speciali protetti");
        }
    }

    // temi: lettura dei temi Reaper (colori BGR, zip), palette derivata coerente, applicazione e ricolorazione dei componenti
    void stepTheme()
    {
        namespace rp = fc::reaper;
        expect (rp::fromColorRef (255) == juce::Colour (255, 0, 0) && rp::fromColorRef (0x00ff0000) == juce::Colour (0, 0, 255)
                && rp::fromColorRef ((int) 0x8000ff00u) == juce::Colour (0, 255, 0), "tema Reaper: COLORREF letto come BGR, byte alti ignorati");

        const juce::String ini = "[REAPER]\nui_img=x\n[color theme]\ncol_mixerbg=3355443\ncol_main_text2=9211020\ncol_main_text=2236958\ncol_tr1_bg=4342338\n"
                                 "col_tr2_bg=4539717\narrange_vgrid=4605510\ngenlist_selbg=6840646\ncol_main_bg=-2135838293\ncol_arrangebg=4539717\n[other]\ncol_mixerbg=1\n";
        const auto colors = rp::parseColorTheme (ini);
        expect (colors.size() == 9 && colors.at ("col_mixerbg") == 3355443 && colors.count ("ui_img") == 0, "tema Reaper: legge solo la sezione [color theme] (" + juce::String ((int) colors.size()) + " chiavi)");

        auto p = rp::paletteFromColors (colors, "Prova");
        expect (p.panel == juce::Colour (0x33, 0x33, 0x33) && p.lane == juce::Colour (0x42, 0x42, 0x42) && p.laneAlt == juce::Colour (0x45, 0x45, 0x45),
                "tema Reaper: pannello e tracce presi dal tema");
        expect (rp::contrastRatio (p.text, p.panel) >= 4.5f && rp::contrastRatio (p.dim, p.panel) >= 2.0f, "tema Reaper: testo leggibile sul pannello (contrasto " + juce::String (rp::contrastRatio (p.text, p.panel), 1) + ")");
        expect (p.blue != fc::theme::Palette::dark().blue && rp::contrastRatio (p.blue, p.panel) >= 1.5f && rp::contrastRatio (p.blue, juce::Colours::white) >= 3.0f, "tema Reaper: accento dal colore di selezione, visibile sul pannello e con testo bianco leggibile");

        // tracce chiare su pannelli scuri (come il tema Default 5): le tracce chiare vengono scartate, il testo resta leggibile
        auto mixed = colors;
        mixed["col_tr1_bg"] = 0x00c6c6c4; mixed["col_tr2_bg"] = 0x00bab8b8; mixed["col_arrangebg"] = 0x00c0c0be;
        const auto pm = rp::paletteFromColors (mixed, "Misto");
        expect (pm.lane.getPerceivedBrightness() < 0.5f && pm.laneAlt.getPerceivedBrightness() < 0.5f, "tema Reaper: tracce chiare su pannelli scuri non usate");

        // tema chiaro
        rp::ColorMap lightMap;
        lightMap["col_mixerbg"] = 0x00e0e0e0; lightMap["col_main_text"] = 0x00202020;
        const auto pl = rp::paletteFromColors (lightMap, "Luce");
        expect (pl.text.getPerceivedBrightness() < 0.5f && rp::contrastRatio (pl.text, pl.panel) >= 4.5f, "tema Reaper: tema chiaro con testo scuro");

        // niente colori: palette completa dal tema scuro
        const auto empty = rp::paletteFromColors ({}, "Vuoto");
        expect (empty.panel == fc::theme::Palette::dark().panel, "tema Reaper: senza chiavi resta il tema scuro");

        // file: zip (.ReaperThemeZip) con il .ReaperTheme nella radice, file semplice, file non valido
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("adr_theme_test");
        dir.deleteRecursively();
        dir.createDirectory();
        {
            juce::ZipFile::Builder zb;
            const juce::String png = "not really a png";
            zb.addEntry (new juce::MemoryInputStream (png.toRawUTF8(), png.getNumBytesAsUTF8(), true), 5, "Prova_unpacked/tcp_bg.png", juce::Time::getCurrentTime());
            zb.addEntry (new juce::MemoryInputStream (ini.toRawUTF8(), ini.getNumBytesAsUTF8(), true), 5, "Prova_unpacked.ReaperTheme", juce::Time::getCurrentTime());
            juce::FileOutputStream out (dir.getChildFile ("Prova.ReaperThemeZip"));
            zb.writeToStream (out, nullptr);
        }
        dir.getChildFile ("Semplice.ReaperTheme").replaceWithText (ini);
        dir.getChildFile ("Rotto.ReaperTheme").replaceWithText ("questo non e' un tema");
        fc::theme::Palette lp;
        juce::String err;
        expect (rp::loadThemeFile (dir.getChildFile ("Prova.ReaperThemeZip"), lp, err) && lp.panel == p.panel && lp.name == "Prova", "tema Reaper: zip caricato (nome '" + lp.name + "')");
        expect (rp::loadThemeFile (dir.getChildFile ("Semplice.ReaperTheme"), lp, err) && lp.name == "Semplice", "tema Reaper: file .ReaperTheme semplice caricato");
        expect (! rp::loadThemeFile (dir.getChildFile ("Rotto.ReaperTheme"), lp, err) && err.isNotEmpty(), "tema Reaper: file non valido rifiutato (" + err + ")");
        dir.deleteRecursively();

        // salvataggio JSON
        const auto back = fc::theme::Palette::fromVar (juce::JSON::parse (juce::JSON::toString (p.toVar())));
        expect (back.panel == p.panel && back.text == p.text && back.blue == p.blue && back.name == "Prova", "tema: palette salvata e riletta identica");

        // applicazione dal vivo: token, derivati e colori impostati sui componenti
        const auto before = fc::theme::Palette::current();
        fc::theme::apply (before);   // i derivati partono dalla formula (i valori iniziali sono letterali)
        juce::Label label;
        label.setColour (juce::Label::textColourId, fc::theme::dim);
        label.setColour (juce::Label::backgroundColourId, juce::Colour (0xff123456));   // colore non del tema: non va toccato
        juce::Component holder;
        holder.addAndMakeVisible (label);
        holder.addToDesktop (juce::ComponentPeer::windowIsTemporary);
        const auto wellBefore = fc::theme::well;
        fc::theme::apply (fc::theme::Palette::light());
        expect (fc::theme::bg == fc::theme::Palette::light().bg && fc::theme::well != wellBefore, "tema: apply aggiorna i token e i derivati");
        expect (label.findColour (juce::Label::textColourId) == fc::theme::dim && label.findColour (juce::Label::backgroundColourId) == juce::Colour (0xff123456),
                "tema: i colori dei componenti seguono il tema, quelli estranei no");
        fc::theme::apply (before);
        expect (fc::theme::bg == before.bg && fc::theme::well == wellBefore && label.findColour (juce::Label::textColourId) == before.dim, "tema: tema precedente ripristinato");
        holder.removeFromDesktop();
    }

    // effetti integrati: ogni effetto deve fare il suo lavoro, lo stato deve tornare identico e l'host deve poterli inserire
    void stepBuiltin()
    {
        using FX = fc::BuiltinFx;
        constexpr double sr = 48000.0;
        auto make = [&] (FX::Kind k, std::initializer_list<std::pair<const char*, float>> values)
        {
            auto fx = std::make_unique<FX> (k);
            fx->setRateAndBufferSizeDetails (sr, 512);
            fx->prepareToPlay (sr, 512);
            for (auto& v : values)
                if (auto* pr = fx->param (v.first)) pr->setValueNotifyingHost (pr->convertTo0to1 (v.second));
            return fx;
        };
        // elabora in blocchi da 512; ritorna il picco assoluto negli ultimi `tailFrom` campioni
        auto run = [&] (FX& fx, const std::vector<float>& in, juce::AudioBuffer<float>& out)
        {
            out.setSize (2, (int) in.size());
            for (int i = 0; i < (int) in.size(); ++i) { out.setSample (0, i, in[(size_t) i]); out.setSample (1, i, in[(size_t) i]); }
            juce::MidiBuffer midi;
            for (int pos = 0; pos < (int) in.size(); pos += 512)
            {
                const int n = juce::jmin (512, (int) in.size() - pos);
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, n);
                fx.processBlock (view, midi);
            }
        };
        auto peak = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            float p = 0.0f;
            for (int i = from; i < to; ++i) p = juce::jmax (p, std::abs (b.getSample (0, i)));
            return p;
        };
        auto sine = [&] (double hz, float amp, int n)
        {
            std::vector<float> v ((size_t) n);
            for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr);
            return v;
        };
        auto amp = [] (const juce::AudioBuffer<float>& b, int from, int to)   // ampiezza di una sinusoide da RMS (non dipende dalla fase di campionamento)
        {
            double e = 0.0;
            for (int i = from; i < to; ++i) e += (double) b.getSample (0, i) * b.getSample (0, i);
            return (float) std::sqrt (2.0 * e / (to - from));
        };
        auto gainToDbTest = [] (float g) { return 20.0f * std::log10 (g + 1.0e-9f); };
        auto impulse = [] (int n) { std::vector<float> v ((size_t) n, 0.0f); v[0] = 1.0f; return v; };
        juce::AudioBuffer<float> o;

        { auto fx = make (FX::Filter, { { "hp", 300.0f }, { "lp", 4000.0f } });
          run (*fx, sine (50.0, 0.8f, 48000), o);   const float low = peak (o, 24000, 48000);
          run (*fx, sine (1000.0, 0.8f, 48000), o); const float mid = peak (o, 24000, 48000);
          run (*fx, sine (12000.0, 0.8f, 48000), o); const float high = peak (o, 24000, 48000);
          expect (low < 0.05f && mid > 0.72f && high < 0.1f, "filtro HP/LP: taglia 50 Hz e 12 kHz, passa 1 kHz (" + juce::String (low, 3) + " / " + juce::String (mid, 3) + " / " + juce::String (high, 3) + ")"); }

        { auto fx = make (FX::DeEsser, { { "freq", 5000.0f }, { "thr", -30.0f }, { "max", 12.0f } });
          run (*fx, sine (8000.0, 0.3f, 48000), o);  const float hf = peak (o, 24000, 48000);
          fx->reset();
          run (*fx, sine (200.0, 0.3f, 48000), o);   const float lf = peak (o, 24000, 48000);
          expect (hf < 0.15f && lf > 0.29f, "de-esser: riduce 8 kHz (0.3 -> " + juce::String (hf, 3) + "), lascia 200 Hz (" + juce::String (lf, 3) + ")"); }

        { auto fx = make (FX::Gate, { { "thr", -50.0f }, { "range", 40.0f }, { "att", 1.0f }, { "rel", 50.0f } });
          run (*fx, sine (300.0, 0.001f, 48000), o); const float quiet = peak (o, 24000, 48000);
          const float gateGr = fx->meterDb (0);   // il gate e' chiuso: ~40 dB di attenuazione
          fx->reset();
          run (*fx, sine (300.0, 0.5f, 48000), o);   const float loud = peak (o, 24000, 48000);
          expect (gateGr > 30.0f, "gate: misuratore di riduzione leggibile (" + juce::String (gateGr, 1) + " dB)");
          expect (quiet < 0.0002f && loud > 0.49f, "gate: segnale basso attenuato (" + juce::String (quiet, 5) + "), forte intatto (" + juce::String (loud, 3) + ")"); }

        { // multibanda: con rapporto 1:1 e' trasparente a tutte le frequenze (anche sui crossover); comprime solo la banda interessata
          auto flat = make (FX::Multiband, {});
          for (const char* b : { "lowRatio", "midRatio", "highRatio" }) flat->param (b)->setValueNotifyingHost (flat->param (b)->convertTo0to1 (1.0f));
          float worst = 0.0f;
          for (double hz : { 60.0, 200.0, 500.0, 3000.0, 8000.0 })
          {
              flat->reset();
              run (*flat, sine (hz, 0.5f, 24000), o);
              worst = juce::jmax (worst, std::abs (gainToDbTest (amp (o, 12000, 24000) / 0.5f)));
          }
          expect (worst < 0.5f, "multibanda: 1:1 trasparente da 60 Hz a 8 kHz (scarto max " + juce::String (worst, 2) + " dB)");
          auto mb = make (FX::Multiband, { { "lowThr", -30.0f }, { "lowRatio", 10.0f }, { "highRatio", 1.0f }, { "midRatio", 1.0f } });
          run (*mb, sine (100.0, 0.5f, 24000), o);  const float lowOut = amp (o, 12000, 24000);
          const float lowGr = mb->meterDb (0);
          mb->reset();
          run (*mb, sine (8000.0, 0.5f, 24000), o); const float highOut = amp (o, 12000, 24000);
          expect (lowGr > 5.0f && mb->meterDb (2) < 0.5f, "multibanda: misuratori (bassi " + juce::String (lowGr, 1) + " dB, alti " + juce::String (mb->meterDb (2), 1) + " dB)");
          expect (lowOut < 0.2f && highOut > 0.47f, "multibanda: comprime i bassi (0.5 -> " + juce::String (lowOut, 3) + ") e non tocca gli alti (" + juce::String (highOut, 3) + ")"); }

        { auto fx = make (FX::Reverb, { { "mix", 100.0f }, { "size", 0.8f } });
          auto pulse = [] (int n) { std::vector<float> v ((size_t) n, 0.0f); v[2400] = 1.0f; return v; };   // un po' di silenzio prima: il livello del riverbero sale in 10 ms
          run (*fx, pulse (24000), o);
          const float tail = peak (o, 7200, 24000);
          auto dry = make (FX::Reverb, { { "mix", 0.0f } });
          run (*dry, pulse (7200), o);
          expect (tail > 0.001f && std::abs (o.getSample (0, 2400) - 1.0f) < 1.0e-3f && peak (o, 2401, 7200) < 1.0e-5f,
                  "riverbero: coda presente (" + juce::String (tail, 4) + "), con mix 0 passa solo il secco"); }

        { auto fx = make (FX::Delay, { { "time", 250.0f }, { "fb", 50.0f }, { "mix", 50.0f } });
          run (*fx, impulse (48000), o);
          expect (std::abs (o.getSample (0, 0) - 1.0f) < 1.0e-4f && std::abs (o.getSample (0, 12000) - 0.5f) < 1.0e-3f && peak (o, 24000, 24200) > 0.05f && std::abs (o.getSample (0, 6000)) < 1.0e-5f,
                  "delay: eco a 250 ms (" + juce::String (o.getSample (0, 12000), 3) + ") e ripetizione con feedback"); }

        { auto fx = make (FX::Saturator, { { "drive", 20.0f }, { "tone", 20000.0f }, { "mix", 100.0f }, { "out", 0.0f } });
          run (*fx, sine (200.0, 0.1f, 4800), o);
          const float sat = peak (o, 2400, 4800);
          auto dry = make (FX::Saturator, { { "mix", 0.0f } });
          run (*dry, sine (200.0, 0.1f, 4800), o);
          expect (sat > 0.7f && sat <= 1.0f && std::abs (peak (o, 2400, 4800) - 0.1f) < 1.0e-4f, "saturazione: 0.1 -> " + juce::String (sat, 3) + ", con mix 0 invariato"); }

        { auto fx = make (FX::Chorus, { { "rate", 1.0f }, { "depth", 5.0f }, { "mix", 100.0f } });
          const auto in = sine (440.0, 0.5f, 48000);
          run (*fx, in, o);
          double diff = 0.0; bool finite = true;
          for (int i = 24000; i < 48000; ++i) { diff += std::abs (o.getSample (0, i) - in[(size_t) i]); finite = finite && std::isfinite (o.getSample (0, i)); }
          expect (finite && diff > 100.0 && peak (o, 24000, 48000) <= 0.52f, "chorus: modula il segnale senza superare il livello (differenza " + juce::String (diff, 0) + ")"); }

        { auto a = make (FX::Reverb, { { "size", 0.9f }, { "mix", 60.0f } });
          juce::MemoryBlock mb; a->getStateInformation (mb);
          FX b (FX::Reverb); b.setStateInformation (mb.getData(), (int) mb.getSize());
          const auto desc = FX::describe (FX::Delay);
          std::unique_ptr<juce::XmlElement> xml (desc.createXml());
          juce::PluginDescription back;
          const bool descOk = xml != nullptr && back.loadFromXml (*xml) && FX::create (back) != nullptr && FX::create (back)->getKind() == FX::Delay;
          expect (std::abs (b.param ("size")->convertFrom0to1 (b.param ("size")->getValue()) - 0.9f) < 1.0e-3f
                  && std::abs (b.param ("mix")->convertFrom0to1 (b.param ("mix")->getValue()) - 60.0f) < 0.1f && descOk,
                  "effetti integrati: stato e descrizione salvati e ripristinati"); }

        {
            auto live = std::make_shared<fc::TrackLive>();
            bool called = false;
            host.addBuiltin (live, FX::Gate, [&called] (juce::String) { called = true; });
            auto chain = live->getChain();
            expect (called && chain != nullptr && chain->slots.size() == 1 && chain->slots[0]->ready.load() && chain->slots[0]->name == "Gate / Expander",
                    "host: effetto integrato inserito nella catena e pronto");
        }
    }

    void stepLimiter()
    {
        fc::TrackLive master;
        master.limiterOn = true;
        master.ceilingDb = -1.0f;
        fc::MasterBus bus;
        bus.prepare (48000.0);
        juce::AudioBuffer<float> buf (2, fc::Mixer::kChunk);

        auto run = [&] (bool on, float amp, float& peakOut, double& rmsOut)
        {
            master.limiterOn = on;
            master.grDb = 0.0f;
            bus.reset();
            double phase = 0.0, energy = 0.0;
            int64_t count = 0;
            float peak = 0.0f;
            for (int b = 0; b < 100; ++b)
            {
                for (int i = 0; i < fc::Mixer::kChunk; ++i, phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0)
                {
                    float v = amp * (float) std::sin (phase);
                    if (amp > 1.0f && ((b * fc::Mixer::kChunk + i) % 48000) < 40) v *= 2.0f;      // transitori sovrapposti
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, 0.7f * v);
                }
                bus.process (master, buf, fc::Mixer::kChunk);
                peak = juce::jmax (peak, buf.getMagnitude (0, fc::Mixer::kChunk));
                for (int i = 0; i < fc::Mixer::kChunk; ++i) { energy += (double) buf.getSample (0, i) * buf.getSample (0, i); ++count; }
            }
            peakOut = peak;
            rmsOut = std::sqrt (energy / (double) count);
        };

        float pOn = 0, pOff = 0, pQuiet = 0;
        double rOn = 0, rOff = 0, rQuiet = 0;
        run (true, 1.8f, pOn, rOn);
        const float grLoud = master.grDb.load();
        run (false, 1.8f, pOff, rOff);
        run (true, 0.5f, pQuiet, rQuiet);
        expect (pOn <= 0.8913f + 1.0e-4f, "limiter: nessun picco oltre -1 dBFS (picco " + juce::String (pOn, 4) + ")");
        expect (pOff > 1.5f, "limiter spento: il segnale non viene toccato (picco " + juce::String (pOff, 2) + ")");
        expect (rOn > 0.45, "limiter: livello medio mantenuto (rms " + juce::String (rOn, 3) + ")");
        expect (grLoud > 3.0f, "limiter: riduzione di guadagno misurata " + juce::String (grLoud, 1) + " dB");
        expect (std::abs (pQuiet - 0.5f) < 0.02f && std::abs (rQuiet - 0.5 / std::sqrt (2.0)) < 0.02,
                "limiter: segnale sotto il tetto invariato (picco " + juce::String (pQuiet, 3) + ")");
    }

    void stepPlugin (std::function<void()> next)
    {
        if (! pluginFile.exists()) { std::cout << "     (nessun .vst3 passato: salto i test plug-in)" << std::endl; next(); return; }

        auto live = project->getSession().tracks[0].live;
        host.onError = [] (const juce::String& m) { std::cout << "     (plug-in: " << m << ")" << std::endl; };
        host.addFromFile (live, pluginFile, [this, live, next] (juce::String problem)
        {
            auto chain = live->getChain();
            const bool loaded = problem.isEmpty() && chain != nullptr && chain->slots.size() == 1 && chain->slots[0]->ready.load();
            expect (loaded, "plug-in VST3 caricato: " + (chain != nullptr && ! chain->slots.empty() ? chain->slots[0]->name : problem));
            if (! loaded) { next(); return; }

            fc::Mixer m;
            juce::AudioBuffer<float> buf (2, 2048);
            m.render (project->getSession(), 96000, buf, 0, 2048);
            bool finite = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 2048; ++i) finite = finite && std::isfinite (buf.getSample (c, i));
            expect (finite && buf.getMagnitude (0, 2048) > 0.0f, "audio attraverso il plug-in: valido, picco " + juce::String (buf.getMagnitude (0, 2048), 3));

            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_plugin.adb");
            juce::String err;
            expect (project->save (tmp, err), "salva sessione con plug-in");

            project4 = std::make_unique<fc::Project> (formats, 48000.0);
            project4->onSessionLoaded = [this] { host.restoreAll (project4->getSession()); };
            project4->loadSession (tmp, [this, live, next] (juce::String)
            {
                waitForRestore (0, live, next);
            });
        });
    }

    void waitForRestore (int attempt, std::shared_ptr<fc::TrackLive> original, std::function<void()> next)
    {
        auto chain = project4->getSession().tracks[0].live->getChain();
        const bool ok = chain != nullptr && chain->slots.size() == 1 && chain->slots[0]->ready.load();
        if (! ok && attempt < 40)
        {
            juce::Timer::callAfterDelay (250, [this, attempt, original, next] { waitForRestore (attempt + 1, original, next); });
            return;
        }
        expect (ok, "plug-in ripristinato dalla sessione (con il suo stato)");
        host.removeSlot (*original, 0);
        expect (original->getChain() == nullptr || original->getChain()->slots.empty(), "plug-in rimosso dalla catena");
        host.closeAllWindows();
        next();
    }

    // registrazione simulata: l'ingresso viene iniettato direttamente nel callback audio
    void stepRecording()
    {
        auto& s = project->getSession();
        const int trackId = s.tracks.back().id;
        const int64_t startPos = 3 * 48000;

        fc::AudioEngine eng;
        eng.setNumInputChannelsForTest (2);
        eng.publish (std::make_shared<const fc::Session> (s));
        auto recFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_rec.wav");

        eng.setPosition (startPos);
        const auto err = eng.startRecording ({ { trackId, recFile, 2 } });
        expect (err.isEmpty(), "startRecording " + err);
        eng.play();

        const int block = 480, blocks = 200;
        std::vector<float> inL ((size_t) block), inR ((size_t) block), outL ((size_t) block), outR ((size_t) block);
        double phase = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i, phase += 2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0)
            {
                inL[(size_t) i] = 0.5f * (float) std::sin (phase);
                inR[(size_t) i] = 0.25f * (float) std::sin (phase);
            }
            const float* ins[2] = { inL.data(), inR.data() };
            float* outs[2] = { outL.data(), outR.data() };
            eng.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, block, juce::AudioIODeviceCallbackContext());
        }
        expect (eng.getRecordStart() == startPos, "inizio registrazione sulla testina: " + juce::String ((juce::int64) eng.getRecordStart()));
        eng.stop();
        const auto results = eng.stopRecording();
        expect (results.size() == 1 && results[0].length == block * blocks && results[0].startPos == startPos,
                "take registrato: " + juce::String (results.empty() ? 0 : (juce::int64) results[0].length) + " campioni");

        std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (recFile));
        float pl = 0.0f, pr = 0.0f;
        if (rd != nullptr)
        {
            juce::AudioBuffer<float> b ((int) rd->numChannels, (int) rd->lengthInSamples);
            rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
            pl = b.getMagnitude (0, 0, b.getNumSamples());
            pr = rd->numChannels > 1 ? b.getMagnitude (1, 0, b.getNumSamples()) : 0.0f;
        }
        expect (rd != nullptr && rd->numChannels == 2 && rd->bitsPerSample == 24 && std::abs (pl - 0.5f) < 0.01f && std::abs (pr - 0.25f) < 0.01f,
                "WAV registrato stereo 24 bit, picchi " + juce::String (pl, 3) + " / " + juce::String (pr, 3));

        std::vector<fc::Project::RecItem> items;
        for (auto& r : results) items.push_back ({ r.trackId, r.file, r.startPos, 480 });   // latenza simulata: 480 campioni
        project->addRecordings (items, [this, trackId] (juce::StringArray errs)
        {
            auto* t = project->findTrack (trackId);
            const bool ok = errs.isEmpty() && t != nullptr && t->clips.size() == 1
                         && t->clips[0].start == 3 * 48000 - 480 && t->clips[0].length == 96000;
            expect (ok, "clip registrato posizionato con compensazione della latenza (start "
                            + juce::String (t != nullptr && ! t->clips.empty() ? (juce::int64) t->clips[0].start : -1) + ")");
            stepPunch ([this] { stepVideo(); });
        });
    }

    void stepPunch (std::function<void()> next)
    {
        // registrazione ADR simulata: punch tra 0,5 s e 1,5 s, bip a 0,2 s
        fc::Project p (formats, 48000.0);
        fc::AudioEngine eng;
        eng.setNumInputChannelsForTest (2);
        eng.publish (std::make_shared<const fc::Session> (p.getSession()));
        const int trackId = p.getSession().tracks[0].id;
        auto recFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_punch.wav");
        const int block = 480, blocks = 200;
        eng.setPunch (50 * block, 150 * block);
        eng.setBeeps ({ 20 * block });
        eng.setPosition (0);
        expect (eng.startRecording ({ { trackId, recFile, 0 } }).isEmpty(), "punch: start della registrazione");
        eng.play();

        std::vector<float> inL ((size_t) block), inR ((size_t) block), outL ((size_t) block), outR ((size_t) block);
        double beepEnergyAtBeep = 0.0, beepEnergyBefore = 0.0, beepEnergyAfter = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i) inL[(size_t) i] = inR[(size_t) i] = 0.5f;
            const float* ins[2] = { inL.data(), inR.data() };
            float* outs[2] = { outL.data(), outR.data() };
            eng.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, block, juce::AudioIODeviceCallbackContext());
            double e = 0.0;
            for (int i = 0; i < block; ++i) e += std::abs (outL[(size_t) i]);
            if (b >= 20 && b < 30) beepEnergyAtBeep += e;
            else if (b < 20) beepEnergyBefore += e;
            else if (b >= 40) beepEnergyAfter += e;
        }
        eng.stop();
        const auto results = eng.stopRecording();
        expect (beepEnergyAtBeep > 1000.0 && beepEnergyBefore < 1.0e-6 && beepEnergyAfter < 1.0e-6, "bip: il tono compare solo nell'intervallo previsto (energia " + juce::String (beepEnergyAtBeep, 0) + ")");
        expect (results.size() == 1 && results[0].startPos == 50 * block && results[0].length == 100 * block,
                "punch: registrati solo i 100 blocchi tra ingresso e uscita (" + juce::String (results.empty() ? 0 : (juce::int64) results[0].length) + " campioni, inizio "
                    + juce::String (results.empty() ? 0 : (juce::int64) results[0].startPos) + ")");
        next();
    }

    void stepVideo()
    {
        if (! videoFile.existsAsFile()) { std::cout << "     (nessun video passato: salto i test video)" << std::endl; finishCb (fails); return; }

        const int tracksBefore = (int) project->getSession().tracks.size();
        project->tcFormat = 1;   // sessione a 24 fps: il video a 25 fps deve portarla a 25
        project->setVideo (videoFile, [this, tracksBefore] (juce::String msg)
        {
            auto v = project->getSession().video;
            expect (msg.isEmpty() && v != nullptr, "video caricato " + msg);
            expect (project->tcFormat == 2, "fps del video riconosciuti: il timecode della sessione passa da 24 a 25 fps");
            if (v == nullptr) { finishCb (fails); return; }
            expect (std::abs (v->duration - 20.0) < 0.1 && std::abs (v->fps - 25.0) < 0.01 && v->width == 640 && v->height == 360,
                    "probe video: " + juce::String (v->duration, 2) + " s, " + juce::String (v->fps, 2) + " fps, " + juce::String (v->width) + "x" + juce::String (v->height));
            bool allValid = ! v->thumbs.empty();
            for (auto& im : v->thumbs) allValid = allValid && im.isValid() && im.getHeight() == 56;
            expect (allValid && (int) v->thumbs.size() == 20, "miniature: " + juce::String ((int) v->thumbs.size()));

            expect ((int) project->getSession().tracks.size() == tracksBefore + 1 && project->getSession().tracks.front().name == "Audio video",
                    "audio del video estratto in una nuova traccia, in cima (vicino alla corsia video)");
            auto& t = project->getSession().tracks.front();
            expect (! t.clips.empty() && std::abs (t.clips[0].length - 20 * 48000) < 4096, "durata audio estratto: "
                        + juce::String (t.clips.empty() ? 0 : (juce::int64) t.clips[0].length));

            // salva/ricarica con video
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_video.adb");
            juce::String err;
            expect (project->save (tmp, err), "salva sessione con video");
            project2 = std::make_unique<fc::Project> (formats, 48000.0);
            project2->loadSession (tmp, [this] (juce::String e)
            {
                expect (e.isEmpty() && project2->getSession().video != nullptr && project2->getSession().video->thumbs.size() == 20,
                        "ricarica sessione con video " + e);

                // esportazione video con il mix
                auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vmix.wav");
                auto mov = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vmix.mov");
                juce::String msg;
                bool ok = fc::renderMixdown (project->getSession(), 48000.0, wav, [] (double) { return true; }, msg);
                ok = ok && fc::video::muxAudio (videoFile, wav, mov, 0.2, [] (double) { return true; }, msg);
                expect (ok, "mux video + mix: " + msg.replaceCharacter ('\n', ' '));
                auto info = fc::video::probe (mov);
                expect (info.ok && info.hasAudio && std::abs (info.duration - 20.2) < 0.1 && info.width == 640,
                        "il .mov esportato (offset video +200 ms) ha video e audio, " + juce::String (info.duration, 2) + " s (attesi ~20.2)");

                // esportazione dell'intervallo 5 s - 9 s: audio e video durano 4 s
                {
                    auto wavR = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vrange.wav");
                    auto movR = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vrange.mov");
                    juce::String msgR;
                    bool okR = fc::renderMixdown (project->getSession(), 48000.0, wavR, [] (double) { return true; }, msgR, 5 * 48000, 9 * 48000);
                    std::unique_ptr<juce::AudioFormatReader> rdR (formats.createReaderFor (wavR));
                    const bool lenOk = rdR != nullptr && rdR->lengthInSamples == 4 * 48000;
                    okR = okR && fc::video::muxAudio (videoFile, wavR, movR, 0.2, [] (double) { return true; }, msgR, 5.0, 9.0);
                    auto infoR = fc::video::probe (movR);
                    expect (okR && lenOk && infoR.ok && infoR.hasAudio && std::abs (infoR.duration - 4.0) < 0.15,
                            "esportazione video dell'intervallo 5-9 s: " + juce::String (infoR.duration, 2) + " s (attesi 4), audio " + juce::String (lenOk ? "ok" : "lunghezza errata") + " " + msgR.replaceCharacter ('\n', ' '));
                    wavR.deleteFile(); movR.deleteFile();
                }

                // burn-in: timecode in alto a sinistra e battuta in basso incisi sul video dell'intervallo 5-9 s
                if (fc::video::burnInSupported())
                {
                    auto wavB = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vburn.wav");
                    auto movP = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vplain.mov");
                    auto movB = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_vburn.mov");
                    juce::String msgB;
                    bool okB = fc::renderMixdown (project->getSession(), 48000.0, wavB, [] (double) { return true; }, msgB, 5 * 48000, 9 * 48000);
                    okB = okB && fc::video::muxAudio (videoFile, wavB, movP, 0.2, [] (double) { return true; }, msgB, 5.0, 9.0);
                    fc::video::BurnIn burn;
                    burn.timecodeAt = [] (double t) { return "01:00:" + juce::String ((int) t).paddedLeft ('0', 2) + ":00"; };
                    burn.lines.push_back ({ 5.0, 9.0, "Sam", "Questa e' la battuta di prova per il burn-in", 3 });
                    okB = okB && fc::video::muxAudioBurnIn (videoFile, wavB, movB, 0.2, burn, [] (double) { return true; }, msgB, 5.0, 9.0);
                    const auto infoB = fc::video::probe (movB);
                    expect (okB && infoB.ok && infoB.hasAudio && std::abs (infoB.duration - 4.0) < 0.2 && infoB.width == 640, "burn-in: video con audio di " + juce::String (infoB.duration, 2) + " s, larghezza " + juce::String (infoB.width) + " " + msgB);
                    const auto plain = fc::video::thumbnails (movP, 1.0, 3, 160);
                    const auto burned = fc::video::thumbnails (movB, 1.0, 3, 160);
                    if (plain.size() == 3 && burned.size() == 3 && plain[1].isValid() && burned[1].isValid() && plain[1].getBounds() == burned[1].getBounds())
                    {
                        auto diff = [&] (juce::Rectangle<float> rel)
                        {
                            const auto r = plain[1].getBounds();
                            const auto area = juce::Rectangle<int> ((int) (rel.getX() * r.getWidth()), (int) (rel.getY() * r.getHeight()), (int) (rel.getWidth() * r.getWidth()), (int) (rel.getHeight() * r.getHeight()));
                            double sum = 0.0;
                            for (int y = area.getY(); y < area.getBottom(); ++y)
                                for (int x = area.getX(); x < area.getRight(); ++x)
                                    sum += std::abs (plain[1].getPixelAt (x, y).getPerceivedBrightness() - burned[1].getPixelAt (x, y).getPerceivedBrightness());
                            return 255.0 * sum / (double) juce::jmax (1, area.getWidth() * area.getHeight());
                        };
                        if (const auto dbg = juce::SystemStats::getEnvironmentVariable ("ADR_DEBUG_BURN", {}); dbg.isNotEmpty())   // per guardare il risultato a occhio
                        {
                            juce::FileOutputStream o (juce::File (dbg).getChildFile ("burn.png").deleteFile() ? juce::File (dbg).getChildFile ("burn.png") : juce::File (dbg).getChildFile ("burn.png"));
                            if (o.openedOk()) juce::PNGImageFormat().writeImageToStream (burned[1], o);
                        }
                        const double tc = diff ({ 0.02, 0.03, 0.22, 0.12 }), bottom = diff ({ 0.10, 0.82, 0.80, 0.14 }), centre = diff ({ 0.35, 0.35, 0.30, 0.25 });
                        expect (tc > centre * 2.0 + 2.0 && bottom > centre * 2.0 + 2.0, "burn-in: timecode e battuta visibili sul fotogramma (differenza " + juce::String (tc, 1) + " / " + juce::String (bottom, 1) + ", centro " + juce::String (centre, 1) + ")");
                    }
                    else
                        expect (false, "burn-in: miniature confrontabili");
                    wavB.deleteFile(); movP.deleteFile(); movB.deleteFile();
                }

                // offset e altezze tracce sopravvivono a salvataggio e caricamento
                project->setVideoOffset (0.123);
                project->getSession().tracks[0].live->height = 150;
                auto tmp3 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_offset.adb");
                juce::String err3;
                project->save (tmp3, err3);
                project3 = std::make_unique<fc::Project> (formats, 48000.0);
                project3->loadSession (tmp3, [this] (juce::String e3)
                {
                    auto v3 = project3->getSession().video;
                    expect (e3.isEmpty() && v3 != nullptr && std::abs (v3->offsetSec - 0.123) < 1.0e-9
                                && project3->getSession().tracks[0].live->height.load() == 150,
                            "offset video e altezza traccia persistono nella sessione");
                    finishCb (fails);
                });
                return;
            });
        });
    }

    juce::File videoFile, pluginFile;
    juce::StringArray files;
    juce::AudioFormatManager formats;
    fc::AudioEngine hostEngine;
    fc::PluginHost host { hostEngine };
    std::unique_ptr<fc::Project> project, reload, project2, project3, project4;
};

class StudioApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "ADR Dub Studio"; }
    const juce::String getApplicationVersion() override { return "2.10.1"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        if (commandLine.contains ("--selftest"))
        {
            auto args = juce::StringArray::fromTokens (commandLine.fromFirstOccurrenceOf ("--selftest", false, false), true);
            selfTest = std::make_unique<SelfTest> (args);
            selfTest->run ([this] (int fails)
            {
                std::cout << (fails == 0 ? "TUTTO OK" : "TEST FALLITI: " + juce::String (fails)) << std::endl;
                setApplicationReturnValue (fails == 0 ? 0 : 1);
                quit();
            });
            return;
        }
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);
        fc::lang::apply (fc::lang::savedCode());   // lingua dell'interfaccia (prima di creare le finestre)
        fc::ThemeManager::applySaved();   // tema scelto l'ultima volta (prima di creare la finestra)
        window = std::make_unique<MainWindow> (getApplicationName());
        window->main().importFromCommandLine (juce::StringArray::fromTokens (commandLine, true));
    }

    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr) window->main().requestQuit(); else quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window != nullptr) window->main().importFromCommandLine (juce::StringArray::fromTokens (commandLine, true));
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, fc::theme::bg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            auto* c = new fc::MainComponent();
            c->onTitleChanged = [this] (const juce::String& t) { setName (t); };
            setContentOwned (c, true);
            setResizable (true, true);
            setResizeLimits (1400, 700, 6000, 4000);
            centreWithSize (1440, 900);
#if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu (c);   // menu nella barra di macOS
#else
            setMenuBar (c);                           // Windows e Linux: menu dentro la finestra
#endif
            setVisible (true);
        }

#if JUCE_MAC
        ~MainWindow() override { juce::MenuBarModel::setMacMainMenu (nullptr); }
#else
        ~MainWindow() override { setMenuBar (nullptr); }
#endif

        fc::MainComponent& main() { return *static_cast<fc::MainComponent*> (getContentComponent()); }
        void closeButtonPressed() override { main().requestQuit(); }
    };

    fc::theme::StudioLookAndFeel laf;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<SelfTest> selfTest;
};

START_JUCE_APPLICATION (StudioApplication)
