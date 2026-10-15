#include "AudioEngine.h"

namespace fc
{
void runInserts (const TrackLive& live, juce::AudioBuffer<float>& buf, int n, juce::MidiBuffer& midi)
{
    auto chain = live.tryGetChain();
    if (chain == nullptr) return;
    for (auto& slot : chain->slots)
    {
        if (! slot->ready.load() || slot->bypass.load() || slot->plugin == nullptr) continue;
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
        midi.clear();
        slot->plugin->processBlock (view, midi);
    }
}

int insertsLatency (const TrackLive& live, bool realtime)
{
    auto chain = realtime ? live.tryGetChain() : live.getChain();
    if (chain == nullptr) return 0;
    int n = 0;
    for (auto& slot : chain->slots)
        if (slot->ready.load() && ! slot->bypass.load() && slot->plugin != nullptr)
            n += juce::jmax (0, slot->plugin->getLatencySamples());
    return n;
}

int Mixer::computeLatency (const Session& s)
{
    int m = 0, a = 0;
    for (auto& t : s.tracks) m = juce::jmax (m, insertsLatency (*t.live, false) + (t.live->limiterOn.load() ? Limiter::latencyFor (s.sampleRate) : 0));
    for (auto& x : s.aux) a = juce::jmax (a, insertsLatency (*x, false));
    for (auto& x : s.group) a = juce::jmax (a, insertsLatency (*x, false));
    return m + a;
}

void MasterBus::process (TrackLive& master, juce::AudioBuffer<float>& buf, int n)
{
    runInserts (master, buf, n, midi);

    const float mg = dbToGain (master.volumeDb.load());
    if (lastGain < 0.0f) lastGain = mg;
    buf.applyGainRamp (0, 0, n, lastGain, mg);
    buf.applyGainRamp (1, 0, n, lastGain, mg);
    lastGain = mg;

    if (master.limiterOn.load())
    {
        if (! limiterWasOn) { limiter.reset(); limiterWasOn = true; }
        const float gr = limiter.process (buf, 0, n, dbToGain (master.ceilingDb.load()));
        if (gr > master.grDb.load()) master.grDb.store (gr);
    }
    else
        limiterWasOn = false;

    const float pl = buf.getMagnitude (0, 0, n), pr = buf.getMagnitude (1, 0, n);
    if (pl > master.peakL.load()) master.peakL.store (pl);
    if (pr > master.peakR.load()) master.peakR.store (pr);
}

//==============================================================================
void Mixer::renderClip (const Clip& c, int64_t pos, int n)
{
    const int64_t cs = juce::jmax (pos, c.start);
    int64_t ce = juce::jmin (pos + n, c.end());
    if (c.asset == nullptr || cs >= ce) return;

    const int64_t srcOff = c.offset + (cs - c.start);
    ce = juce::jmin (ce, cs + (c.asset->length() - srcOff));
    const int cnt = (int) (ce - cs);
    if (cnt <= 0) return;

    const int dstOff = (int) (cs - pos);
    const int nch = c.asset->numChannels();
    const float gain = dbToGain (c.gainDb);
    const int64_t inClip0 = cs - c.start;

    const bool needsFade = (c.fadeIn > 0 && inClip0 < c.fadeIn)
                        || (c.fadeOut > 0 && inClip0 + cnt > c.length - c.fadeOut)
                        || (c.xfadeIn > 0 && inClip0 < c.xfadeIn)
                        || (c.xfadeOut > 0 && inClip0 + cnt > c.length - c.xfadeOut);

    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = c.asset->audio.getReadPointer (juce::jmin (ch, nch - 1)) + srcOff;
        float* dst = scratch.getWritePointer (ch) + dstOff;

        if (! needsFade)
        {
            juce::FloatVectorOperations::addWithMultiply (dst, src, gain, cnt);
            continue;
        }

        for (int i = 0; i < cnt; ++i)
        {
            const int64_t ic = inClip0 + i;
            float g = gain;
            if (c.fadeIn > 0 && ic < c.fadeIn) g *= (float) ic / (float) c.fadeIn;
            const int64_t toEnd = c.length - ic;
            if (c.fadeOut > 0 && toEnd < c.fadeOut) g *= (float) toEnd / (float) c.fadeOut;
            // crossfade con il clip adiacente: guadagno costante se e' lo stesso materiale, potenza costante altrimenti
            if (c.xfadeIn > 0 && ic < c.xfadeIn)
            {
                const float x = (float) ic / (float) c.xfadeIn;
                g *= c.xLinear ? x : std::sin (x * juce::MathConstants<float>::halfPi);
            }
            if (c.xfadeOut > 0 && toEnd < c.xfadeOut)
            {
                const float x = (float) toEnd / (float) c.xfadeOut;
                g *= c.xLinear ? x : std::sin (x * juce::MathConstants<float>::halfPi);
            }
            dst[i] += src[i] * g;
        }
    }
}

