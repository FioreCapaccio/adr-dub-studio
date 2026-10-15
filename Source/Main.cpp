#include "MainComponent.h"
#include <iostream>
#include "VideoSupport.h"
#include "AdrImport.h"
#include "TimeStretch.h"
#include "ExportEngine.h"
#include "BuiltinFx.h"

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
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest.arostudio");
        juce::String err;
        project->getSession().group[2]->volumeDb = -3.0f;   // gruppi: i parametri e l'instradamento si salvano (un gruppo a 0 dB non cambia il mix)
        project->getSession().tracks[0].live->outBus = 4;
        project->exportIn = 12000; project->exportOut = 96000; project->adrDialogueSize = 44;
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
            stepStretch ([this] { stepPlugin ([this] { stepRecording(); }); });
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
        fc::applyTrackPreset (l, fc::TrackMusic);
        expect (l.trackType.load() == fc::TrackMusic && std::abs (l.volumeDb.load() + 6.0f) < 0.01f && std::abs (l.comp.ratio.load() - 2.0f) < 0.01f, "preset musica: livello -6 dB, compressore 2:1");
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
        p.setTrackType (p.getSession().tracks[0].id, fc::TrackMusic);
        expect (p.getSession().tracks[0].live->trackType.load() == fc::TrackMusic && p.getSession().tracks[0].live->comp.enabled.load(), "setTrackType applica la catena alla traccia");
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
          fx->reset();
          run (*fx, sine (300.0, 0.5f, 48000), o);   const float loud = peak (o, 24000, 48000);
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
          mb->reset();
          run (*mb, sine (8000.0, 0.5f, 24000), o); const float highOut = amp (o, 12000, 24000);
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

            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_plugin.arostudio");
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
        project->setVideo (videoFile, [this, tracksBefore] (juce::String msg)
        {
            auto v = project->getSession().video;
            expect (msg.isEmpty() && v != nullptr, "video caricato " + msg);
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
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_video.arostudio");
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

                // offset e altezze tracce sopravvivono a salvataggio e caricamento
                project->setVideoOffset (0.123);
                project->getSession().tracks[0].live->height = 150;
                auto tmp3 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("selftest_offset.arostudio");
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
    const juce::String getApplicationVersion() override { return "1.7.0"; }
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