void Mixer::render (const Session& s, int64_t pos, juce::AudioBuffer<float>& out, int outStart, int n)
{
    if (lastGains.size() < s.tracks.size())
        lastGains.resize (s.tracks.size(), { -1.0f, -1.0f }); // -1 = prima volta: parti dal valore di destinazione

    bool anySolo = false;
    for (auto& t : s.tracks)
        if (t.live->solo.load()) { anySolo = true; break; }

    // latenza dei plug-in di ogni traccia (anche delle tracce mute, cosi' l'allineamento non cambia con mute/solo)
    latencies.resize (s.tracks.size());
    int trackMax = 0, auxD = 0, auxLat[2] { 0, 0 }, grpLat[Session::kNumGroups] {};   // auxD = latenza del piu' lento tra ritorni FX e bus di gruppo
    if (pdcEnabled)
    {
        for (size_t i = 0; i < s.tracks.size(); ++i)
        {
            latencies[i] = insertsLatency (*s.tracks[i].live, true) + (s.tracks[i].live->limiterOn.load() ? Limiter::latencyFor (s.sampleRate) : 0);
            trackMax = juce::jmax (trackMax, latencies[i]);
        }
        for (int k = 0; k < 2; ++k) { auxLat[k] = insertsLatency (*s.aux[k], true); auxD = juce::jmax (auxD, auxLat[k]); }
        for (int k = 0; k < Session::kNumGroups; ++k) { grpLat[k] = insertsLatency (*s.group[k], true); auxD = juce::jmax (auxD, grpLat[k]); }
    }
    else
        std::fill (latencies.begin(), latencies.end(), 0);
    maxLatency = trackMax + auxD;
    if (lastSend.size() < s.tracks.size()) lastSend.resize (s.tracks.size(), { -1.0f, -1.0f });

    int done = 0;
    while (done < n)
    {
        const int chunk = juce::jmin (n - done, kChunk);
        const int64_t cpos = pos + done;
        out.clear (outStart + done, chunk);
        auxBuf[0].clear (0, chunk);
        auxBuf[1].clear (0, chunk);
        for (auto& gb : groupBuf) gb.clear (0, chunk);

        for (size_t ti = 0; ti < s.tracks.size(); ++ti)
        {
            const auto& t = s.tracks[ti];
            auto& live = *t.live;
            const bool audible = auditionClip > 0 || (! live.mute.load() && (! anySolo || live.solo.load()));

            scratch.clear (0, chunk);
            scratch.clear (1, 0, chunk);
            if (audible)
                for (auto& c : t.clips)
                    if ((auditionClip > 0 ? c.id == auditionClip : (! c.muted && ! (auditionClip < 0 && c.cueId != 0))) && c.end() > cpos && c.start < cpos + chunk)
                        renderClip (c, cpos, chunk);

            if (audible)   // guadagno in ingresso (a rampa): alimenta EQ e compressore al livello giusto
            {
                const float tgt = dbToGain (live.trimDb.load());
                if (live.trimGainLast < 0.0f) live.trimGainLast = tgt;
                if (live.trimGainLast != 1.0f || tgt != 1.0f)
                {
                    scratch.applyGainRamp (0, 0, chunk, live.trimGainLast, tgt);
                    scratch.applyGainRamp (1, 0, chunk, live.trimGainLast, tgt);
                }
                live.trimGainLast = tgt;
                const float ml = scratch.getMagnitude (0, 0, chunk), mr = scratch.getMagnitude (1, 0, chunk);   // livello dopo il guadagno
                if (ml > live.meterL.load()) live.meterL.store (ml);
                if (mr > live.meterR.load()) live.meterR.store (mr);
            }
            if (audible) live.eq.process (scratch.getWritePointer (0), scratch.getWritePointer (1), chunk, s.sampleRate);   // EQ, compressore, poi plug-in
            if (audible) live.comp.process (scratch.getWritePointer (0), scratch.getWritePointer (1), chunk, s.sampleRate);
            if (audible) runInserts (live, scratch, chunk, midi);   // plug-in pre-fader

            // compensazione di latenza: ritarda la traccia fino alla latenza della piu' lenta
            const int delay = audible && pdcEnabled ? juce::jmin (trackMax - latencies[ti], TrackLive::kPdcCap - 1) : 0;
            if (delay > 0)
                for (int c = 0; c < 2; ++c)
                {
                    float* data = scratch.getWritePointer (c);
                    float* ring = live.pdcBuf[c].data();
                    int wp = live.pdcPos;
                    for (int i = 0; i < chunk; ++i)
                    {
                        ring[wp] = data[i];
                        int rp = wp - delay;
                        if (rp < 0) rp += TrackLive::kPdcCap;
                        data[i] = ring[rp];
                        if (++wp == TrackLive::kPdcCap) wp = 0;
                    }
                    if (c == 1) live.pdcPos = wp;
                }

            // fader (con rampa) e bilanciamento
            // il volume segue l'automazione se e' attiva e ha dei punti, altrimenti il fader
            float volDb = live.volumeDb.load();
            if (live.autoMode.load() > 0 && ! t.volAuto.empty()) volDb = evalAutomationDb (t.volAuto, cpos + chunk);
            const float vol = audible ? dbToGain (volDb) * (live.invert.load() ? -1.0f : 1.0f) : 0.0f;
            float panV = live.pan.load();
            if (live.autoMode.load() > 0 && ! t.panAuto.empty()) panV = evalAutomationDb (t.panAuto, cpos + chunk);
            const float pan = juce::jlimit (-1.0f, 1.0f, panV);
            const float tgtL = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
            const float tgtR = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
            auto& last = lastGains[ti];
            if (last[0] < 0.0f) last = { tgtL, tgtR };
            scratch.applyGainRamp (0, 0, chunk, last[0], tgtL);
            scratch.applyGainRamp (1, 0, chunk, last[1], tgtR);
            last = { tgtL, tgtR };

            // limiter di traccia (dopo il fader): nessun picco della traccia supera il tetto
            if (audible && live.limiterOn.load())
            {
                if (! live.trackLimiterActive || live.trackLimiterRate != s.sampleRate)
                {
                    live.trackLimiter.prepare (s.sampleRate);
                    live.trackLimiterRate = s.sampleRate;
                    live.trackLimiterActive = true;
                }
                const float gr = live.trackLimiter.process (scratch, 0, chunk, dbToGain (live.ceilingDb.load()));
                if (gr > live.grDb.load()) live.grDb.store (gr);
            }
            else
                live.trackLimiterActive = false;

            const float pl = scratch.getMagnitude (0, 0, chunk), pr = scratch.getMagnitude (1, 0, chunk);
            if (pl > live.peakL.load()) live.peakL.store (pl);
            if (pr > live.peakR.load()) live.peakR.store (pr);

            // send verso i ritorni FX (post-fader, a rampa)
            for (int k = 0; k < 2; ++k)
            {
                float sd = live.sendDb[k].load();
                if (live.autoMode.load() > 0) { const auto& lane = k == 0 ? t.sendAAuto : t.sendBAuto; if (! lane.empty()) sd = evalAutomationDb (lane, cpos + chunk); }
                const float tgt = (audible && sd > -59.5f) ? dbToGain (sd) : 0.0f;
                float& last = lastSend[ti][(size_t) k];
                if (last < 0.0f) last = tgt;
                if (last != 0.0f || tgt != 0.0f)
                {
                    auxBuf[k].addFromWithRamp (0, 0, scratch.getReadPointer (0), chunk, last, tgt);
                    auxBuf[k].addFromWithRamp (1, 0, scratch.getReadPointer (1), chunk, last, tgt);
                }
                last = tgt;
            }

            const int bus = live.outBus.load();
            if (bus > 0 && bus <= Session::kNumGroups)   // uscita su un bus di gruppo: il mix finale lo somma dopo
            {
                groupBuf[bus - 1].addFrom (0, 0, scratch, 0, 0, chunk);
                groupBuf[bus - 1].addFrom (1, 0, scratch, 1, 0, chunk);
            }
            else
            {
                out.addFrom (0, outStart + done, scratch, 0, 0, chunk);
                out.addFrom (1, outStart + done, scratch, 1, 0, chunk);
            }
        }

        // il segnale secco viene ritardato come il ritorno FX piu' lento
        if (auxD > 0)
            for (int c = 0; c < 2; ++c)
            {
                float* data = out.getWritePointer (c) + outStart + done;
                float* ring = dryRing[(size_t) c].data();
                int wp = dryPos;
                for (int i = 0; i < chunk; ++i)
                {
                    ring[wp] = data[i];
                    int rp = wp - auxD;
                    if (rp < 0) rp += TrackLive::kPdcCap;
                    data[i] = ring[rp];
                    if (++wp == TrackLive::kPdcCap) wp = 0;
                }
                if (c == 1) dryPos = wp;
            }

        // ritorni FX e bus di gruppo: inserti, fader, mute, allineamento e somma al mix
        auto finishReturn = [&] (TrackLive& live, juce::AudioBuffer<float>& buf, float& lastGain, int latency)
        {
            runInserts (live, buf, chunk, midi);

            const float tgt = live.mute.load() ? 0.0f : dbToGain (live.volumeDb.load());
            if (lastGain < 0.0f) lastGain = tgt;
            buf.applyGainRamp (0, 0, chunk, lastGain, tgt);
            buf.applyGainRamp (1, 0, chunk, lastGain, tgt);
            lastGain = tgt;

            const float pl = buf.getMagnitude (0, 0, chunk), pr = buf.getMagnitude (1, 0, chunk);
            if (pl > live.peakL.load()) live.peakL.store (pl);
            if (pr > live.peakR.load()) live.peakR.store (pr);

            const int delay = pdcEnabled ? juce::jmin (auxD - latency, TrackLive::kPdcCap - 1) : 0;
            if (delay > 0)
                for (int c = 0; c < 2; ++c)
                {
                    float* data = buf.getWritePointer (c);
                    float* ring = live.pdcBuf[c].data();
                    int wp = live.pdcPos;
                    for (int i = 0; i < chunk; ++i)
                    {
                        ring[wp] = data[i];
                        int rp = wp - delay;
                        if (rp < 0) rp += TrackLive::kPdcCap;
                        data[i] = ring[rp];
                        if (++wp == TrackLive::kPdcCap) wp = 0;
                    }
                    if (c == 1) live.pdcPos = wp;
                }

            out.addFrom (0, outStart + done, buf, 0, 0, chunk);
            out.addFrom (1, outStart + done, buf, 1, 0, chunk);
        };
        for (int k = 0; k < 2; ++k) finishReturn (*s.aux[(size_t) k], auxBuf[k], auxLastGain[k], auxLat[k]);
        for (int k = 0; k < Session::kNumGroups; ++k) finishReturn (*s.group[(size_t) k], groupBuf[k], groupLastGain[k], grpLat[k]);
        done += chunk;
    }
}

bool renderMixdown (const Session& source, double rate, const juce::File& file,
                    const std::function<bool (double)>& progress, juce::String& message, int64_t from, int64_t to)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto* format = fm.findFormatForFileExtension (file.getFileExtension());
    if (format == nullptr) { message = "Formato di esportazione non supportato."; return false; }

    Session session = source.cloneDetached();
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
    if (out == nullptr) { message = "Impossibile creare " + file.getFullPathName(); return false; }
    std::unique_ptr<juce::AudioFormatWriter> writer (format->createWriterFor (out.get(), rate, 2, 24, {}, 0));
    if (writer == nullptr) { message = "Impossibile creare il writer audio."; return false; }
    out.release();

    // i plug-in lavorano in modalita' offline (qualita' massima, niente vincoli di tempo reale)
    std::vector<std::shared_ptr<InsertSlot>> plugins;
    auto collect = [&plugins] (const TrackLive& l)
    {
        if (auto c = l.getChain()) for (auto& sl : c->slots) if (sl->ready.load() && sl->plugin != nullptr) plugins.push_back (sl);
    };
    for (auto& t : session.tracks) collect (*t.live);
    collect (*session.master);
    for (auto& x : session.aux) collect (*x);
    for (auto& x : session.group) collect (*x);
    for (auto& sl : plugins) { sl->plugin->reset(); sl->plugin->setNonRealtime (true); }

    Mixer mixer;
    MasterBus bus;
    bus.prepare (rate);
    juce::AudioBuffer<float> buf (2, Mixer::kChunk);

    const int skip = Mixer::computeLatency (session) + bus.latency (*session.master);   // plug-in e limiter ritardano: si compensa
    const bool ranged = to > from;
    const int64_t startPos = ranged ? juce::jmax<int64_t> (0, from) : 0;
    const int64_t totalOut = ranged ? to - startPos : session.endSample() + (int64_t) (0.5 * rate);
    const int64_t total = totalOut + skip;
    float peak = 0.0f;
    bool cancelled = false;

    for (int64_t pos = 0; pos < total; pos += Mixer::kChunk)
    {
        const int n = (int) juce::jmin<int64_t> (Mixer::kChunk, total - pos);
        mixer.render (session, startPos + pos, buf, 0, n);
        bus.process (*session.master, buf, n);

        const int first = (int) juce::jmax<int64_t> (0, (int64_t) skip - pos);
        if (first < n)
        {
            peak = juce::jmax (peak, buf.getMagnitude (first, n - first));
            writer->writeFromAudioSampleBuffer (buf, first, n - first);
        }
        if (progress && ! progress ((double) pos / (double) total)) { cancelled = true; break; }
    }
    writer.reset();
    for (auto& sl : plugins) { sl->plugin->setNonRealtime (false); sl->plugin->reset(); }

    if (cancelled)
    {
        file.deleteFile();
        message = "Esportazione annullata.";
        return false;
    }

    message = "Esportato in " + file.getFullPathName() + "\nPicco: "
              + juce::String (juce::Decibels::gainToDecibels (peak, -120.0f), 1) + " dBFS"
              + (peak > 1.0f ? " (attenzione: sopra 0 dBFS, abbassa i fader o il master)" : "");
    return true;
}

//==============================================================================
AudioEngine::AudioEngine() = default;

bool AudioEngine::ensureInput()
{
    if (numInputs.load() > 0) return true;
    auto setup = deviceManager.getAudioDeviceSetup();
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr) return false;

    if (setup.inputDeviceName.isEmpty())
    {
        const auto names = type->getDeviceNames (true);
        // preferisci l'ingresso con lo stesso nome dell'uscita (stessa scheda audio), altrimenti il predefinito
        int idx = names.indexOf (setup.outputDeviceName);
        if (idx < 0) idx = type->getDefaultDeviceIndex (true);
        if (idx < 0 || idx >= names.size()) return false;
        setup.inputDeviceName = names[idx];
    }
    setup.useDefaultInputChannels = true;
    const auto err = deviceManager.setAudioDeviceSetup (setup, true);
    return err.isEmpty() && numInputs.load() > 0;
}

juce::String AudioEngine::startRecording (const std::vector<TakeSpec>& specs)
{
    if (recording.load()) return "Registrazione gia' in corso.";
    if (specs.empty()) return "Nessuna traccia armata.";
    const int nIn = numInputs.load();
    if (nIn <= 0) return "Nessun ingresso audio attivo: controlla Impostazioni audio e il permesso del microfono.";

    if (! writerThread.isThreadRunning()) writerThread.startThread();

    juce::WavAudioFormat wav;
    const double rate = sampleRate.load();
    const int64_t latency = recordLatency();

    std::vector<std::unique_ptr<Take>> made;
    for (auto& sp : specs)
    {
        auto t = std::make_unique<Take>();
        t->trackId = sp.trackId;
        t->file = sp.file;
        t->latency = latency;
        t->srcA = sp.inputMode == 1 ? juce::jmin (1, nIn - 1) : 0;
        t->srcB = (sp.inputMode == 2 && nIn > 1) ? 1 : -1;
        const int nch = t->srcB >= 0 ? 2 : 1;

        sp.file.getParentDirectory().createDirectory();
        sp.file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (sp.file.createOutputStream());
        if (os == nullptr) return "Impossibile creare " + sp.file.getFullPathName();
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, (unsigned) nch, 24, {}, 0));
        if (w == nullptr) return "Impossibile creare il file WAV di registrazione.";
        os.release();
        t->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), writerThread, 262144);
        made.push_back (std::move (t));
    }

    dropped = 0;
    recStart = -1;
    {
        const juce::SpinLock::ScopedLockType l (recLock);
        takes = std::move (made);
    }
    recording = true;
    return {};
}

int64_t AudioEngine::recordLatency() const
{
    int64_t latency = 0;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        latency = (int64_t) dev->getInputLatencyInSamples() + (int64_t) dev->getOutputLatencyInSamples();
    return latency + pdcLatency.load();   // l'esecutore ha suonato sentendo il mix in ritardo dei plug-in
}

std::vector<TakeResult> AudioEngine::stopRecording()
{
    recording = false;
    std::vector<std::unique_ptr<Take>> old;
    {
        const juce::SpinLock::ScopedLockType l (recLock);
        old = std::move (takes);
        takes.clear();
    }

    std::vector<TakeResult> results;
    for (auto& t : old)
        results.push_back ({ t->trackId, t->file, juce::jmax<int64_t> (0, t->startPos), t->latency, t->written.load() });
    old.clear();   // svuota il FIFO e chiude i file (sul thread messaggi)
    recStart = -1;

    std::vector<TakeResult> valid;
    for (auto& r : results)
        if (r.length > 0) valid.push_back (r); else r.file.deleteFile();
    return valid;
}

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::initialise (const juce::XmlElement* savedState)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.sampleRate = 48000.0;
    const auto err = deviceManager.initialise (0, 2, savedState, true, {}, &setup);  // gli ingressi si aprono alla prima registrazione
    deviceManager.addAudioCallback (this);
    return err.isEmpty();
}

void AudioEngine::shutdown()
{
    deviceManager.removeAudioCallback (this);
    writerThread.stopThread (2000);
    deviceManager.closeAudioDevice();
}

void AudioEngine::publish (std::shared_ptr<const Session> s)
{
    const juce::SpinLock::ScopedLockType l (snapLock);
    pending = std::move (s);   // l'eventuale vecchio snapshot viene liberato qui, sul thread messaggi
    hasPending = true;
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();
    sampleRate = rate;
    masterBus.prepare (rate);
    loudness.prepare (rate);
    busNeedsReset = true;
    numInputs = device->getActiveInputChannels().countNumberOfSetBits();
    playGain = 0.0f;
    juce::MessageManager::callAsync ([this, rate]
    {
        if (onSampleRateChanged) onSampleRateChanged (rate);
    });
}

void AudioEngine::audioDeviceStopped() {}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], numSamples);

    {
        const juce::SpinLock::ScopedTryLockType l (snapLock);
        if (l.isLocked() && hasPending)
        {
            std::swap (current, pending);
            hasPending = false;
        }
    }

    if (numIn > 0 && in != nullptr && in[0] != nullptr)
    {
        const float pl = juce::FloatVectorOperations::findMinAndMax (in[0], numSamples).getEnd();
        const float nl = -juce::FloatVectorOperations::findMinAndMax (in[0], numSamples).getStart();
        float pr = 0.0f, nr = 0.0f;
        if (numIn > 1 && in[1] != nullptr)
        {
            auto r = juce::FloatVectorOperations::findMinAndMax (in[1], numSamples);
            pr = r.getEnd(); nr = -r.getStart();
        }
        else { pr = pl; nr = nl; }
        const float l = juce::jmax (pl, nl), r2 = juce::jmax (pr, nr);
        if (l > inPeakL.load()) inPeakL.store (l);
        if (r2 > inPeakR.load()) inPeakR.store (r2);
    }

    // misuratore delle tracce armate: livello dell'ingresso dopo il guadagno, anche a registrazione ferma
    if (current != nullptr && numIn > 0 && in != nullptr)
        for (auto& t : current->tracks)
        {
            auto& live = *t.live;
            if (! live.armed.load()) continue;
            const int mode = live.inputMode.load();
            const int a = mode == 1 ? juce::jmin (1, numIn - 1) : 0;
            const int b = (mode == 2 && numIn > 1) ? 1 : a;
            if (in[a] == nullptr || in[b] == nullptr) continue;
            const float g = dbToGain (live.trimDb.load());
            const float pl = juce::FloatVectorOperations::findMinAndMax (in[a], numSamples).getEnd() * g;
            const float nl = -juce::FloatVectorOperations::findMinAndMax (in[a], numSamples).getStart() * g;
            const float pr = juce::FloatVectorOperations::findMinAndMax (in[b], numSamples).getEnd() * g;
            const float nr = -juce::FloatVectorOperations::findMinAndMax (in[b], numSamples).getStart() * g;
            const float l = juce::jmax (pl, nl), r = juce::jmax (pr, nr);
            if (l > live.meterL.load()) live.meterL.store (l);
            if (r > live.meterR.load()) live.meterR.store (r);
            // anche il misuratore della strip nel mixer mostra l'ingresso della traccia armata, a registrazione ferma
            if (l > live.peakL.load()) live.peakL.store (l);
            if (r > live.peakR.load()) live.peakR.store (r);
        }

    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin (numSamples - done, Mixer::kChunk);
        processChunk (n, in, numIn, done);

        if (numOut == 1)
        {
            for (int i = 0; i < n; ++i)
                out[0][done + i] = 0.5f * (master.getSample (0, i) + master.getSample (1, i));
        }
        else
        {
            for (int c = 0; c < juce::jmin (2, numOut); ++c)
                if (out[c] != nullptr) juce::FloatVectorOperations::copy (out[c] + done, master.getReadPointer (c), n);
        }
        done += n;
    }
}

void AudioEngine::processChunk (int n, const float* const* in, int numIn, int inOffset)
{
    int64_t pos = position.load();
    const auto seek = pendingSeek.exchange (-1);
    if (seek >= 0) { pos = seek; position.store (pos); }

    if (current == nullptr) { master.clear (0, n); return; }

    const bool wantPlay = playing.load();
    if (! wantPlay && playGain <= 0.0f)
    {
        master.clear (0, n);
        busNeedsReset = true;
        return;
    }
    if (busNeedsReset) { masterBus.reset(); busNeedsReset = false; }

    const float target = wantPlay ? 1.0f : 0.0f;
    const float step = (float) n / 256.0f;
    const float newPlayGain = playGain < target ? juce::jmin (target, playGain + step)
                                                : juce::jmax (target, playGain - step);

    // registrazione: i campioni d'ingresso di questo blocco cadono sulla posizione 'pos' della timeline
    if (wantPlay && recording.load() && numIn > 0 && in != nullptr)
    {
        const juce::SpinLock::ScopedTryLockType l (recLock);
        if (l.isLocked())
            for (auto& t : takes)
            {
                if (t->srcA >= numIn || in[t->srcA] == nullptr) continue;
                const float* ch[2] = { in[t->srcA] + inOffset,
                                       (t->srcB >= 0 && t->srcB < numIn && in[t->srcB] != nullptr) ? in[t->srcB] + inOffset : in[t->srcA] + inOffset };
                // punch automatico: si scrive solo la parte del blocco che cade tra ingresso e uscita
                int from = 0, to = n;
                const int64_t pin = punchIn.load(), pout = punchOut.load();
                if (pin >= 0)
                {
                    from = (int) juce::jlimit<int64_t> (0, n, pin - pos);
                    to = (int) juce::jlimit<int64_t> (0, n, pout - pos);
                }
                if (to <= from) continue;
                const float* part[2] = { ch[0] + from, ch[1] + from };
                if (t->startPos < 0)
                {
                    t->startPos = pos + from;
                    int64_t expected = -1;
                    recStart.compare_exchange_strong (expected, pos + from);
                }
                if (t->writer->write (part, to - from)) t->written += (to - from); else ++dropped;
            }
    }

    const bool looping = loopOn.load();
    const int64_t ls = loopStart.load(), le = loopEnd.load();
    const int64_t chunkPos = pos;

    int remaining = n, outPos = 0;
    while (remaining > 0)
    {
        mixer.setPdcEnabled (pdcOn.load());
        int seg = remaining;
        if (looping && pos < le) seg = (int) juce::jmin<int64_t> (seg, le - pos);
        mixer.auditionClip = auditionClip.load();
        mixer.render (*current, pos, master, outPos, seg);
        pos += seg;
        outPos += seg;
        remaining -= seg;
        if (looping && pos >= le) pos = ls;
    }
    position.store (pos);

    // anti-click all'avvio/arresto
    master.applyGainRamp (0, 0, n, playGain, newPlayGain);
    master.applyGainRamp (1, 0, n, playGain, newPlayGain);
    playGain = newPlayGain;

    // ascolto dell'ingresso durante la registrazione
    if (wantPlay && recording.load() && monitorInput.load() && numIn > 0 && in != nullptr)
    {
        const juce::SpinLock::ScopedTryLockType l (recLock);
        if (l.isLocked())
            for (auto& t : takes)
            {
                if (t->srcA >= numIn || in[t->srcA] == nullptr) continue;
                const float* a = in[t->srcA] + inOffset;
                const float* b = (t->srcB >= 0 && t->srcB < numIn && in[t->srcB] != nullptr) ? in[t->srcB] + inOffset : a;
                juce::FloatVectorOperations::add (master.getWritePointer (0), a, n);
                juce::FloatVectorOperations::add (master.getWritePointer (1), b, n);
            }
    }

    // master: inserti, fader, limiter, misuratori
    masterBus.process (*current->master, master, n);
    pdcLatency = mixer.getMaxLatency() + masterBus.latency (*current->master, true);
    loudness.process (master.getReadPointer (0), master.getReadPointer (1), n);
    {
        uint32_t sp = specPos.load (std::memory_order_relaxed);
        const float* l = master.getReadPointer (0);
        const float* r = master.getReadPointer (1);
        for (int i = 0; i < n; ++i) specRing[(sp++) & (kSpecCap - 1)] = 0.5f * (l[i] + r[i]);
        specPos.store (sp, std::memory_order_release);
    }

    // bip di conteggio (ADR): solo in uscita, non entrano nelle misure ne' nelle registrazioni
    if (const int nb = beepCount.load(); nb > 0)
    {
        const double rate = sampleRate.load();
        const int len = (int) (rate * 0.1), fade = (int) (rate * 0.005);
        const int64_t shift = pdcLatency.load();   // il materiale si sente in ritardo di questo valore: il bip deve seguirlo
        float* l = master.getWritePointer (0);
        float* r = master.getWritePointer (1);
        for (int k = 0; k < nb; ++k)
        {
            const int64_t b = beepPos[k].load() + shift;
            if (b >= chunkPos + n || b + len <= chunkPos) continue;
            for (int i = 0; i < n; ++i)
            {
                const int64_t idx = chunkPos + i - b;
                if (idx < 0 || idx >= len) continue;
                float env = 1.0f;
                if (idx < fade) env = (float) idx / (float) fade;
                else if (idx > len - fade) env = (float) (len - idx) / (float) fade;
                const float v = 0.35f * env * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) idx / rate);
                l[i] += v;
                r[i] += v;
            }
        }
    }

    // controllo d'ascolto (solo uscita, non misure ne' export)
    if (current->master->monoMon.load())
    {
        float* l = master.getWritePointer (0);
        float* r = master.getWritePointer (1);
        for (int i = 0; i < n; ++i) l[i] = r[i] = 0.5f * (l[i] + r[i]);
    }
    if (current->master->dimMon.load()) master.applyGain (0, n, 0.1f);

    // arresto automatico a fine contenuto
    if (wantPlay && ! looping && ! recording.load() && pos > current->endSample() + (int64_t) sampleRate.load())
        playing = false;
}

} // namespace fc
