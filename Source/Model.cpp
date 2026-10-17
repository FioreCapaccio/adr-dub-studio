#include "Model.h"
#include "ExportEngine.h"
#include "AudioTools.h"
#include "TimeStretch.h"
#include "TrackPresets.h"
#include "Theme.h"
#include "VideoSupport.h"
#include <map>
#include <thread>

namespace fc
{
void retire (std::shared_ptr<const void> object)
{
    // il lambda tiene vivo l'oggetto per 3 secondi e lo rilascia sul thread messaggi
    juce::Timer::callAfterDelay (3000, [keep = std::move (object)] {});
}

//==============================================================================
void Asset::buildPeaks()
{
    levels.clear();
    const int n = audio.getNumSamples(), nch = audio.getNumChannels();
    const bool stereo = nch >= 2;

    PeakLevel l0;
    l0.block = 64;
    const int nb = juce::jmax (1, (n + 63) / 64);
    l0.mn.assign ((size_t) nb, 0.0f);
    l0.mx.assign ((size_t) nb, 0.0f);
    if (stereo) { l0.mn1.assign ((size_t) nb, 0.0f); l0.mx1.assign ((size_t) nb, 0.0f); }

    for (int b = 0; b < nb; ++b)
    {
        const int s0 = b * 64, s1 = juce::jmin (n, s0 + 64);
        if (s1 <= s0) break;
        auto r0 = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (0) + s0, s1 - s0);
        l0.mn[(size_t) b] = juce::jmin (0.0f, r0.getStart());
        l0.mx[(size_t) b] = juce::jmax (0.0f, r0.getEnd());
        if (stereo)
        {
            auto r1 = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (1) + s0, s1 - s0);
            l0.mn1[(size_t) b] = juce::jmin (0.0f, r1.getStart());
            l0.mx1[(size_t) b] = juce::jmax (0.0f, r1.getEnd());
        }
    }
    levels.push_back (std::move (l0));

    while (levels.back().mn.size() > 64)
    {
        const auto& p = levels.back();
        PeakLevel nx;
        nx.block = p.block * 8;
        const size_t cnt = (p.mn.size() + 7) / 8;
        nx.mn.assign (cnt, 0.0f);
        nx.mx.assign (cnt, 0.0f);
        if (stereo) { nx.mn1.assign (cnt, 0.0f); nx.mx1.assign (cnt, 0.0f); }
        for (size_t i = 0; i < p.mn.size(); ++i)
        {
            nx.mn[i / 8] = juce::jmin (nx.mn[i / 8], p.mn[i]);
            nx.mx[i / 8] = juce::jmax (nx.mx[i / 8], p.mx[i]);
            if (stereo)
            {
                nx.mn1[i / 8] = juce::jmin (nx.mn1[i / 8], p.mn1[i]);
                nx.mx1[i / 8] = juce::jmax (nx.mx1[i / 8], p.mx1[i]);
            }
        }
        levels.push_back (std::move (nx));
    }
}

AssetPtr loadAsset (juce::AudioFormatManager& fm, const juce::File& f, double rate, juce::String& error, const std::function<void (float)>& progress)
{
    if (video::isVideoFile (f))
    {
        // l'audio dei video (.mov compresi) si decodifica con AVFoundation: i lettori audio di JUCE non conoscono tutti i contenitori
        juce::AudioBuffer<float> native;
        juce::String e;
        if (video::decodeAudioNative (f, rate, native, progress, e) && native.getNumSamples() > 0)
        {
            auto a = std::make_shared<Asset>();
            a->file = f;
            a->name = f.getFileNameWithoutExtension();
            a->audio = std::move (native);
            a->buildPeaks();
            return a;
        }
    }

    std::unique_ptr<juce::AudioFormatReader> r;
    juce::File tempWav;
    if (video::usesExternalDecoder() && video::isVideoFile (f))
    {
        // senza decodificatori di sistema l'audio del video si estrae con ffmpeg in un WAV temporaneo
        tempWav = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("adr_audio", ".wav");
        juce::String e;
        if (progress) progress (0.1f);
        if (video::extractAudio (f, tempWav, e)) r.reset (fm.createReaderFor (tempWav));
        if (progress) progress (0.6f);
    }
    if (r == nullptr) r.reset (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0)
    {
        error = TRANS ("Formato non supportato o file vuoto: ") + f.getFileName();
        return nullptr;
    }
    if (r->lengthInSamples > 2000000000LL)
    {
        error = TRANS ("File troppo lungo: ") + f.getFileName();
        return nullptr;
    }

    const int nch = (int) juce::jmin (2u, r->numChannels);
    const int srcLen = (int) r->lengthInSamples;
    juce::AudioBuffer<float> src (nch, srcLen);
    for (int done = 0; done < srcLen;)   // a blocchi, per poter mostrare l'avanzamento
    {
        const int n = juce::jmin (1 << 20, srcLen - done);
        r->read (&src, done, n, done, true, nch > 1);
        done += n;
        if (progress) progress ((float) done / (float) srcLen);
    }
    const double srcRate = r->sampleRate;
    r.reset();
    if (tempWav != juce::File()) tempWav.deleteFile();

    auto a = std::make_shared<Asset>();
    a->file = f;
    a->name = f.getFileNameWithoutExtension();

    if (std::abs (srcRate - rate) > 0.5)
    {
        const double ratio = srcRate / rate;
        const int outLen = (int) std::ceil ((double) srcLen / ratio);
        a->audio.setSize (nch, outLen);
        for (int ch = 0; ch < nch; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, src.getReadPointer (ch), a->audio.getWritePointer (ch), outLen);
        }
    }
    else
    {
        a->audio = std::move (src);
    }

    a->buildPeaks();
    if (progress) progress (1.0f);
    return a;
}

//==============================================================================
Project::Project (juce::AudioFormatManager& fm, double rate) : formats (fm), sampleRate (rate)
{
    session.sampleRate = rate;
    for (int i = 0; i < 4; ++i)
        addTrackInternal();
    selectedTrackId = session.tracks.front().id;
}

Project::~Project() { *alive = false; }

void Project::setSampleRate (double newRate)
{
    if (std::abs (newRate - sampleRate) < 0.5) return;
    const double ratio = newRate / sampleRate;

    std::map<juce::String, AssetPtr> cache;
    auto remap = [&] (AssetPtr& a) -> bool
    {
        const auto key = a->file.getFullPathName();
        auto it = cache.find (key);
        if (it == cache.end())
        {
            if (a->offline)   // un file mancante resta mancante: si adatta solo la lunghezza ricordata
            {
                auto off = std::make_shared<Asset>();
                off->file = a->file; off->name = a->name; off->offline = true; off->offlineChannels = a->offlineChannels;
                off->offlineLength = (int64_t) std::llround ((double) a->offlineLength * ratio);
                it = cache.emplace (key, off).first;
            }
            else
            {
                juce::String err;
                it = cache.emplace (key, loadAsset (formats, a->file, newRate, err)).first;
            }
        }
        if (it->second == nullptr) return false;
        a = it->second;
        return true;
    };

    for (auto& t : session.tracks)
        for (auto it = t.clips.begin(); it != t.clips.end();)
        {
            if (! remap (it->asset)) { it = t.clips.erase (it); continue; }
            it->start = (int64_t) std::llround ((double) it->start * ratio);
            it->offset = (int64_t) std::llround ((double) it->offset * ratio);
            it->length = (int64_t) std::llround ((double) it->length * ratio);
            it->fadeIn = (int64_t) std::llround ((double) it->fadeIn * ratio);
            it->fadeOut = (int64_t) std::llround ((double) it->fadeOut * ratio);
            it->length = juce::jmin (it->length, it->asset->length() - it->offset);
            ++it;
        }

    for (auto& m : session.markers) m.pos = (int64_t) std::llround ((double) m.pos * ratio);
    for (auto& c : session.cues) { c.inPos = (int64_t) std::llround ((double) c.inPos * ratio); c.outPos = (int64_t) std::llround ((double) c.outPos * ratio); }
    for (auto& t : session.tracks) for (int pr = 0; pr < kNumAutoParams; ++pr) for (auto& ap : t.lane (pr)) ap.pos = (int64_t) std::llround ((double) ap.pos * ratio);
    sampleRate = newRate;
    undoStack.clear();
    redoStack.clear();
    changed();
}

//==============================================================================
void Project::beginEdit()
{
    undoStack.push_back (session);
    if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

// blocco, visibilita' e gruppi compressi sono stato di lavoro: annullare una modifica non li cambia
Project::ViewState Project::viewState() const
{
    ViewState v;
    for (auto& t : session.tracks) v.tracks[t.id] = { t.locked, t.hidden };
    for (auto& g : session.groups) v.collapsed[g.id] = g.collapsed;
    return v;
}

void Project::applyViewState (const ViewState& v)
{
    for (auto& t : session.tracks)
        if (auto it = v.tracks.find (t.id); it != v.tracks.end()) { t.locked = it->second.first; t.hidden = it->second.second; }
    for (auto& g : session.groups)
        if (auto it = v.collapsed.find (g.id); it != v.collapsed.end()) g.collapsed = it->second;
}

void Project::undo()
{
    if (undoStack.empty()) return;
    const auto view = viewState();
    redoStack.push_back (session);
    session = std::move (undoStack.back());
    undoStack.pop_back();
    applyViewState (view);
    changed();
}

void Project::redo()
{
    if (redoStack.empty()) return;
    const auto view = viewState();
    undoStack.push_back (session);
    session = std::move (redoStack.back());
    redoStack.pop_back();
    applyViewState (view);
    changed();
}

void Project::publish()
{
    if (onPublish)
    {
        session.sampleRate = sampleRate;
        auto copy = std::make_shared<Session> (session);
        copy->computeCrossfades();
        copy->computeLanes();
        onPublish (std::move (copy));
    }
}

void Project::pruneSelection()
{
    for (auto it = selection.begin(); it != selection.end();)
        if (findClip (*it) == nullptr) it = selection.erase (it); else ++it;
    if (trackIndexForId (selectedTrackId) < 0 && ! session.tracks.empty())
        selectedTrackId = session.tracks.front().id;
}

void Project::changed()
{
    dirty = true;
    pruneSelection();
    publish();
    sendChangeMessage();
}

void Project::liveChanged()
{
    if (juce::Time::getMillisecondCounterHiRes() > quietUntil) dirty = true;
    sendChangeMessage();
}

//==============================================================================
int Project::trackIndexForId (int id) const noexcept
{
    for (size_t i = 0; i < session.tracks.size(); ++i)
        if (session.tracks[i].id == id) return (int) i;
    return -1;
}

Track* Project::findTrack (int id) noexcept
{
    const int i = trackIndexForId (id);
    return i >= 0 ? &session.tracks[(size_t) i] : nullptr;
}

Clip* Project::findClip (int64_t id, int* trackIndex) noexcept
{
    for (size_t ti = 0; ti < session.tracks.size(); ++ti)
        for (auto& c : session.tracks[ti].clips)
            if (c.id == id)
            {
                if (trackIndex != nullptr) *trackIndex = (int) ti;
                return &c;
            }
    return nullptr;
}

//==============================================================================
int Project::addTrackInternal()
{
    Track t;
    t.id = nextTrackId++;
    t.name = "Audio " + juce::String ((int) session.tracks.size() + 1);
    t.colour = theme::trackPalette[(t.id - 1) % theme::numPaletteColours];
    t.live->limiterOn = false;   // il limiter di traccia e' opzionale (nel master e' acceso di default)
    session.tracks.push_back (std::move (t));
    return session.tracks.back().id;
}

int Project::addTrack()
{
    beginEdit();
    const int id = addTrackInternal();
    selectedTrackId = id;
    changed();
    return id;
}

int Project::addTrackAs (const juce::String& name, int type, juce::Colour colour, int outBus)
{
    beginEdit();
    const int id = addTrackInternal();
    if (auto* t = findTrack (id))
    {
        t->name = name;
        t->colour = colour;
        applyTrackPreset (*t->live, type);
        t->live->outBus = juce::jlimit (0, Session::kNumGroups, outBus);
    }
    selectedTrackId = id;
    changed();
    return id;
}

void Project::setTrackOutBus (int trackId, int bus)
{
    if (auto* t = findTrack (trackId))
    {
        t->live->outBus = juce::jlimit (0, Session::kNumGroups, bus);
        liveChanged();
    }
}

void Project::setTrackType (int trackId, int type)
{
    if (auto* t = findTrack (trackId))
    {
        applyTrackPreset (*t->live, type);
        liveChanged();
    }
}

void Project::removeTrack (int id)
{
    const int i = trackIndexForId (id);
    if (i < 0) return;
    beginEdit();
    session.tracks.erase (session.tracks.begin() + i);
    changed();
}

void Project::renameTrack (int id, const juce::String& name)
{
    if (auto* t = findTrack (id))
    {
        beginEdit();
        t->name = name.trim().isEmpty() ? t->name : name.trim();
        changed();
    }
}

void Project::setTrackColour (int id, juce::Colour c)
{
    if (auto* t = findTrack (id))
    {
        beginEdit();
        t->colour = c;
        changed();
    }
}

//==============================================================================
void Project::runAsync (std::function<std::function<void()>()> work)
{
    auto flag = alive;
    ++busy;
    sendChangeMessage();

    std::thread ([this, flag, work = std::move (work)]
    {
        auto apply = work();
        juce::MessageManager::callAsync ([this, flag, apply = std::move (apply)]
        {
            if (! flag->load()) return;
            --busy;
            if (apply) apply();
            sendChangeMessage();
        });
    }).detach();
}

void Project::importFiles (juce::Array<juce::File> files, int trackId, int64_t start,
                           std::function<void (juce::StringArray)> done)
{
    const double rate = sampleRate;
    auto* fm = &formats;

    runAsync ([=, this]() -> std::function<void()>
    {
        std::vector<AssetPtr> loaded;
        juce::StringArray errors;
        for (auto& f : files)
        {
            juce::String err;
            if (auto a = loadAsset (*fm, f, rate, err)) loaded.push_back (a);
            else errors.add (err);
        }

        return [=, this]
        {
            auto errs = errors;
            if (std::abs (rate - sampleRate) > 0.5)
                errs.add (TRANS ("La frequenza di campionamento e' cambiata durante l'importazione."));
            else if (! loaded.empty())
            {
                beginEdit();
                int idx = trackId >= 0 ? trackIndexForId (trackId) : -1;
                if (idx < 0) idx = (int) session.tracks.size();
                selection.clear();
                for (size_t i = 0; i < loaded.size(); ++i)
                {
                    while ((size_t) idx + i >= session.tracks.size()) addTrackInternal();
                    Clip c;
                    c.id = nextClipId++;
                    c.asset = loaded[i];
                    c.start = juce::jmax<int64_t> (0, start);
                    c.length = loaded[i]->length();
                    if (autoFadeMs > 0) c.fadeIn = c.fadeOut = juce::jmin (autoFadeSamples(), c.length / 4);
                    session.tracks[(size_t) idx + i].clips.push_back (c);
                    selection.insert (c.id);
                }
                changed();
            }
            if (done) done (errs);
        };
    });
}

void Project::addRecordings (std::vector<RecItem> items, std::function<void (juce::StringArray)> done)
{
    const double rate = sampleRate;
    auto* fm = &formats;

    runAsync ([=, this]() -> std::function<void()>
    {
        auto loaded = std::make_shared<std::vector<AssetPtr>>();
        juce::StringArray errors;
        for (auto& it : items)
        {
            juce::String err;
            auto a = loadAsset (*fm, it.file, rate, err);
            if (a == nullptr) errors.add (err);
            loaded->push_back (a);
        }

        return [=, this]
        {
            auto errs = errors;
            bool any = false;
            selection.clear();
            for (size_t i = 0; i < items.size(); ++i)
            {
                auto& asset = (*loaded)[i];
                if (asset == nullptr || findTrack (items[i].trackId) == nullptr) continue;
                if (! any) { beginEdit(); any = true; }

                // la registrazione arriva in ritardo di 'latency' campioni rispetto a quanto sentito dall'esecutore
                int64_t start = items[i].startPos - items[i].latency;
                int64_t offset = 0;
                if (start < 0) { offset = -start; start = 0; }
                if (offset >= asset->length()) continue;

                Clip c;
                c.id = nextClipId++;
                c.asset = asset;
                c.start = start;
                c.offset = offset;
                c.length = asset->length() - offset;
                if (autoFadeMs > 0) c.fadeIn = c.fadeOut = juce::jmin (autoFadeSamples(), c.length / 4);
                if (items[i].cueId != 0 && findCue (items[i].cueId) != nullptr)
                {
                    // take ADR: il nuovo take diventa quello attivo e gli altri vengono silenziati
                    const int cueId = items[i].cueId;
                    c.cueId = cueId;
                    c.takeNo = nextTakeNumber (cueId);
                    for (auto& tr : session.tracks) for (auto& oc : tr.clips) if (oc.cueId == cueId) oc.muted = true;
                    auto* cue = findCue (cueId);
                    cue->activeTake = c.takeNo;
                    if (cue->status == 0 || cue->status == 3) cue->status = 1;
                    if (items[i].takeName.isNotEmpty())
                        asset->name = items[i].takeName.contains ("{take}") ? items[i].takeName.replace ("{take}", juce::String (c.takeNo)) : items[i].takeName + "_T" + juce::String (c.takeNo);
                }
                findTrack (items[i].trackId)->clips.push_back (c);
                selection.insert (c.id);
            }
            if (any) changed();
            if (done) done (errs);
        };
    });
}

void Project::importTakes (std::vector<TakeFile> items, std::function<void (juce::StringArray, int)> done)
{
    const double rate = sampleRate;
    auto* fm = &formats;

    runAsync ([=, this]() -> std::function<void()>
    {
        auto loaded = std::make_shared<std::vector<AssetPtr>>();
        juce::StringArray errors;
        int n = 0;
        for (auto& it : items)
        {
            setTask (TRANS ("Importo le take registrate"), (float) n++ / (float) juce::jmax<size_t> (1, items.size()));
            juce::String err;
            auto a = loadAsset (*fm, it.file, rate, err);
            if (a == nullptr) errors.add (err);
            loaded->push_back (a);
        }
        setTask ({}, -1.0f);

        return [=, this]
        {
            auto errs = errors;
            int imported = 0;
            std::set<int> touched;
            std::map<int, int> bestOf;   // cue -> numero della take indicata come BEST
            selection.clear();
            bool any = false;
            // i take con il numero scritto nel nome si tengono; se due file reclamano lo stesso numero il secondo prende il primo libero, dopo tutti gli altri
            std::vector<size_t> order, deferred;
            {
                std::set<std::pair<int, int>> used;
                for (auto& t : session.tracks) for (auto& oc : t.clips) if (oc.cueId != 0) used.insert ({ oc.cueId, oc.takeNo });
                for (size_t i = 0; i < items.size(); ++i)
                    if (used.insert ({ items[i].cueId, items[i].takeNo }).second) order.push_back (i); else deferred.push_back (i);
                order.insert (order.end(), deferred.begin(), deferred.end());
            }
            for (size_t oi = 0; oi < order.size(); ++oi)
            {
                const size_t i = order[oi];
                auto& asset = (*loaded)[i];
                if (asset == nullptr || findTrack (items[i].trackId) == nullptr || findCue (items[i].cueId) == nullptr) continue;
                if (! any) { beginEdit(); any = true; }
                int64_t start = items[i].startPos, offset = 0;
                if (start < 0) { offset = -start; start = 0; }
                if (offset >= asset->length()) continue;

                Clip c;
                c.id = nextClipId++;
                c.asset = asset;
                c.start = start;
                c.offset = offset;
                c.length = asset->length() - offset;
                if (autoFadeMs > 0) c.fadeIn = c.fadeOut = juce::jmin (autoFadeSamples(), c.length / 4);
                c.cueId = items[i].cueId;
                c.takeNo = items[i].takeNo;
                for (auto& t : session.tracks) for (auto& oc : t.clips) if (oc.cueId == c.cueId && oc.takeNo == c.takeNo) { c.takeNo = nextTakeNumber (c.cueId); break; }   // numero gia' preso da un'altra take
                c.muted = true;
                findTrack (items[i].trackId)->clips.push_back (c);
                touched.insert (c.cueId);
                if (items[i].best) bestOf[c.cueId] = c.takeNo;
                ++imported;
            }
            for (int cueId : touched)
            {
                auto* cue = findCue (cueId);
                int active = bestOf.count (cueId) > 0 ? bestOf[cueId] : 0;
                if (active == 0)
                {
                    active = cue->activeTake;   // niente BEST: resta quella attiva, se c'e', altrimenti la piu' alta
                    for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId && cue->activeTake == 0) active = juce::jmax (active, c.takeNo);
                }
                for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) c.muted = (c.takeNo != active);
                cue->activeTake = active;
                if (cue->status == 0 || cue->status == 3) cue->status = 1;
            }
            if (any) changed();
            if (done) done (errs, imported);
        };
    });
}

void Project::setVideo (const juce::File& f, std::function<void (juce::String)> done)
{
    const double rate = sampleRate;
    auto* fm = &formats;

    runAsync ([=, this]() -> std::function<void()>
    {
        setTask (TRANS ("Carico il video: lettura delle informazioni"), 0.01f);
        auto info = video::probe (f);
        if (! info.ok)
        {
            setTask ({}, -1.0f);
            return [=] { if (done) done (info.error); };
        }

        auto va = std::make_shared<VideoAsset>();
        va->file = f;
        va->duration = info.duration;
        va->fps = info.fps;
        va->width = info.width;
        va->height = info.height;
        va->thumbInterval = juce::jmax (1.0, std::ceil (info.duration / 240.0));
        setTask (TRANS ("Carico il video: miniature"), 0.03f);
        va->thumbs = video::thumbnails (f, va->thumbInterval, (int) std::ceil (info.duration / va->thumbInterval), 56,
                                        [this] (float p) { setTask (TRANS ("Carico il video: miniature"), 0.03f + 0.37f * p); });

        AssetPtr audio;
        juce::String audioNote;
        if (info.hasAudio)
        {
            juce::String err;
            setTask (TRANS ("Carico il video: audio"), 0.4f);
            audio = loadAsset (*fm, f, rate, err, [this] (float p) { setTask (TRANS ("Carico il video: audio"), 0.4f + 0.6f * p); });
            if (audio == nullptr) audioNote = "Il video e' stato caricato, ma non riesco a estrarne l'audio: " + err;
        }

        setTask ({}, -1.0f);
        return [=, this]
        {
            if (std::abs (rate - sampleRate) > 0.5) { if (done) done (TRANS ("La frequenza di campionamento e' cambiata.")); return; }
            beginEdit();
            session.video = va;
            // il timecode della sessione prende la frequenza del video (il drop-frame resta se la frequenza e' la stessa)
            if (info.fpsKnown && std::abs (va->fps - tcFmt().fps) > 0.01) tcFormat = tcFormatForFps (va->fps);
            if (audio != nullptr)
            {
                // la traccia dell'audio del video sta in cima, accanto alla corsia video; una prima traccia vuota si riusa
                int id;
                if (! session.tracks.empty() && session.tracks.front().clips.empty())
                    id = session.tracks.front().id;
                else
                {
                    id = addTrackInternal();
                    std::rotate (session.tracks.begin(), session.tracks.end() - 1, session.tracks.end());
                }
                selectedTrackId = id;
                auto* t = findTrack (id);
                t->name = TRANS ("Audio video");
                applyTrackPreset (*t->live, TrackGuide);          // l'audio del video e' sempre la Guida: dialogo originale, piu' basso, sul suo bus, fuori dal mix consegnato
                t->live->outBus = roleOutBus (TrackGuide);
                t->colour = trackTypeColour (TrackGuide);
                Clip c;
                c.id = nextClipId++;
                c.asset = audio;
                c.start = 0;
                c.length = audio->length();
                t->clips.push_back (c);
            }
            changed();
            if (done) done (audioNote);
        };
    });
}

//==============================================================================
namespace
{
void sortLane (std::vector<AutoPoint>& v) { std::sort (v.begin(), v.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; }); }
}

int Project::addAutoPoint (int trackId, int64_t pos, float value)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) return 0;
    const int param = t->live->autoParam.load();
    beginEdit();
    AutoPoint p;
    p.id = nextAutoId++;
    p.pos = juce::jmax<int64_t> (0, pos);
    p.db = clampAutoValue (param, value);
    t->lane (param).push_back (p);
    sortLane (t->lane (param));
    if (t->live->autoMode.load() == 0) t->live->autoMode = 1;
    changed();
    return p.id;
}

void Project::removeAutoPoint (int trackId, int pointId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) return;
    auto& v = t->lane (t->live->autoParam.load());
    beginEdit();
    v.erase (std::remove_if (v.begin(), v.end(), [pointId] (const AutoPoint& p) { return p.id == pointId; }), v.end());
    changed();
}

void Project::clearAutomation (int trackId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) return;
    auto& v = t->lane (t->live->autoParam.load());
    if (v.empty()) return;
    beginEdit();
    v.clear();
    changed();
}

void Project::moveAutoPoint (int trackId, int pointId, int64_t pos, float value)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) return;
    const int param = t->live->autoParam.load();
    auto& v = t->lane (param);
    for (auto& p : v)
        if (p.id == pointId)
        {
            p.pos = juce::jmax<int64_t> (0, pos);
            p.db = clampAutoValue (param, value);
            break;
        }
    sortLane (v);
    changed();
}

void Project::writeAuto (int trackId, int param, int64_t pos, float value)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) return;
    param = juce::jlimit (0, kNumAutoParams - 1, param);
    if (touchTrack != trackId || touchParam != param)
    {
        beginEdit();                       // un solo passo di undo per ogni "touch"
        touchTrack = trackId;
        touchParam = param;
        touchLastPos = pos;
    }
    // la regione appena scritta sostituisce i punti esistenti
    // (il punto scritto per ultimo resta: si cancellano solo quelli preesistenti nella regione attraversata)
    auto& v = t->lane (param);
    const int64_t last = touchLastPos;
    v.erase (std::remove_if (v.begin(), v.end(), [last, pos] (const AutoPoint& p)
    {
        if (pos > last) return p.pos > last && p.pos <= pos;
        if (pos < last) return p.pos >= pos && p.pos < last;
        return p.pos == pos;
    }), v.end());
    AutoPoint p;
    p.id = nextAutoId++;
    p.pos = pos;
    p.db = clampAutoValue (param, value);
    v.push_back (p);
    sortLane (v);
    touchLastPos = pos;
    changed();
}

//==============================================================================
AdrCue* Project::findCue (int id) noexcept
{
    for (auto& c : session.cues) if (c.id == id) return &c;
    return nullptr;
}

const AdrCue* Project::findCue (int id) const noexcept
{
    for (auto& c : session.cues) if (c.id == id) return &c;
    return nullptr;
}

int Project::addCue (const AdrCue& in)
{
    beginEdit();
    AdrCue c = in;
    c.id = nextCueId++;
    if (c.number <= 0)
    {
        int mx = 0;
        for (auto& o : session.cues) mx = juce::jmax (mx, o.number);
        c.number = mx + 1;
    }
    session.cues.push_back (c);
    std::sort (session.cues.begin(), session.cues.end(), [] (const AdrCue& a, const AdrCue& b) { return a.inPos < b.inPos; });
    changed();
    return c.id;
}

int Project::addCues (std::vector<AdrCue> cues, bool replaceExisting)
{
    beginEdit();
    if (replaceExisting) session.cues.clear();
    int mx = 0;
    for (auto& o : session.cues) mx = juce::jmax (mx, o.number);
    for (auto& c : cues)
    {
        c.id = nextCueId++;
        if (c.number <= 0) c.number = ++mx; else mx = juce::jmax (mx, c.number);
        session.cues.push_back (c);
    }
    std::sort (session.cues.begin(), session.cues.end(), [] (const AdrCue& a, const AdrCue& b) { return a.inPos < b.inPos; });
    changed();
    return (int) cues.size();
}

void Project::removeCue (int id)
{
    if (findCue (id) == nullptr) return;
    beginEdit();
    session.cues.erase (std::remove_if (session.cues.begin(), session.cues.end(), [id] (const AdrCue& c) { return c.id == id; }), session.cues.end());
    changed();
}

void Project::editCue (int id, const std::function<void (AdrCue&)>& change)
{
    if (auto* c = findCue (id))
    {
        beginEdit();
        change (*c);
        std::sort (session.cues.begin(), session.cues.end(), [] (const AdrCue& a, const AdrCue& b) { return a.inPos < b.inPos; });
        changed();
    }
}

std::vector<int64_t> Project::takeClipIds (int cueId) const
{
    std::vector<std::pair<int, int64_t>> found;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (c.cueId == cueId && cueId != 0) found.emplace_back (c.takeNo, c.id);
    std::sort (found.begin(), found.end());
    std::vector<int64_t> ids;
    for (auto& f : found) ids.push_back (f.second);
    return ids;
}

namespace { int bestTake (const std::vector<Project::TakeInfo>& v, int excluding); }

void Project::deleteTake (int cueId, int takeNo)
{
    auto* cue = findCue (cueId);
    if (cue == nullptr) return;
    beginEdit();
    for (auto& t : session.tracks)
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [cueId, takeNo] (const Clip& c) { return c.cueId == cueId && c.takeNo == takeNo; }), t.clips.end());
    if (cue->activeTake == takeNo)
    {
        const int best = bestTake (takeInfos (cueId), takeNo);
        cue->activeTake = best;
        for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) c.muted = c.rejected || (c.takeNo != best);
        if (best == 0 && cue->status == 1) cue->status = 0;
    }
    changed();
}

std::vector<Project::TakeInfo> Project::takeInfos (int cueId) const
{
    std::vector<TakeInfo> out;
    const auto* cue = findCue (cueId);
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (c.cueId == cueId && cueId != 0)
            {
                TakeInfo i;
                i.takeNo = c.takeNo; i.clipId = c.id; i.rating = c.rating; i.rejected = c.rejected;
                i.active = cue != nullptr && cue->activeTake == c.takeNo;
                i.seconds = (double) c.length / juce::jmax (1.0, sampleRate);
                out.push_back (i);
            }
    std::sort (out.begin(), out.end(), [] (const TakeInfo& x, const TakeInfo& y) { return x.takeNo < y.takeNo; });
    return out;
}

namespace
{
/** Il take da far suonare: il piu' valutato tra quelli non scartati (a parita' il piu' recente); 0 se non ce ne sono. */
int bestTake (const std::vector<Project::TakeInfo>& v, int excluding)
{
    int best = 0, bestRating = -1;
    for (auto& i : v)
        if (! i.rejected && i.takeNo != excluding && i.rating >= bestRating) { best = i.takeNo; bestRating = i.rating; }
    return best;
}
}

void Project::setTakeRating (int cueId, int takeNo, int stars)
{
    bool found = false;
    for (auto& t : session.tracks) for (auto& c : t.clips) found = found || (c.cueId == cueId && c.takeNo == takeNo);
    if (! found) return;
    beginEdit();
    for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId && c.takeNo == takeNo) c.rating = juce::jlimit (0, 5, stars);
    changed();
}

void Project::setTakeRejected (int cueId, int takeNo, bool rejected)
{
    auto* cue = findCue (cueId);
    if (cue == nullptr) return;
    beginEdit();
    for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId && c.takeNo == takeNo) { c.rejected = rejected; if (rejected) c.muted = true; }
    const auto infos = takeInfos (cueId);
    if (rejected && cue->activeTake == takeNo)
    {
        const int best = bestTake (infos, takeNo);
        cue->activeTake = best;
        for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) c.muted = c.rejected || c.takeNo != best;
        if (best == 0 && cue->status == 1) cue->status = 0;
    }
    else if (! rejected && cue->activeTake == 0)   // un take ripristinato e' l'unico: suona
    {
        cue->activeTake = takeNo;
        for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) c.muted = c.rejected || c.takeNo != takeNo;
        if (cue->status == 0 || cue->status == 3) cue->status = 1;
    }
    changed();
}

void Project::keepOnlyTake (int cueId, int takeNo)
{
    auto* cue = findCue (cueId);
    if (cue == nullptr) return;
    beginEdit();
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (c.cueId == cueId) { c.rejected = c.takeNo != takeNo; c.muted = c.takeNo != takeNo; }
    cue->activeTake = takeNo;
    if (cue->status == 0 || cue->status == 3) cue->status = 1;
    changed();
}

int Project::deleteRejectedTakes (int cueId)
{
    int n = 0;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (c.rejected && c.cueId != 0 && (cueId == 0 || c.cueId == cueId)) ++n;
    if (n == 0) return 0;
    beginEdit();
    for (auto& t : session.tracks)
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [cueId] (const Clip& c) { return c.rejected && c.cueId != 0 && (cueId == 0 || c.cueId == cueId); }), t.clips.end());
    changed();
    return n;
}

int Project::undecidedCues() const
{
    int n = 0;
    for (auto& c : session.cues)
    {
        int alive = 0;
        for (auto& i : takeInfos (c.id)) alive += i.rejected ? 0 : 1;
        if (alive > 1) ++n;
    }
    return n;
}

int Project::cleanUpTrack (int trackId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->editLocked()) return 0;
    int n = 0;
    for (auto& c : t->clips) if (c.muted) ++n;
    if (n == 0) return 0;
    beginEdit();
    t->clips.erase (std::remove_if (t->clips.begin(), t->clips.end(), [] (const Clip& c) { return c.muted; }), t->clips.end());
    changed();
    return n;
}

int Project::takeCount (int cueId) const
{
    int n = 0;
    for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId && cueId != 0) ++n;
    return n;
}

int Project::nextTakeNumber (int cueId) const
{
    int mx = 0;
    for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) mx = juce::jmax (mx, c.takeNo);
    return mx + 1;
}

void Project::setActiveTake (int cueId, int takeNo)
{
    auto* cue = findCue (cueId);
    if (cue == nullptr) return;
    beginEdit();
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (c.cueId == cueId) { if (c.takeNo == takeNo) c.rejected = false; c.muted = (c.takeNo != takeNo); }
    cue->activeTake = takeNo;
    changed();
}

//==============================================================================
Marker* Project::findMarker (int id) noexcept
{
    for (auto& m : session.markers) if (m.id == id) return &m;
    return nullptr;
}

void Project::addMarkers (const std::vector<std::pair<int64_t, juce::String>>& list, juce::Colour colour)
{
    if (list.empty()) return;
    beginEdit();
    for (auto& e : list)
    {
        Marker m;
        m.id = nextMarkerId++;
        m.pos = juce::jmax<int64_t> (0, e.first);
        m.name = e.second;
        m.colour = colour;
        session.markers.push_back (m);
    }
    std::sort (session.markers.begin(), session.markers.end(), [] (const Marker& a, const Marker& b) { return a.pos < b.pos; });
    changed();
}

int Project::addCycleMarker (int64_t start, int64_t end, const juce::String& name)
{
    beginEdit();
    Marker m;
    m.id = nextMarkerId++;
    m.pos = juce::jmax<int64_t> (0, start);
    m.end = end > m.pos ? end : -1;
    m.name = name.isEmpty() ? TRANS ("Ciclo ") + juce::String ((int) session.markers.size() + 1) : name;
    session.markers.push_back (m);
    std::sort (session.markers.begin(), session.markers.end(), [] (const Marker& a, const Marker& b) { return a.pos < b.pos; });
    changed();
    return m.id;
}

void Project::setMarkerEnd (int id, int64_t end)
{
    if (auto* m = findMarker (id)) { beginEdit(); m->end = end > m->pos ? end : -1; changed(); }
}

int Project::addMarker (int64_t pos, const juce::String& name)
{
    beginEdit();
    Marker m;
    m.id = nextMarkerId++;
    m.pos = juce::jmax<int64_t> (0, pos);
    m.name = name.isEmpty() ? "Marker " + juce::String ((int) session.markers.size() + 1) : name;
    session.markers.push_back (m);
    std::sort (session.markers.begin(), session.markers.end(), [] (const Marker& a, const Marker& b) { return a.pos < b.pos; });
    changed();
    return m.id;
}

void Project::removeMarker (int id)
{
    if (findMarker (id) == nullptr) return;
    beginEdit();
    session.markers.erase (std::remove_if (session.markers.begin(), session.markers.end(), [id] (const Marker& m) { return m.id == id; }),
                           session.markers.end());
    changed();
}

void Project::renameMarker (int id, const juce::String& name)
{
    if (auto* m = findMarker (id))
    {
        beginEdit();
        m->name = name.trim().isEmpty() ? m->name : name.trim();
        changed();
    }
}

void Project::setMarkerColour (int id, juce::Colour c)
{
    if (auto* m = findMarker (id)) { beginEdit(); m->colour = c; changed(); }
}

int64_t Project::nextMarkerPos (int64_t pos) const
{
    int64_t best = -1;
    for (auto& m : session.markers) if (m.pos > pos && (best < 0 || m.pos < best)) best = m.pos;
    return best;
}

int64_t Project::previousMarkerPos (int64_t pos) const
{
    int64_t best = -1;
    for (auto& m : session.markers) if (m.pos < pos && m.pos > best) best = m.pos;
    return best;
}

void Project::setVideoOffset (double seconds)
{
    if (session.video == nullptr) return;
    seconds = juce::jlimit (-30.0, 30.0, seconds);
    if (std::abs (seconds - session.video->offsetSec) < 1.0e-9) return;
    session.video->offsetSec = seconds;
    liveChanged();
}

void Project::clearVideo()
{
    if (session.video == nullptr) return;
    beginEdit();
    session.video.reset();
    changed();
}

bool Project::trackLocked (int trackId) const noexcept
{
    for (auto& t : session.tracks) if (t.id == trackId) return t.editLocked();
    return false;
}

void Project::setTracksLocked (const std::vector<int>& ids, bool locked)
{
    for (auto& t : session.tracks) if (std::find (ids.begin(), ids.end(), t.id) != ids.end()) t.locked = locked;
    markDirty();
}

void Project::setTracksHidden (const std::vector<int>& ids, bool hidden)
{
    for (auto& t : session.tracks) if (std::find (ids.begin(), ids.end(), t.id) != ids.end()) t.hidden = hidden;
    markDirty();
}

void Project::freezeTrack (int trackId, const juce::File& folder, std::function<void (juce::String)> done)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) { if (done) done (TRANS ("La traccia non c'e' piu'.")); return; }
    if (t->frozen != nullptr) { if (done) done (TRANS ("La traccia e' gia' congelata.")); return; }
    int64_t end = 0;
    for (auto& c : t->clips) if (! c.muted || c.cueId == 0) end = juce::jmax (end, c.end());
    if (end <= 0) { if (done) done (TRANS ("La traccia e' vuota: non c'e' niente da congelare.")); return; }

    const double rate = sampleRate;
    auto* fm = &formats;
    auto snap = std::make_shared<Session> (session.cloneDetached());
    const int64_t total = end + (int64_t) (2.0 * rate);   // coda per riverberi e delay
    const auto base = juce::File::createLegalFileName (t->name + "_freeze");

    runAsync ([=, this]() -> std::function<void()>
    {
        folder.createDirectory();
        const auto file = folder.getNonexistentChildFile (base, ".wav", false);
        juce::String err;
        setTask (TRANS ("Freeze della traccia"), 0.0f);
        const bool ok = renderTrackProcessed (*snap, trackId, rate, total, file, [this] (double p) { setTask (TRANS ("Freeze della traccia"), (float) p); return true; }, err);
        AssetPtr a = ok ? loadAsset (*fm, file, rate, err) : nullptr;
        setTask ({}, -1.0f);
        return [=, this]
        {
            auto* tr = findTrack (trackId);
            if (a == nullptr || tr == nullptr) { if (done) done (err.isNotEmpty() ? err : TRANS ("Freeze non riuscito.")); return; }
            beginEdit();
            tr->frozen = a;
            changed();
            if (done) done ({});
        };
    });
}

void Project::unfreezeTrack (int trackId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->frozen == nullptr) return;
    beginEdit();
    t->frozen.reset();
    changed();
}

void Project::setTracksLanes (const std::vector<int>& ids, bool on)
{
    for (auto& t : session.tracks) if (std::find (ids.begin(), ids.end(), t.id) != ids.end()) t.showLanes = on;
    markDirty();
}

//==============================================================================
// Track Versions
int64_t Project::zeroCrossNear (const Clip& c, int64_t pos) const
{
    if (! snapZero || c.asset == nullptr || c.asset->offline) return pos;
    const int64_t rel = c.offset + (pos - c.start);
    const int radius = (int) (0.005 * sampleRate);
    const auto& a = c.asset->audio;
    if (rel < 1 || rel + radius + 2 >= a.getNumSamples()) return pos;
    const float* d = a.getReadPointer (0);
    int64_t best = pos;
    int bestDist = radius + 1;
    for (int k = -radius; k <= radius; ++k)
    {
        const int64_t i = rel + k;
        if (i < 0 || i + 1 >= a.getNumSamples()) continue;
        if ((d[i] <= 0.0f && d[i + 1] > 0.0f) || (d[i] >= 0.0f && d[i + 1] < 0.0f))
            if (std::abs (k) < bestDist) { bestDist = std::abs (k); best = pos + k; }
    }
    return juce::jlimit (c.start + 16, c.end() - 16, best);
}

bool Project::setClipInfo (int64_t clipId, const ClipEdit& e)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || c->asset == nullptr || session.tracks[(size_t) ti].editLocked()) return false;
    Clip n = *c;
    if (e.start) n.start = juce::jmax<int64_t> (0, *e.start);
    if (e.offset) n.offset = juce::jmax<int64_t> (0, *e.offset);
    if (e.length) n.length = *e.length;
    if (n.length < 16 || n.offset + n.length > c->asset->length()) return false;
    if (e.gainDb) n.gainDb = juce::jlimit (-60.0f, 24.0f, *e.gainDb);
    if (e.fadeIn) n.fadeIn = juce::jlimit<int64_t> (0, n.length, *e.fadeIn);
    if (e.fadeOut) n.fadeOut = juce::jlimit<int64_t> (0, n.length, *e.fadeOut);
    n.fadeIn = juce::jmin (n.fadeIn, n.length);
    n.fadeOut = juce::jmin (n.fadeOut, n.length);
    if (e.muted) n.muted = *e.muted;
    if (e.invert) n.invert = *e.invert;
    beginEdit();
    *c = n;
    changed();
    return true;
}

bool Project::makeCrossfade (int64_t idA, int64_t idB, double lengthSec, int shape)
{
    int ta = -1, tb = -1;
    auto* a = findClip (idA, &ta);
    auto* b = findClip (idB, &tb);
    if (a == nullptr || b == nullptr || ta != tb || session.tracks[(size_t) ta].editLocked()) return false;
    if (b->start < a->start) { std::swap (a, b); std::swap (idA, idB); }
    if (a->asset == nullptr || b->asset == nullptr || a->asset->offline || b->asset->offline) return false;
    const int64_t len = juce::jmax<int64_t> (16, (int64_t) (lengthSec * sampleRate));
    const int64_t boundary = juce::jmax (a->end() <= b->start ? a->end() : b->start, a->start + 16);   // il punto del taglio
    const int64_t bEdge = juce::jmin (boundary, b->end() - 16);
    // A si allunga oltre il taglio e B parte prima: serve materiale audio da entrambe le parti
    const int64_t extraA = len / 2, extraB = len - len / 2;
    if (a->offset + a->length + extraA > a->asset->length() || b->offset < extraB) return false;
    beginEdit();
    const int64_t aEnd = juce::jmax (a->end(), boundary);
    a->length = (aEnd - a->start) + extraA;
    const int64_t newStart = bEdge - extraB;
    const int64_t shift = b->start - newStart;
    b->start = newStart;
    b->offset -= shift;
    b->length += shift;
    b->xShape = juce::jlimit (0, 2, shape);
    changed();
    return true;
}

int Project::addVersion (int trackId, const juce::String& name, bool duplicateCurrent)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->editLocked()) return 0;
    beginEdit();
    if (t->versions.empty()) { t->versions.push_back ({ 1, "V1", {} }); t->activeVersion = 1; }
    for (auto& v : t->versions) if (v.id == t->activeVersion) v.clips = t->clips;
    int id = 1;
    for (auto& v : t->versions) id = juce::jmax (id, v.id + 1);
    Track::Version nv;
    nv.id = id;
    nv.name = name.trim().isEmpty() ? "V" + juce::String (id) : name.trim();
    if (duplicateCurrent) { nv.clips = t->clips; for (auto& c : nv.clips) c.id = nextClipId++; }
    t->clips = nv.clips;
    t->versions.push_back (std::move (nv));
    t->activeVersion = id;
    selection.clear();
    changed();
    return id;
}

void Project::switchVersion (int trackId, int versionId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->editLocked() || versionId == t->activeVersion) return;
    const Track::Version* target = nullptr;
    for (auto& v : t->versions) if (v.id == versionId) target = &v;
    if (target == nullptr) return;
    beginEdit();
    for (auto& v : t->versions) if (v.id == t->activeVersion) v.clips = t->clips;
    t->clips = target->clips;
    t->activeVersion = versionId;
    selection.clear();
    changed();
}

void Project::renameVersion (int trackId, int versionId, const juce::String& name)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || name.trim().isEmpty()) return;
    beginEdit();
    for (auto& v : t->versions) if (v.id == versionId) v.name = name.trim();
    changed();
}

void Project::deleteVersion (int trackId, int versionId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->editLocked() || t->versions.size() < 2) return;
    beginEdit();
    if (versionId == t->activeVersion)
    {
        for (auto& v : t->versions) if (v.id != versionId) { t->clips = v.clips; t->activeVersion = v.id; break; }
    }
    t->versions.erase (std::remove_if (t->versions.begin(), t->versions.end(), [versionId] (const Track::Version& v) { return v.id == versionId; }), t->versions.end());
    if (t->versions.size() < 2) { t->versions.clear(); t->activeVersion = 0; }
    selection.clear();
    changed();
}

int Project::lanesFromVersions (int trackId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->editLocked() || t->versions.size() < 2) return 0;
    beginEdit();
    int n = 0;
    for (auto& v : t->versions)
        if (v.id != t->activeVersion)
            for (auto c : v.clips) { c.id = nextClipId++; c.muted = true; t->clips.push_back (c); ++n; }
    t->versions.clear();
    t->activeVersion = 0;
    t->showLanes = true;
    selection.clear();
    changed();
    return n;
}

int Project::versionsFromLanes (int trackId)
{
    auto* t = findTrack (trackId);
    if (t == nullptr || t->editLocked() || ! t->versions.empty()) return 0;
    session.computeLanes();
    int lanes = 1;
    for (auto& c : t->clips) lanes = juce::jmax (lanes, c.lane + 1);
    if (lanes < 2) return 0;
    beginEdit();
    std::vector<Track::Version> vs (lanes);
    for (int k = 0; k < lanes; ++k) { vs[(size_t) k].id = k + 1; vs[(size_t) k].name = k == 0 ? juce::String ("V1") : TRANS ("Corsia ") + juce::String (k + 1); }
    for (auto& c : t->clips) vs[(size_t) c.lane].clips.push_back (c);
    for (int k = 1; k < lanes; ++k) for (auto& c : vs[(size_t) k].clips) c.muted = false;
    t->clips = vs[0].clips;
    t->versions = std::move (vs);
    t->activeVersion = 1;
    t->showLanes = false;
    selection.clear();
    changed();
    return lanes - 1;
}

void Project::setSoloLane (int trackId, int lane)
{
    auto* t = findTrack (trackId);
    if (t == nullptr) return;
    t->soloLane = t->soloLane == lane ? -1 : lane;
    changed();
}

//==============================================================================
int Project::stripSilence (const std::vector<int64_t>& clipIds, float thresholdDb, double minSilenceSec, double padSec, bool deleteSilence)
{
    struct Edit { size_t track; int64_t id; std::vector<std::pair<int64_t, int64_t>> regions; };
    std::vector<Edit> edits;
    for (size_t ti = 0; ti < session.tracks.size(); ++ti)
    {
        auto& t = session.tracks[ti];
        if (t.editLocked()) continue;
        for (auto& c : t.clips)
        {
            if (std::find (clipIds.begin(), clipIds.end(), c.id) == clipIds.end() || c.muted || c.asset == nullptr || c.asset->offline) continue;
            auto r = tools::soundRegions (c.asset->audio, c.offset, c.length, sampleRate, thresholdDb, minSilenceSec, padSec);
            if (r.size() == 1 && r[0].first == 0 && r[0].second == c.length) continue;   // niente da togliere
            edits.push_back ({ ti, c.id, std::move (r) });
        }
    }
    if (edits.empty()) return 0;
    beginEdit();
    const int64_t fade = (int64_t) (0.004 * sampleRate);
    selection.clear();
    for (auto& e : edits)
    {
        auto& clips = session.tracks[e.track].clips;
        const auto it = std::find_if (clips.begin(), clips.end(), [&e] (const Clip& c) { return c.id == e.id; });
        if (it == clips.end()) continue;
        const Clip orig = *it;
        clips.erase (it);
        // zone da produrre: con deleteSilence solo quelle con suono, altrimenti tutto il clip diviso ai bordi
        std::vector<std::pair<int64_t, int64_t>> pieces;
        int64_t cursor = 0;
        for (auto& r : e.regions)
        {
            if (! deleteSilence && r.first > cursor) pieces.emplace_back (cursor, r.first);
            pieces.emplace_back (r.first, r.second);
            cursor = r.second;
        }
        if (! deleteSilence && cursor < orig.length) pieces.emplace_back (cursor, orig.length);
        bool first = true;
        for (auto& pc : pieces)
        {
            Clip c = orig;
            c.id = first ? orig.id : nextClipId++;
            c.start = orig.start + pc.first;
            c.offset = orig.offset + pc.first;
            c.length = pc.second - pc.first;
            c.fadeIn = pc.first == 0 ? orig.fadeIn : juce::jmin (fade, c.length / 2);
            c.fadeOut = pc.second == orig.length ? orig.fadeOut : juce::jmin (fade, c.length / 2);
            clips.push_back (c);
            selection.insert (c.id);
            first = false;
        }
    }
    changed();
    return (int) edits.size();
}

void Project::processClips (const std::vector<int64_t>& clipIds, int opInt, double targetLufs, const juce::File& folder, std::function<void (juce::String)> done)
{
    const auto op = (tools::Op) opInt;
    struct Job { int64_t id; AssetPtr asset; int64_t off, len; juce::String base; };
    std::vector<Job> jobs;
    for (auto& t : session.tracks)
    {
        if (t.editLocked()) continue;
        for (auto& c : t.clips)
            if (std::find (clipIds.begin(), clipIds.end(), c.id) != clipIds.end() && c.asset != nullptr && ! c.asset->offline && c.length > 0)
                jobs.push_back ({ c.id, c.asset, c.offset, c.length, juce::File::createLegalFileName (c.asset->name) });
    }
    if (jobs.empty()) { if (done) done (TRANS ("Seleziona almeno un clip (non su una traccia bloccata).")); return; }

    if (op == tools::Op::InvertPolarity)
    {
        beginEdit();
        for (auto& j : jobs) if (auto* c = findClip (j.id)) c->invert = ! c->invert;
        changed();
        if (done) done ({});
        return;
    }

    const double rate = sampleRate;
    auto* fm = &formats;
    auto shared = std::make_shared<std::vector<Job>> (std::move (jobs));
    runAsync ([=, this]() -> std::function<void()>
    {
        struct Result { AssetPtr asset; double gainDb = 0.0; bool ok = false; };
        auto results = std::make_shared<std::vector<Result>>();
        juce::String err;
        int k = 0;
        for (auto& j : *shared)
        {
            setTask (TRANS ("Elaboro i clip"), (float) k++ / (float) shared->size());
            Result r;
            if (op == tools::Op::NormalizeLufs || op == tools::Op::NormalizeDialogueLufs)
            {
                const double l = tools::measureLufs (j.asset->audio, j.off, j.len, rate, op == tools::Op::NormalizeDialogueLufs);
                if (l > -90.0) { r.gainDb = juce::jlimit (-30.0, 30.0, targetLufs - l); r.ok = true; }
                else err = TRANS ("Un clip e' troppo silenzioso per misurarne il loudness.");
            }
            else
            {
                const int nch = j.asset->audio.getNumChannels();
                juce::AudioBuffer<float> buf (nch, (int) j.len);
                for (int ch = 0; ch < nch; ++ch) buf.copyFrom (ch, 0, j.asset->audio, ch, (int) j.off, (int) j.len);
                if (op == tools::Op::Reverse)
                    for (int ch = 0; ch < nch; ++ch) std::reverse (buf.getWritePointer (ch), buf.getWritePointer (ch) + buf.getNumSamples());
                else   // RemoveDc: toglie la media di ogni canale
                    for (int ch = 0; ch < nch; ++ch)
                    {
                        double sum = 0.0;
                        const float* p = buf.getReadPointer (ch);
                        for (int i = 0; i < buf.getNumSamples(); ++i) sum += p[i];
                        const float mean = (float) (sum / juce::jmax (1, buf.getNumSamples()));
                        float* w = buf.getWritePointer (ch);
                        for (int i = 0; i < buf.getNumSamples(); ++i) w[i] -= mean;
                    }
                folder.createDirectory();
                const auto file = folder.getNonexistentChildFile (j.base + (op == tools::Op::Reverse ? "_rev" : "_dc"), ".wav", false);
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w;
                if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) nch, 32, {}, 0));
                if (w == nullptr) { err = "Impossibile scrivere " + file.getFullPathName(); results->push_back (r); continue; }
                os.release();
                w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
                w.reset();
                juce::String e2;
                r.asset = loadAsset (*fm, file, rate, e2);
                r.ok = r.asset != nullptr;
                if (! r.ok) err = e2;
            }
            results->push_back (r);
        }
        setTask ({}, -1.0f);
        return [=, this]
        {
            bool any = false;
            for (size_t i = 0; i < shared->size(); ++i)
            {
                auto* c = findClip ((*shared)[i].id);
                if (c == nullptr || ! (*results)[i].ok) continue;
                if (! any) { beginEdit(); any = true; c = findClip ((*shared)[i].id); }
                if ((*results)[i].asset != nullptr)
                {
                    c->asset = (*results)[i].asset;
                    c->offset = 0;
                    c->length = c->asset->length();
                    if (op == tools::Op::Reverse) std::swap (c->fadeIn, c->fadeOut);
                }
                else c->gainDb = (float) (*results)[i].gainDb;
            }
            if (any) changed();
            if (done) done (any ? juce::String() : (err.isNotEmpty() ? err : juce::String ("Elaborazione non riuscita.")));
        };
    });
}

//==============================================================================
// Clip Package / Track Archive
namespace
{
/** Percorsi dei file (audio, congelati, video) scritti in un albero di sessione. */
void collectFiles (const juce::var& v, std::set<juce::String>& out)
{
    if (auto* o = v.getDynamicObject())
    {
        for (auto& nv : o->getProperties())
        {
            if ((nv.name.toString() == "file" || nv.name.toString() == "frozenFile" || nv.name.toString() == "video") && nv.value.isString() && nv.value.toString().isNotEmpty())
                out.insert (nv.value.toString());
            else collectFiles (nv.value, out);
        }
    }
    else if (auto* a = v.getArray())
        for (auto& e : *a) collectFiles (e, out);
}

void relocateFiles (juce::var& v, const std::map<juce::String, juce::String>& map)
{
    if (auto* o = v.getDynamicObject())
    {
        std::vector<juce::Identifier> names;
        for (auto& nv : o->getProperties()) names.push_back (nv.name);
        for (auto& n : names)
        {
            auto child = o->getProperty (n);
            const auto key = n.toString();
            if ((key == "file" || key == "frozenFile" || key == "video") && child.isString())
            {
                if (auto it = map.find (child.toString()); it != map.end()) o->setProperty (n, it->second);
            }
            else if (child.getDynamicObject() != nullptr || child.getArray() != nullptr) relocateFiles (child, map);
        }
    }
    else if (auto* a = v.getArray())
        for (auto& e : *a) relocateFiles (e, map);
}
}

void Project::exportPackage (const juce::File& zip, const std::vector<int>& onlyTrackIds, std::function<void (juce::String)> done)
{
    auto root = toVar();
    if (! onlyTrackIds.empty())
    {
        // archivio di tracce: solo le tracce scelte, senza cue, marker e video
        auto* ro = root.getDynamicObject();
        juce::Array<juce::var> kept;
        if (auto* all = ro->getProperty ("tracks").getArray())
            for (size_t i = 0; i < session.tracks.size() && i < (size_t) all->size(); ++i)
                if (std::find (onlyTrackIds.begin(), onlyTrackIds.end(), session.tracks[i].id) != onlyTrackIds.end()) kept.add ((*all)[(int) i]);
        ro->setProperty ("tracks", kept);
        ro->setProperty ("cues", juce::Array<juce::var>());
        ro->setProperty ("markers", juce::Array<juce::var>());
        ro->removeProperty ("video");
        ro->removeProperty ("notes");
        juce::Array<juce::var> none;
        ro->setProperty ("trackGroups", none);
    }
    if (root.getDynamicObject() == nullptr || (root.getDynamicObject()->getProperty ("tracks").getArray() != nullptr && root.getDynamicObject()->getProperty ("tracks").getArray()->isEmpty()))
    { if (done) done (TRANS ("Non ci sono tracce da esportare.")); return; }

    runAsync ([=, this]() mutable -> std::function<void()>
    {
        std::set<juce::String> files;
        collectFiles (root, files);
        const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("adr_pacchetto_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        temp.createDirectory();
        const auto media = temp.getChildFile ("Media");
        media.createDirectory();
        std::map<juce::String, juce::String> map;
        juce::String err;
        int k = 0;
        for (auto& path : files)
        {
            setTask (TRANS ("Preparo il pacchetto"), (float) k++ / (float) juce::jmax<size_t> (1, files.size()) * 0.8f);
            const juce::File src (path);
            if (! src.existsAsFile()) { err += (err.isEmpty() ? "" : "\n") + juce::String (TRANS ("File non trovato (non incluso): ")) + path; continue; }
            const auto rel = "Media/" + juce::String (k).paddedLeft ('0', 3) + "_" + juce::File::createLegalFileName (src.getFileName());
            if (src.copyFileTo (temp.getChildFile (rel))) map[path] = rel;
        }
        relocateFiles (root, map);
        temp.getChildFile ("session.adb").replaceWithText (juce::JSON::toString (root, false));
        juce::ZipFile::Builder builder;
        builder.addFile (temp.getChildFile ("session.adb"), 9, "session.adb");
        for (auto& e : map) builder.addFile (temp.getChildFile (e.second), 0, e.second);   // l'audio non si comprime: e' lento e guadagna poco
        zip.deleteFile();
        {
            juce::FileOutputStream os (zip);
            if (os.openedOk()) { builder.writeToStream (os, nullptr); os.flush(); }
            else err += (err.isEmpty() ? "" : "\n") + juce::String (TRANS ("Impossibile scrivere ")) + zip.getFullPathName();
        }
        temp.deleteRecursively();
        setTask ({}, -1.0f);
        return [=] { if (done) done (err); };
    });
}

void Project::appendTracksFrom (const Project& other)
{
    if (other.session.tracks.empty()) return;
    beginEdit();
    int firstId = -1;
    for (auto t : other.session.tracks)
    {
        t.id = nextTrackId++;
        t.groupId = 0;
        for (auto& c : t.clips) c.id = nextClipId++;
        for (auto& v : t.versions) for (auto& c : v.clips) c.id = nextClipId++;
        if (firstId < 0) firstId = t.id;
        session.tracks.push_back (std::move (t));
    }
    selectedTrackId = firstId;
    changed();
    if (onSessionLoaded) onSessionLoaded();   // ripristina i plug-in delle tracce arrivate
}

void Project::importTrackArchive (const juce::File& zip, const juce::File& extractTo, std::function<void (juce::String)> done)
{
    auto holder = std::make_shared<std::unique_ptr<Project>>();
    runAsync ([=, this]() -> std::function<void()>
    {
        setTask (TRANS ("Apro l'archivio"), 0.1f);
        juce::String err;
        juce::ZipFile z (zip);
        if (z.getNumEntries() == 0) err = TRANS ("L'archivio e' vuoto o non e' valido.");
        else
        {
            extractTo.createDirectory();
            const auto r = z.uncompressTo (extractTo, true);
            if (! r.wasOk()) err = r.getErrorMessage();
        }
        setTask ({}, -1.0f);
        return [=, this]
        {
            if (err.isNotEmpty() || ! extractTo.getChildFile ("session.adb").existsAsFile()) { if (done) done (err.isNotEmpty() ? err : TRANS ("L'archivio non contiene una sessione.")); return; }
            *holder = std::make_unique<Project> (formats, sampleRate);
            (*holder)->loadSession (extractTo.getChildFile ("session.adb"), [this, holder, done] (juce::String errors)
            {
                appendTracksFrom (**holder);
                juce::MessageManager::callAsync ([holder] { holder->reset(); });   // il progetto temporaneo non si distrugge dentro la sua stessa callback
                if (done) done (errors);
            });
        };
    });
}

void Project::showAllTracks()
{
    for (auto& t : session.tracks) t.hidden = false;
    markDirty();
}

void Project::hideEmptyTracks()
{
    for (auto& t : session.tracks) if (t.clips.empty()) t.hidden = true;
    markDirty();
}

bool Project::anyTrackMuted() const noexcept { for (auto& t : session.tracks) if (t.live->mute.load()) return true; return false; }
bool Project::anyTrackSoloed() const noexcept { for (auto& t : session.tracks) if (t.live->solo.load()) return true; return false; }
void Project::clearAllMute() { for (auto& t : session.tracks) t.live->mute = false; liveChanged(); }
void Project::clearAllSolo() { for (auto& t : session.tracks) t.live->solo = false; liveChanged(); }

juce::String Project::editModeName (EditMode m)
{
    return m == EditMode::Ripple ? TRANS ("RIPPLE") : (m == EditMode::Shuffle ? TRANS ("SHUFFLE") : TRANS ("NORMALE"));
}

void Project::shuffleResolve()
{
    bool any = false;
    for (auto& t : session.tracks)
    {
        if (t.editLocked()) continue;
        std::vector<Clip*> order;
        for (auto& c : t.clips) if (! c.muted) order.push_back (&c);
        std::stable_sort (order.begin(), order.end(), [] (const Clip* x, const Clip* y) { return x->start < y->start; });
        int64_t edge = 0;
        for (auto* c : order)
        {
            if (c->start < edge) { c->start = edge; any = true; }
            edge = c->end();
        }
    }
    if (any) changed();
}

void Project::toggleClipMute (int64_t clipId)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked()) return;
    beginEdit();
    c->muted = ! c->muted;
    if (c->cueId != 0)
        if (auto* cue = findCue (c->cueId)) { if (! c->muted) cue->activeTake = c->takeNo; }   // riattivare un take lo rende quello attivo del cue
    changed();
}

void Project::eraseClip (int64_t clipId)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked()) return;
    const int64_t s0 = c->start, e0 = c->end();
    beginEdit();
    auto& clips = session.tracks[(size_t) ti].clips;
    clips.erase (clips.begin() + (c - clips.data()));
    if (editMode != EditMode::Normal)   // Ripple / Shuffle: il vuoto si chiude
        for (auto& o : clips) if (o.start >= e0) o.start -= (e0 - s0);
    selection.erase (clipId);
    changed();
}

namespace
{
/** Divide in 'pos' tutti i clip della traccia che lo contengono (stesso trattamento di splitAt). */
bool splitTrackAt (Track& t, int64_t pos, int64_t& nextId)
{
    bool any = false;
    const size_t n = t.clips.size();
    for (size_t ci = 0; ci < n; ++ci)
    {
        if (! (pos > t.clips[ci].start + 16 && pos < t.clips[ci].end() - 16)) continue;
        Clip right = t.clips[ci];
        const int64_t cut = pos - t.clips[ci].start;
        right.id = nextId++;
        right.start = pos;
        right.offset += cut;
        right.length -= cut;
        right.fadeIn = 0;
        right.fadeOut = juce::jmin (right.fadeOut, right.length);
        t.clips[ci].length = cut;
        t.clips[ci].fadeOut = 0;
        t.clips[ci].fadeIn = juce::jmin (t.clips[ci].fadeIn, cut);
        t.clips.push_back (right);
        any = true;
    }
    return any;
}
}

void Project::rangeSplit()
{
    if (! range.valid()) return;
    beginEdit();
    selection.clear();
    for (int i = range.first; i <= juce::jmin (range.last, (int) session.tracks.size() - 1); ++i)
    {
        auto& t = session.tracks[(size_t) i];
        if (t.editLocked() || t.hidden) continue;
        splitTrackAt (t, range.a, nextClipId);
        splitTrackAt (t, range.b, nextClipId);
        for (auto& c : t.clips) if (c.start >= range.a && c.end() <= range.b) selection.insert (c.id);
    }
    changed();
}

void Project::rangeDelete (bool ripple)
{
    if (! range.valid()) return;
    beginEdit();
    selection.clear();
    const int64_t a = range.a, b = range.b;
    for (int i = range.first; i <= juce::jmin (range.last, (int) session.tracks.size() - 1); ++i)
    {
        auto& t = session.tracks[(size_t) i];
        if (t.editLocked() || t.hidden) continue;
        splitTrackAt (t, a, nextClipId);
        splitTrackAt (t, b, nextClipId);
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [a, b] (const Clip& c) { return c.start >= a && c.end() <= b; }), t.clips.end());
        if (ripple) for (auto& c : t.clips) if (c.start >= b) c.start -= (b - a);
    }
    changed();
}

juce::String Project::insertTime (int64_t pos, int64_t len)
{
    if (len <= 0 || pos < 0) return TRANS ("Posizione o durata non valide.");
    for (auto& t : session.tracks) if (t.frozen != nullptr) return TRANS ("Ci sono tracce congelate: scongelale prima di cambiare il tempo.");
    beginEdit();
    selection.clear();
    range = {};
    auto mapPos = [=] (int64_t p) { return p >= pos ? p + len : p; };
    for (auto& t : session.tracks)
    {
        splitTrackAt (t, pos, nextClipId);
        for (auto& c : t.clips) if (c.start >= pos) c.start += len;
        for (auto* lane : { &t.volAuto, &t.panAuto, &t.sendAAuto, &t.sendBAuto }) for (auto& p : *lane) p.pos = mapPos (p.pos);
    }
    for (auto& c : session.cues) { c.inPos = mapPos (c.inPos); c.outPos = mapPos (c.outPos); }
    for (auto& m : session.markers) { m.pos = mapPos (m.pos); if (m.end >= 0) m.end = mapPos (m.end); }
    if (exportIn >= 0) { exportIn = mapPos (exportIn); exportOut = mapPos (exportOut); }
    changed();
    return {};
}

juce::String Project::removeTime (int64_t a, int64_t b)
{
    if (b <= a || a < 0) return TRANS ("Posizione o durata non valide.");
    for (auto& t : session.tracks) if (t.frozen != nullptr) return TRANS ("Ci sono tracce congelate: scongelale prima di cambiare il tempo.");
    beginEdit();
    selection.clear();
    range = {};
    const int64_t d = b - a;
    auto mapPos = [=] (int64_t p) { return p < a ? p : (p >= b ? p - d : a); };
    for (auto& t : session.tracks)
    {
        splitTrackAt (t, a, nextClipId);
        splitTrackAt (t, b, nextClipId);
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [a, b] (const Clip& c) { return c.start >= a && c.end() <= b; }), t.clips.end());
        for (auto& c : t.clips) if (c.start >= b) c.start -= d;
        for (auto* lane : { &t.volAuto, &t.panAuto, &t.sendAAuto, &t.sendBAuto })
        {
            lane->erase (std::remove_if (lane->begin(), lane->end(), [a, b] (const AutoPoint& p) { return p.pos >= a && p.pos < b; }), lane->end());
            for (auto& p : *lane) p.pos = mapPos (p.pos);
        }
    }
    for (auto& c : session.cues) { c.inPos = mapPos (c.inPos); c.outPos = mapPos (c.outPos); }
    for (auto& m : session.markers) { m.pos = mapPos (m.pos); if (m.end >= 0) m.end = mapPos (m.end); }
    if (exportIn >= 0) { exportIn = mapPos (exportIn); exportOut = mapPos (exportOut); }
    changed();
    return {};
}

void Project::rangeTrim()
{
    if (! range.valid()) return;
    beginEdit();
    selection.clear();
    const int64_t a = range.a, b = range.b;
    for (int i = range.first; i <= juce::jmin (range.last, (int) session.tracks.size() - 1); ++i)
    {
        auto& t = session.tracks[(size_t) i];
        if (t.editLocked() || t.hidden) continue;
        splitTrackAt (t, a, nextClipId);
        splitTrackAt (t, b, nextClipId);
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [a, b] (const Clip& c) { return c.end() <= a || c.start >= b; }), t.clips.end());
        for (auto& c : t.clips) selection.insert (c.id);
    }
    changed();
}

void Project::compRange (int trackId, int64_t clipId, int64_t a, int64_t b)
{
    const int ti = trackIndexForId (trackId);
    if (ti < 0 || b - a < 64 || session.tracks[(size_t) ti].editLocked()) return;
    auto& t = session.tracks[(size_t) ti];
    Clip* chosen = nullptr;
    for (auto& c : t.clips) if (c.id == clipId) chosen = &c;
    if (chosen == nullptr || chosen->asset == nullptr) return;
    // i pezzi di uno stesso clip condividono materiale e allineamento: e' la loro "origine"
    const auto asset = chosen->asset;
    const int64_t origin = chosen->start - chosen->offset;
    a = juce::jmax (a, chosen->start);
    b = juce::jmin (b, chosen->end());
    if (b - a < 64) return;

    beginEdit();
    splitTrackAt (t, a, nextClipId);
    splitTrackAt (t, b, nextClipId);
    const int64_t fade = juce::jmin<int64_t> ((int64_t) (0.005 * sampleRate), (b - a) / 4);   // 5 ms ai bordi del pezzo scelto
    for (auto& c : t.clips)
    {
        if (c.start < a || c.end() > b) continue;   // fuori dall'intervallo non cambia nulla
        const bool mine = c.asset == asset && c.start - c.offset == origin;
        c.muted = ! mine;
        if (mine)
        {
            if (c.start == a) c.fadeIn = juce::jmax (c.fadeIn, fade);
            if (c.end() == b) c.fadeOut = juce::jmax (c.fadeOut, fade);
        }
    }
    changed();
}

void Project::moveClipTo (int64_t clipId, int trackIndex, int64_t start)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked()) return;
    trackIndex = juce::jlimit (0, (int) session.tracks.size() - 1, trackIndex);
    if (session.tracks[(size_t) trackIndex].editLocked()) trackIndex = ti;   // non si sposta su una traccia bloccata
    c->start = juce::jmax<int64_t> (0, start);
    if (ti != trackIndex)
    {
        Clip copy = *c;
        auto& src = session.tracks[(size_t) ti].clips;
        src.erase (src.begin() + (c - src.data()));
        session.tracks[(size_t) trackIndex].clips.push_back (copy);
    }
}

void Project::splitAt (int64_t pos, bool allTracks)
{
    auto contains = [pos] (const Clip& c) { return pos > c.start + 16 && pos < c.end() - 16; };

    // se ci sono clip selezionate sotto la testina si dividono solo quelle, altrimenti tutte quelle sotto la testina
    bool onlySelected = false;
    if (! allTracks)
        for (auto& t : session.tracks)
            for (auto& c : t.clips)
                if (selection.count (c.id) > 0 && contains (c)) onlySelected = true;

    std::vector<std::pair<size_t, size_t>> targets;
    for (size_t ti = 0; ti < session.tracks.size(); ++ti)
        for (size_t ci = 0; ci < session.tracks[ti].clips.size() && ! session.tracks[ti].editLocked(); ++ci)
        {
            auto& c = session.tracks[ti].clips[ci];
            if (contains (c) && (! onlySelected || selection.count (c.id) > 0)) targets.emplace_back (ti, ci);
        }
    if (targets.empty()) return;

    beginEdit();
    const int64_t af = autoFadeSamples();
    for (auto [ti, ci] : targets)
    {
        auto& clips = session.tracks[ti].clips;
        Clip right = clips[ci];
        const bool wasSelected = selection.count (right.id) > 0;
        const int64_t cp = zeroCrossNear (clips[ci], pos);
        const int64_t cut = cp - clips[ci].start;
        right.id = nextClipId++;
        right.start = cp;
        right.offset += cut;
        right.length -= cut;
        right.fadeIn = af > 0 ? juce::jmin (af, right.length / 2) : 0;
        right.fadeOut = juce::jmin (right.fadeOut, right.length);
        clips[ci].length = cut;
        clips[ci].fadeOut = af > 0 ? juce::jmin (af, cut / 2) : 0;
        clips[ci].fadeIn = juce::jmin (clips[ci].fadeIn, cut);
        clips.push_back (right);
        if (wasSelected) selection.insert (right.id);
    }
    changed();
}

void Project::splitClip (int64_t id, int64_t pos)
{
    int ti = -1;
    auto* c = findClip (id, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked() || pos <= c->start + 16 || pos >= c->end() - 16) return;

    beginEdit();
    auto& clips = session.tracks[(size_t) ti].clips;
    const size_t idx = (size_t) (c - clips.data());
    Clip right = clips[idx];
    const int64_t af = autoFadeSamples();
    pos = zeroCrossNear (clips[idx], pos);
    const int64_t cut = pos - clips[idx].start;
    right.id = nextClipId++;
    right.start = pos;
    right.offset += cut;
    right.length -= cut;
    right.fadeIn = af > 0 ? juce::jmin (af, right.length / 2) : 0;
    right.fadeOut = juce::jmin (right.fadeOut, right.length);
    clips[idx].length = cut;
    clips[idx].fadeOut = af > 0 ? juce::jmin (af, cut / 2) : 0;
    clips[idx].fadeIn = juce::jmin (clips[idx].fadeIn, cut);
    clips.push_back (right);
    selection = { right.id };
    changed();
}

void Project::rippleDeleteSelected()
{
    if (selection.empty()) return;
    beginEdit();
    for (auto& t : session.tracks)
    {
        std::vector<std::pair<int64_t, int64_t>> removed;
        if (t.editLocked()) continue;
        for (auto& c : t.clips)
            if (selection.count (c.id) > 0) removed.emplace_back (c.start, c.end());
        if (removed.empty()) continue;

        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(),
                                       [this] (const Clip& c) { return selection.count (c.id) > 0; }), t.clips.end());
        std::sort (removed.begin(), removed.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        for (auto& [s0, e0] : removed)
            for (auto& c : t.clips)
                if (c.start >= e0) c.start -= (e0 - s0);
    }
    selection.clear();
    changed();
}

void Project::trimToPlayhead (bool trimStart, int64_t pos)
{
    auto inside = [pos] (const Clip& c) { return pos > c.start + 16 && pos < c.end() - 16; };
    bool onlySelected = false;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (selection.count (c.id) > 0 && inside (c)) onlySelected = true;

    bool any = false;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (! t.editLocked() && inside (c) && (! onlySelected || selection.count (c.id) > 0))
            {
                if (! any) { beginEdit(); any = true; }
                const int64_t cp = zeroCrossNear (c, pos);
                if (trimStart)
                {
                    const int64_t d = cp - c.start;
                    c.start = cp;
                    c.offset += d;
                    c.length -= d;
                    c.fadeIn = 0;
                    c.fadeOut = juce::jmin (c.fadeOut, c.length);
                }
                else
                {
                    c.length = cp - c.start;
                    c.fadeOut = 0;
                    c.fadeIn = juce::jmin (c.fadeIn, c.length);
                }
            }
    if (any) changed();
}

const TrackGroup* Project::findGroup (int groupId) const noexcept
{
    for (auto& g : session.groups) if (g.id == groupId) return &g;
    return nullptr;
}

int Project::addGroup (const juce::String& name)
{
    const auto n = name.trim().isEmpty() ? juce::String ("Gruppo") : name.trim();
    for (auto& g : session.groups) if (g.name.equalsIgnoreCase (n)) return g.id;
    beginEdit();
    int id = 1;
    for (auto& g : session.groups) id = juce::jmax (id, g.id + 1);
    session.groups.push_back ({ id, n, false });
    changed();
    return id;
}

void Project::setTrackGroup (int trackId, int groupId) { setTracksGroup ({ trackId }, groupId); }

void Project::setTracksGroup (const std::vector<int>& trackIds, int groupId)
{
    if (groupId != 0 && findGroup (groupId) == nullptr) return;
    auto& tracks = session.tracks;
    std::vector<int> ids;   // nell'ordine della timeline, solo le tracce che cambiano
    for (auto& t : tracks)
        if (t.groupId != groupId && std::find (trackIds.begin(), trackIds.end(), t.id) != trackIds.end()) ids.push_back (t.id);
    if (ids.empty()) return;
    beginEdit();
    for (int id : ids)
    {
        const int from = trackIndexForId (id);
        tracks[(size_t) from].groupId = groupId;
        if (groupId == 0) continue;
        int last = -1;   // accanto all'ultima traccia del gruppo, cosi' il gruppo resta compatto
        for (int i = 0; i < (int) tracks.size(); ++i) if (i != from && tracks[(size_t) i].groupId == groupId) last = i;
        if (last < 0) continue;
        auto t = std::move (tracks[(size_t) from]);
        tracks.erase (tracks.begin() + from);
        if (from < last) --last;
        tracks.insert (tracks.begin() + last + 1, std::move (t));
    }
    if (groupId != 0)   // le tracce appena aggiunte non devono restare nascoste in un gruppo compresso
        for (auto& g : session.groups) if (g.id == groupId) g.collapsed = false;
    changed();
}

std::vector<int> Project::selectedTracks() const
{
    std::vector<int> out;
    if (trackSelection.count (selectedTrackId) > 0)
        for (auto& t : session.tracks) if (trackSelection.count (t.id) > 0) out.push_back (t.id);
    if (out.empty() && trackIndexForId (selectedTrackId) >= 0) out.push_back (selectedTrackId);
    return out;
}

bool Project::isTrackSelected (int trackId) const
{
    const auto s = selectedTracks();
    return std::find (s.begin(), s.end(), trackId) != s.end();
}

void Project::selectOnlyTrack (int trackId)
{
    selectedTrackId = trackAnchorId = trackId;
    trackSelection = { trackId };
}

void Project::toggleTrackSelection (int trackId)
{
    auto cur = selectedTracks();
    trackSelection = std::set<int> (cur.begin(), cur.end());
    if (trackSelection.count (trackId) > 0 && trackSelection.size() > 1)
    {
        trackSelection.erase (trackId);
        if (selectedTrackId == trackId) selectedTrackId = *trackSelection.begin();
    }
    else
    {
        trackSelection.insert (trackId);
        selectedTrackId = trackId;
    }
    trackAnchorId = trackId;
}

void Project::selectTrackRange (int trackId)
{
    const int a = trackIndexForId (trackAnchorId >= 0 ? trackAnchorId : selectedTrackId), b = trackIndexForId (trackId);
    if (a < 0 || b < 0) { selectOnlyTrack (trackId); return; }
    trackSelection.clear();
    for (int i = juce::jmin (a, b); i <= juce::jmax (a, b); ++i) trackSelection.insert (session.tracks[(size_t) i].id);
    selectedTrackId = trackId;
}

void Project::renameGroup (int groupId, const juce::String& name)
{
    for (auto& g : session.groups)
        if (g.id == groupId && name.trim().isNotEmpty()) { beginEdit(); g.name = name.trim(); changed(); return; }
}

void Project::removeGroup (int groupId)
{
    beginEdit();
    for (auto& t : session.tracks) if (t.groupId == groupId) t.groupId = 0;
    session.groups.erase (std::remove_if (session.groups.begin(), session.groups.end(), [groupId] (const TrackGroup& g) { return g.id == groupId; }), session.groups.end());
    changed();
}

void Project::setGroupCollapsed (int groupId, bool collapsed)
{
    for (auto& g : session.groups) if (g.id == groupId) g.collapsed = collapsed;
    markDirty();
    sendChangeMessage();
}

void Project::setGroupMuteSolo (int groupId, bool solo, bool on)
{
    for (auto& t : session.tracks)
        if (t.groupId == groupId) (solo ? t.live->solo : t.live->mute) = on;
    liveChanged();
}

void Project::moveTrackTo (int from, int to)
{
    const int n = (int) session.tracks.size();
    if (from < 0 || from >= n) return;
    to = juce::jlimit (0, n - 1, to);
    if (from == to) return;
    beginEdit();
    auto t = std::move (session.tracks[(size_t) from]);
    session.tracks.erase (session.tracks.begin() + from);
    session.tracks.insert (session.tracks.begin() + to, std::move (t));
    // la traccia entra nel gruppo dei vicini; se i vicini divergono resta nel suo gruppo solo se uno dei due e' lo stesso
    auto& tracks = session.tracks;
    const int prev = to > 0 ? tracks[(size_t) to - 1].groupId : 0, next = to + 1 < n ? tracks[(size_t) to + 1].groupId : 0;
    auto& g = tracks[(size_t) to].groupId;
    g = prev == next ? prev : ((g != 0 && (g == prev || g == next)) ? g : 0);
    changed();
}

void Project::duplicateTrack (int id)
{
    const int idx = trackIndexForId (id);
    if (idx < 0) return;
    beginEdit();
    Track copy = session.tracks[(size_t) idx];
    copy.id = nextTrackId++;
    copy.name += " copia";
    copy.live = std::make_shared<TrackLive>();
    copy.live->copyFrom (*session.tracks[(size_t) idx].live);   // i plug-in non si duplicano: un'istanza non si puo' condividere
    copy.live->armed = false;
    for (auto& c : copy.clips) c.id = nextClipId++;
    session.tracks.insert (session.tracks.begin() + idx + 1, std::move (copy));
    selectedTrackId = session.tracks[(size_t) idx + 1].id;
    changed();
}

void Project::deleteSelected()
{
    if (selection.empty()) return;
    if (editMode != EditMode::Normal) { rippleDeleteSelected(); return; }   // in Ripple e Shuffle eliminare chiude il vuoto
    beginEdit();
    for (auto& t : session.tracks)
        if (! t.editLocked())
            t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(),
                                           [this] (const Clip& c) { return selection.count (c.id) > 0; }),
                           t.clips.end());
    selection.clear();
    changed();
}

void Project::duplicateSelected()
{
    if (selection.empty()) return;
    beginEdit();
    std::set<int64_t> newSel;
    // sposta le copie dopo l'intero blocco selezionato
    int64_t blockStart = std::numeric_limits<int64_t>::max(), blockEnd = 0;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (selection.count (c.id)) { blockStart = juce::jmin (blockStart, c.start); blockEnd = juce::jmax (blockEnd, c.end()); }
    const int64_t shift = blockEnd - blockStart;
    for (auto& t : session.tracks)
    {
        std::vector<Clip> add;
        for (auto& c : t.clips)
            if (selection.count (c.id) && ! t.editLocked())
            {
                Clip n = c;
                n.id = nextClipId++;
                n.start += shift;
                add.push_back (n);
                newSel.insert (n.id);
            }
        t.clips.insert (t.clips.end(), add.begin(), add.end());
    }
    selection = newSel;
    changed();
}

void Project::selectAll()
{
    selection.clear();
    for (auto& t : session.tracks)
        for (auto& c : t.clips) selection.insert (c.id);
    sendChangeMessage();
}

void Project::normalizeClip (int64_t id)
{
    int nti = -1;
    auto* c = findClip (id, &nti);
    if (c == nullptr || c->asset == nullptr || session.tracks[(size_t) nti].editLocked()) return;
    float peak = 0.0f;
    if (c->asset->offline) return;
    for (int ch = 0; ch < c->asset->numChannels(); ++ch)
        peak = juce::jmax (peak, c->asset->audio.getMagnitude (ch, (int) c->offset, (int) c->length));
    if (peak <= 1.0e-5f) return;
    beginEdit();
    c->gainDb = juce::Decibels::gainToDecibels (juce::Decibels::decibelsToGain (-1.0f) / peak);
    changed();
}

void Project::stretchClip (int64_t id, double ratio, bool keepPitch, const juce::File& folder, std::function<void (juce::String)> done, int64_t newStart)
{
    int sti = -1;
    auto* c = findClip (id, &sti);
    if (c == nullptr || c->asset == nullptr || c->length <= 0) { if (done) done ("Clip non valido."); return; }
    if (session.tracks[(size_t) sti].editLocked()) { if (done) done (TRANS ("La traccia e' bloccata: sbloccala per modificare i suoi clip.")); return; }
    if (c->asset->offline) { if (done) done (TRANS ("Il file di questo clip non e' stato trovato: ricollegalo con File > Ricollega i media.")); return; }
    ratio = juce::jlimit (0.25, 4.0, ratio);
    const auto src = c->asset;
    const int off = (int) c->offset, len = (int) c->length;
    const double rate = sampleRate;
    auto* fm = &formats;
    const auto base = juce::File::createLegalFileName (src->name + "_x" + juce::String ((int) std::lround (ratio * 100.0)));

    runAsync ([=, this]() -> std::function<void()>
    {
        auto stretched = keepPitch ? timeStretch (src->audio, off, len, ratio, rate) : varispeed (src->audio, off, len, ratio);
        folder.createDirectory();
        const auto file = folder.getNonexistentChildFile (base, ".wav", false);
        juce::String err;
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w;
            if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) stretched.getNumChannels(), 32, {}, 0));
            if (w == nullptr) err = "Impossibile scrivere " + file.getFullPathName();
            else { os.release(); w->writeFromAudioSampleBuffer (stretched, 0, stretched.getNumSamples()); }
        }
        AssetPtr a = err.isEmpty() ? loadAsset (*fm, file, rate, err) : nullptr;

        return [=, this]
        {
            auto* cl = findClip (id);
            if (a == nullptr || cl == nullptr) { if (done) done (err.isNotEmpty() ? err : juce::String ("Il clip non c'e' piu'.")); return; }
            beginEdit();
            if (! cl->env.empty())   // l'inviluppo segue il materiale allungato
            {
                std::vector<GainPoint> ne { { 0, envDbAt (cl->env, cl->offset) } };
                for (auto& gp : cl->env)
                    if (gp.pos > cl->offset && gp.pos < cl->offset + len) ne.push_back ({ (int64_t) std::llround ((double) (gp.pos - cl->offset) * ratio), gp.db });
                ne.push_back ({ a->length(), envDbAt (cl->env, cl->offset + len) });
                cl->env = std::move (ne);
            }
            cl->syncPoint = cl->hasSync() ? (int64_t) std::llround ((double) (cl->syncPoint - cl->offset) * ratio) : -1;
            cl->asset = a;
            if (newStart >= 0) cl->start = newStart;   // stretch dal bordo sinistro: anche l'inizio si sposta
            cl->offset = 0;
            cl->length = a->length();
            cl->fadeIn = (int64_t) std::llround ((double) cl->fadeIn * ratio);
            cl->fadeOut = (int64_t) std::llround ((double) cl->fadeOut * ratio);
            changed();
            if (done) done ({});
        };
    });
}

int Project::addEnvPoint (int64_t clipId, int64_t timelinePos, float db)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked() || c->asset == nullptr) return -1;
    beginEdit();
    c = findClip (clipId);
    const int64_t p = juce::jlimit (c->offset, c->offset + c->length, c->offset + (timelinePos - c->start));
    if (c->env.empty()) c->env = { { c->offset, 0.0f }, { c->offset + c->length, 0.0f } };   // il primo punto crea l'inviluppo piatto a 0 dB agli estremi
    GainPoint gp { p, juce::jlimit (-60.0f, 12.0f, db) };
    auto it = std::lower_bound (c->env.begin(), c->env.end(), gp, [] (const GainPoint& a, const GainPoint& b) { return a.pos < b.pos; });
    if (it != c->env.end() && it->pos == gp.pos) { it->db = gp.db; changed(); return (int) (it - c->env.begin()); }
    it = c->env.insert (it, gp);
    changed();
    return (int) (it - c->env.begin());
}

void Project::moveEnvPoint (int64_t clipId, int index, int64_t timelinePos, float db)
{
    auto* c = findClip (clipId);
    if (c == nullptr || index < 0 || index >= (int) c->env.size()) return;
    const int64_t lo = index > 0 ? c->env[(size_t) index - 1].pos + 1 : c->offset;
    const int64_t hi = index + 1 < (int) c->env.size() ? c->env[(size_t) index + 1].pos - 1 : c->offset + c->length;
    c->env[(size_t) index].pos = juce::jlimit (lo, juce::jmax (lo, hi), c->offset + (timelinePos - c->start));
    c->env[(size_t) index].db = juce::jlimit (-60.0f, 12.0f, db);
    changed();
}

void Project::removeEnvPoint (int64_t clipId, int index)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked() || index < 0 || index >= (int) c->env.size()) return;
    beginEdit();
    c = findClip (clipId);
    c->env.erase (c->env.begin() + index);
    if (std::all_of (c->env.begin(), c->env.end(), [] (const GainPoint& g) { return std::abs (g.db) < 0.05f; })) c->env.clear();   // tutto a 0 dB: e' come non averlo
    changed();
}

void Project::clearEnvelope (int64_t clipId)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || c->env.empty() || session.tracks[(size_t) ti].editLocked()) return;
    beginEdit();
    findClip (clipId)->env.clear();
    changed();
}

bool Project::setSyncPoint (int64_t clipId, int64_t pos)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr || session.tracks[(size_t) ti].editLocked()) return false;
    if (pos >= 0 && (pos < c->start || pos >= c->end())) return false;
    beginEdit();
    c = findClip (clipId);
    c->syncPoint = pos < 0 ? -1 : c->offset + (pos - c->start);
    changed();
    return true;
}

bool Project::moveSyncTo (int64_t clipId, int64_t pos)
{
    int ti = -1;
    const auto* c = findClip (clipId, &ti);
    if (c == nullptr || ! c->hasSync() || session.tracks[(size_t) ti].editLocked()) return false;
    const int64_t ns = c->start + (pos - c->syncTime());
    if (ns < 0) return false;
    beginEdit();
    findClip (clipId)->start = ns;
    changed();
    return true;
}

int Project::sliceClip (int64_t id, std::vector<int64_t> positions)
{
    int ti = -1;
    auto* c0 = findClip (id, &ti);
    if (c0 == nullptr || session.tracks[(size_t) ti].editLocked()) return 0;
    std::sort (positions.begin(), positions.end());
    std::vector<int64_t> cuts;
    for (auto p : positions)
    {
        p = zeroCrossNear (*c0, p);
        if (p > c0->start + 16 && p < c0->end() - 16 && (cuts.empty() || p > cuts.back() + 16)) cuts.push_back (p);
    }
    if (cuts.empty()) return 0;
    beginEdit();
    auto& clips = session.tracks[(size_t) ti].clips;
    const size_t idx = (size_t) (findClip (id) - clips.data());
    const Clip orig = clips[idx];
    const int64_t af = autoFadeSamples();
    std::vector<Clip> pieces;
    int64_t s0 = orig.start;
    for (size_t k = 0; k <= cuts.size(); ++k)
    {
        const int64_t s1 = k < cuts.size() ? cuts[k] : orig.end();
        Clip piece = orig;
        piece.id = k == 0 ? orig.id : nextClipId++;
        piece.start = s0;
        piece.offset = orig.offset + (s0 - orig.start);
        piece.length = s1 - s0;
        piece.fadeIn = k == 0 ? juce::jmin (orig.fadeIn, piece.length) : (af > 0 ? juce::jmin (af, piece.length / 2) : 0);
        piece.fadeOut = k == cuts.size() ? juce::jmin (orig.fadeOut, piece.length) : (af > 0 ? juce::jmin (af, piece.length / 2) : 0);
        pieces.push_back (piece);
        s0 = s1;
    }
    clips[idx] = pieces.front();
    clips.insert (clips.end(), pieces.begin() + 1, pieces.end());
    selection.clear();
    for (auto& p : pieces) selection.insert (p.id);
    changed();
    return (int) cuts.size();
}

void Project::processRange (int64_t id, int64_t from, int64_t to, RangeOp op, double param, const juce::File& folder, std::function<void (juce::String)> done)
{
    int sti = -1;
    auto* c = findClip (id, &sti);
    if (c == nullptr || c->asset == nullptr || c->length <= 0) { if (done) done ("Clip non valido."); return; }
    if (session.tracks[(size_t) sti].editLocked()) { if (done) done (TRANS ("La traccia e' bloccata: sbloccala per modificare i suoi clip.")); return; }
    if (c->asset->offline) { if (done) done (TRANS ("Il file di questo clip non e' stato trovato: ricollegalo con File > Ricollega i media.")); return; }
    from = juce::jlimit<int64_t> (0, c->length, from);
    to = juce::jlimit<int64_t> (0, c->length, to);
    if (to - from < 2) { if (done) done (TRANS ("Seleziona prima una porzione di audio.")); return; }
    const auto src = c->asset;
    const int off = (int) c->offset, len = (int) c->length, a = (int) from, b = (int) to;
    const double rate = sampleRate;
    auto* fm = &formats;
    const auto base = juce::File::createLegalFileName (src->name + "_ed");

    runAsync ([=, this]() -> std::function<void()>
    {
        const int nch = src->audio.getNumChannels();
        juce::AudioBuffer<float> buf (nch, len);
        for (int ch = 0; ch < nch; ++ch) buf.copyFrom (ch, 0, src->audio, ch, off, len);
        const int n = b - a;
        juce::String err;
        float gain = 1.0f;
        if (op == RangeOp::Gain) gain = juce::Decibels::decibelsToGain ((float) param);
        if (op == RangeOp::Normalize)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < nch; ++ch) peak = juce::jmax (peak, buf.getMagnitude (ch, a, n));
            if (peak < 1.0e-5f) err = TRANS ("La selezione e' silenzio: non c'e' nulla da normalizzare.");
            else gain = juce::jmin (100.0f, juce::Decibels::decibelsToGain ((float) param) / peak);   // param = picco di arrivo in dBFS
        }
        for (int ch = 0; ch < nch && err.isEmpty(); ++ch)
        {
            float* d = buf.getWritePointer (ch) + a;
            switch (op)
            {
                case RangeOp::Gain: case RangeOp::Normalize: juce::FloatVectorOperations::multiply (d, gain, n); break;
                case RangeOp::FadeIn:  for (int i = 0; i < n; ++i) d[i] *= (float) i / (float) n; break;
                case RangeOp::FadeOut: for (int i = 0; i < n; ++i) d[i] *= 1.0f - (float) i / (float) n; break;
                case RangeOp::Silence: juce::FloatVectorOperations::clear (d, n); break;
                case RangeOp::Reverse: std::reverse (d, d + n); break;
                case RangeOp::Invert:  juce::FloatVectorOperations::negate (d, d, n); break;
                case RangeOp::RemoveDc:
                {
                    double mean = 0.0;
                    for (int i = 0; i < n; ++i) mean += d[i];
                    juce::FloatVectorOperations::add (d, (float) (-mean / n), n);
                    break;
                }
            }
        }
        AssetPtr asset;
        if (err.isEmpty())
        {
            folder.createDirectory();
            const auto file = folder.getNonexistentChildFile (base, ".wav", false);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w;
            if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) nch, 32, {}, 0));
            if (w == nullptr) err = "Impossibile scrivere " + file.getFullPathName();
            else { os.release(); w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples()); w.reset(); asset = loadAsset (*fm, file, rate, err); }
        }

        return [=, this]
        {
            auto* cl = findClip (id);
            if (asset == nullptr || cl == nullptr) { if (done) done (err.isNotEmpty() ? err : juce::String ("Il clip non c'e' piu'.")); return; }
            beginEdit();
            for (auto& gp : cl->env) gp.pos -= cl->offset;      // il nuovo file comincia dove comincia il clip
            if (cl->syncPoint >= 0) cl->syncPoint -= cl->offset;
            cl->asset = asset;
            cl->offset = 0;
            changed();
            if (done) done ({});
        };
    });
}

void Project::warpClip (int64_t id, std::vector<warp::Anchor> anchors, const juce::File& folder, std::function<void (juce::String)> done, int64_t newStart)
{
    int sti = -1;
    auto* c = findClip (id, &sti);
    if (c == nullptr || c->asset == nullptr || c->length <= 0) { if (done) done ("Clip non valido."); return; }
    if (session.tracks[(size_t) sti].editLocked()) { if (done) done (TRANS ("La traccia e' bloccata: sbloccala per modificare i suoi clip.")); return; }
    if (c->asset->offline) { if (done) done (TRANS ("Il file di questo clip non e' stato trovato: ricollegalo con File > Ricollega i media.")); return; }
    const auto src = c->asset;
    const int off = (int) c->offset, len = (int) c->length;
    const double rate = sampleRate;
    anchors = warp::normalise (std::move (anchors), len, rate);
    if (anchors.empty()) { if (done) done (TRANS ("Nessun punto di ancoraggio spostato.")); return; }
    auto* fm = &formats;
    const auto base = juce::File::createLegalFileName (src->name + "_warp");

    runAsync ([=, this]() -> std::function<void()>
    {
        auto warped = warp::render (src->audio, off, len, anchors, rate);
        folder.createDirectory();
        const auto file = folder.getNonexistentChildFile (base, ".wav", false);
        juce::String err;
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w;
            if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) warped.getNumChannels(), 32, {}, 0));
            if (w == nullptr) err = "Impossibile scrivere " + file.getFullPathName();
            else { os.release(); w->writeFromAudioSampleBuffer (warped, 0, warped.getNumSamples()); }
        }
        AssetPtr a = err.isEmpty() ? loadAsset (*fm, file, rate, err) : nullptr;

        return [=, this]
        {
            auto* cl = findClip (id);
            if (a == nullptr || cl == nullptr) { if (done) done (err.isNotEmpty() ? err : juce::String ("Il clip non c'e' piu'.")); return; }
            beginEdit();
            cl->syncPoint = cl->hasSync() ? warp::mapPosition (anchors, len, cl->syncPoint - cl->offset) : -1;
            if (! cl->env.empty())
            {
                std::vector<GainPoint> ne { { 0, envDbAt (cl->env, cl->offset) } };
                for (auto& gp : cl->env)
                    if (gp.pos > cl->offset && gp.pos < cl->offset + len) ne.push_back ({ warp::mapPosition (anchors, len, gp.pos - cl->offset), gp.db });
                ne.push_back ({ a->length(), envDbAt (cl->env, cl->offset + len) });
                cl->env = std::move (ne);
            }
            cl->asset = a;
            if (newStart >= 0) cl->start = newStart;
            cl->offset = 0;
            cl->length = a->length();
            cl->fadeIn = juce::jmin (cl->fadeIn, cl->length);
            cl->fadeOut = juce::jmin (cl->fadeOut, cl->length);
            changed();
            if (done) done ({});
        };
    });
}

int Project::joinableGroups (const std::set<int64_t>& ids) const
{
    int groups = 0;
    for (auto& t : session.tracks)
    {
        int n = 0;
        for (auto& c : t.clips) if (! t.editLocked() && ids.count (c.id) > 0 && ! c.muted && c.asset != nullptr && ! c.asset->offline) ++n;
        if (n >= 2) ++groups;
    }
    return groups;
}

void Project::joinClips (const std::vector<int64_t>& ids, const juce::File& folder, std::function<void (juce::String)> done)
{
    struct Job { int trackId; std::vector<Clip> clips; int64_t start = 0, length = 0; int channels = 1; juce::String base; };
    auto jobs = std::make_shared<std::vector<Job>>();
    session.computeCrossfades();
    for (auto& t : session.tracks)
    {
        Job j;
        j.trackId = t.id;
        if (t.editLocked()) continue;
        for (auto& c : t.clips)
            if (std::find (ids.begin(), ids.end(), c.id) != ids.end() && ! c.muted && c.asset != nullptr && ! c.asset->offline && c.length > 0) j.clips.push_back (c);
        if (j.clips.size() < 2) continue;
        std::sort (j.clips.begin(), j.clips.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
        int64_t end = 0;
        j.start = j.clips.front().start;
        for (auto& c : j.clips) { end = juce::jmax (end, c.end()); j.channels = juce::jmax (j.channels, c.asset->numChannels()); }
        j.length = end - j.start;
        j.base = juce::File::createLegalFileName (j.clips.front().asset->name + "_join");
        jobs->push_back (std::move (j));
    }
    if (jobs->empty()) { if (done) done (TRANS ("Seleziona almeno due clip sulla stessa traccia (i clip silenziati non si uniscono).")); return; }

    const double rate = sampleRate;
    auto* fm = &formats;
    runAsync ([=, this]() -> std::function<void()>
    {
        auto assets = std::make_shared<std::vector<AssetPtr>>();
        juce::String err;
        for (auto& j : *jobs)
        {
            if (j.length <= 0 || j.length > (int64_t) 2000000000 / juce::jmax (1, j.channels)) { err = TRANS ("L'unione e' troppo lunga."); assets->push_back (nullptr); continue; }
            juce::AudioBuffer<float> out (j.channels, (int) j.length);
            out.clear();
            for (auto& c : j.clips)   // stesso calcolo di fade e crossfade del mixer
            {
                const int nch = c.asset->numChannels();
                const float gain = juce::Decibels::decibelsToGain (c.gainDb);
                const int64_t dst0 = c.start - j.start;
                const int64_t cnt = juce::jmin (c.length, c.asset->length() - c.offset);
                for (int ch = 0; ch < j.channels; ++ch)
                {
                    const float* src = c.asset->audio.getReadPointer (juce::jmin (ch, nch - 1)) + c.offset;
                    float* dst = out.getWritePointer (ch) + dst0;
                    std::vector<float> envG;
                    if (! c.env.empty()) { envG.resize ((size_t) cnt); fillEnvGain (c.env, c.offset, (int) cnt, envG.data()); }
                    for (int64_t i = 0; i < cnt; ++i)
                    {
                        float g = gain;
                        if (! envG.empty()) g *= envG[(size_t) i];
                        if (c.fadeIn > 0 && i < c.fadeIn) g *= (float) i / (float) c.fadeIn;
                        const int64_t toEnd = c.length - i;
                        if (c.fadeOut > 0 && toEnd < c.fadeOut) g *= (float) toEnd / (float) c.fadeOut;
                        if (c.xfadeIn > 0 && i < c.xfadeIn) { const float x = (float) i / (float) c.xfadeIn; g *= c.xLinear ? x : std::sin (x * juce::MathConstants<float>::halfPi); }
                        if (c.xfadeOut > 0 && toEnd < c.xfadeOut) { const float x = (float) toEnd / (float) c.xfadeOut; g *= c.xLinear ? x : std::sin (x * juce::MathConstants<float>::halfPi); }
                        dst[i] += src[i] * g;
                    }
                }
            }
            folder.createDirectory();
            const auto file = folder.getNonexistentChildFile (j.base, ".wav", false);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w;
            if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) j.channels, 32, {}, 0));
            if (w == nullptr) { err = "Impossibile scrivere " + file.getFullPathName(); assets->push_back (nullptr); continue; }
            os.release();
            w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
            w.reset();
            juce::String e2;
            assets->push_back (loadAsset (*fm, file, rate, e2));
            if (assets->back() == nullptr) err = e2;
        }

        return [=, this]
        {
            bool any = false;
            selection.clear();
            for (size_t k = 0; k < jobs->size(); ++k)
            {
                auto& j = (*jobs)[k];
                auto* t = findTrack (j.trackId);
                if ((*assets)[k] == nullptr || t == nullptr) continue;
                if (! any) { beginEdit(); any = true; t = findTrack (j.trackId); }
                Clip nc;
                nc.id = nextClipId++;
                nc.asset = (*assets)[k];
                nc.start = j.start;
                nc.length = nc.asset->length();
                nc.fadeIn = j.clips.front().fadeIn;
                nc.fadeOut = j.clips.back().fadeOut;
                bool sameTake = true;   // se erano tutti lo stesso take ADR, il risultato lo resta
                for (auto& c : j.clips) sameTake = sameTake && c.cueId == j.clips.front().cueId && c.takeNo == j.clips.front().takeNo;
                if (sameTake) { nc.cueId = j.clips.front().cueId; nc.takeNo = j.clips.front().takeNo; }
                t->clips.erase (std::remove_if (t->clips.begin(), t->clips.end(), [&j] (const Clip& c)
                {
                    for (auto& o : j.clips) if (o.id == c.id) return true;
                    return false;
                }), t->clips.end());
                t->clips.push_back (nc);
                selection.insert (nc.id);
            }
            if (any) changed();
            if (done) done (any ? juce::String() : (err.isNotEmpty() ? err : juce::String ("Unione non riuscita.")));
        };
    });
}

void Project::setClipGain (int64_t id, float db)
{
    int gti = -1;
    if (auto* c = findClip (id, &gti); c != nullptr && ! session.tracks[(size_t) gti].editLocked())
    {
        beginEdit();
        c->gainDb = db;
        changed();
    }
}

//==============================================================================
namespace
{
/** Serializza la catena di plug-in di una traccia (descrizione, bypass e stato). */
juce::var insertsToVar (const TrackLive& live)
{
    juce::Array<juce::var> arr;
    if (auto chain = live.getChain())
        for (auto& slot : chain->slots)
        {
            auto* o = new juce::DynamicObject();
            if (auto xml = slot->desc.createXml()) o->setProperty ("desc", xml->toString());
            o->setProperty ("bypass", slot->bypass.load());
            juce::String state = slot->initialState;
            if (slot->ready.load() && slot->plugin != nullptr)
            {
                juce::MemoryBlock mb;
                slot->plugin->getStateInformation (mb);
                state = mb.toBase64Encoding();
            }
            o->setProperty ("state", state);
            arr.add (juce::var (o));
        }
    // plug-in non ancora ripristinati
    for (auto& p : live.pending)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("desc", p.descXml);
        o->setProperty ("bypass", p.bypass);
        o->setProperty ("state", p.stateB64);
        arr.add (juce::var (o));
    }
    return arr;
}

void insertsFromVar (const juce::var& v, TrackLive& live)
{
    if (auto* arr = v.getArray())
        for (auto& e : *arr)
            live.pending.push_back ({ e.getProperty ("desc", "").toString(), e.getProperty ("state", "").toString(), (bool) e.getProperty ("bypass", false) });
}
} // namespace

juce::var Project::toVar() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("sampleRate", sampleRate);
    root->setProperty ("masterDb", (double) session.master->volumeDb.load());
    root->setProperty ("loudnessStandard", loudnessStandard);
    root->setProperty ("tcFormat", tcFormat);
    root->setProperty ("tcStart", tcStartSec);
    root->setProperty ("rulerTimecode", rulerTimecode);
    root->setProperty ("adrPreRoll", adrPreRoll);
    root->setProperty ("adrPostRoll", adrPostRoll);
    root->setProperty ("adrBeeps", adrBeeps);
    root->setProperty ("adrStreamer", adrStreamer);
    root->setProperty ("adrOverlay", adrOverlay);
    root->setProperty ("adrDialogueSize", adrDialogueSize);
    root->setProperty ("adrNaming", adrNaming);
    root->setProperty ("exportIn", (juce::int64) exportIn);
    root->setProperty ("exportOut", (juce::int64) exportOut);
    root->setProperty ("limiterOn", session.master->limiterOn.load());
    root->setProperty ("ceilingDb", (double) session.master->ceilingDb.load());
    root->setProperty ("masterInserts", insertsToVar (*session.master));
    {
        juce::Array<juce::var> auxes;
        for (int k = 0; k < 2; ++k)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("volumeDb", (double) session.aux[k]->volumeDb.load());
            o->setProperty ("mute", session.aux[k]->mute.load());
            o->setProperty ("inserts", insertsToVar (*session.aux[k]));
            auxes.add (juce::var (o));
        }
        root->setProperty ("aux", auxes);
        juce::Array<juce::var> groups;
        for (int k = 0; k < Session::kNumGroups; ++k)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("volumeDb", (double) session.group[k]->volumeDb.load());
            o->setProperty ("mute", session.group[k]->mute.load());
            o->setProperty ("inserts", insertsToVar (*session.group[k]));
            groups.add (juce::var (o));
        }
        root->setProperty ("groups", groups);
    }
    {
        juce::Array<juce::var> cueArr;
        for (auto& c : session.cues)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", c.id);
            o->setProperty ("number", c.number);
            o->setProperty ("character", c.character);
            o->setProperty ("scene", c.scene);
            o->setProperty ("reel", c.reel);
            o->setProperty ("dialogue", c.dialogue);
            o->setProperty ("notes", c.notes);
            o->setProperty ("in", (juce::int64) c.inPos);
            o->setProperty ("out", (juce::int64) c.outPos);
            o->setProperty ("status", c.status);
            o->setProperty ("activeTake", c.activeTake);
            cueArr.add (juce::var (o));
        }
        root->setProperty ("adrCues", cueArr);
    }
    {
        juce::Array<juce::var> marks;
        for (auto& m : session.markers)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("pos", (juce::int64) m.pos);
            if (m.end > m.pos) o->setProperty ("end", (juce::int64) m.end);
            o->setProperty ("name", m.name);
            o->setProperty ("colour", m.colour.toString());
            marks.add (juce::var (o));
        }
        root->setProperty ("markers", marks);
        if (notes.isNotEmpty()) root->setProperty ("notes", notes);
    }
    if (session.video != nullptr)
    {
        root->setProperty ("video", session.video->file.getFullPathName());
        root->setProperty ("videoOffset", session.video->offsetSec);
    }
    else if (missingVideoPath.isNotEmpty())   // video non trovato all'apertura: il percorso non va perso
    {
        root->setProperty ("video", missingVideoPath);
        root->setProperty ("videoOffset", missingVideoOffset);
    }

    auto clipsToVar = [] (const std::vector<Clip>& list)
    {
        juce::Array<juce::var> clips;
        for (auto& c : list)
        {
            auto* co = new juce::DynamicObject();
            co->setProperty ("file", c.asset->file.getFullPathName());
            co->setProperty ("start", (juce::int64) c.start);
            co->setProperty ("offset", (juce::int64) c.offset);
            co->setProperty ("length", (juce::int64) c.length);
            co->setProperty ("gainDb", (double) c.gainDb);
            co->setProperty ("fadeIn", (juce::int64) c.fadeIn);
            co->setProperty ("fadeOut", (juce::int64) c.fadeOut);
            if (c.muted) co->setProperty ("muted", true);
            if (c.invert) co->setProperty ("invert", true);
            if (c.syncPoint >= 0) co->setProperty ("sync", (juce::int64) c.syncPoint);
            if (! c.env.empty())
            {
                juce::Array<juce::var> pts;
                for (auto& gp : c.env) { juce::Array<juce::var> one; one.add ((juce::int64) gp.pos); one.add (gp.db); pts.add (juce::var (one)); }
                co->setProperty ("env", pts);
            }
            if (c.xShape != 0) co->setProperty ("xShape", c.xShape);
            if (c.cueId != 0) { co->setProperty ("cueId", c.cueId); co->setProperty ("takeNo", c.takeNo); if (c.rating > 0) co->setProperty ("rating", c.rating); if (c.rejected) co->setProperty ("rejected", true); }
            clips.add (juce::var (co));
        }
        return clips;
    };
    juce::Array<juce::var> tracks;
    for (auto& t : session.tracks)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", t.name);
        o->setProperty ("group", t.groupId);
        if (t.locked) o->setProperty ("locked", true);
        if (t.frozen != nullptr) o->setProperty ("frozenFile", t.frozen->file.getFullPathName());
        if (t.hidden) o->setProperty ("hidden", true);
        if (t.showLanes) o->setProperty ("lanes", true);
        o->setProperty ("colour", t.colour.toString());
        o->setProperty ("volumeDb", (double) t.live->volumeDb.load());
        o->setProperty ("pan", (double) t.live->pan.load());
        o->setProperty ("mute", t.live->mute.load());
        o->setProperty ("solo", t.live->solo.load());
        o->setProperty ("inputMode", t.live->inputMode.load());
        o->setProperty ("height", t.live->height.load());
        o->setProperty ("inserts", insertsToVar (*t.live));
        o->setProperty ("autoMode", t.live->autoMode.load());
        o->setProperty ("invert", t.live->invert.load());
        o->setProperty ("outBus", t.live->outBus.load());
        o->setProperty ("trim", (double) t.live->trimDb.load());
        o->setProperty ("trackType", t.live->trackType.load());
        {
            auto* c = new juce::DynamicObject();
            const auto& k = t.live->comp;
            c->setProperty ("on", k.enabled.load()); c->setProperty ("thr", (double) k.thresholdDb.load()); c->setProperty ("ratio", (double) k.ratio.load());
            c->setProperty ("atk", (double) k.attackMs.load()); c->setProperty ("rel", (double) k.releaseMs.load());
            c->setProperty ("knee", (double) k.kneeDb.load()); c->setProperty ("makeup", (double) k.makeupDb.load());
            o->setProperty ("comp", juce::var (c));
        }
        o->setProperty ("limiter", t.live->limiterOn.load());
        o->setProperty ("ceiling", (double) t.live->ceilingDb.load());
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("on", t.live->eq.enabled.load());
            juce::Array<juce::var> bands;
            for (auto& b : t.live->eq.band)
            {
                auto* bo = new juce::DynamicObject();
                bo->setProperty ("type", b.type.load());
                bo->setProperty ("freq", (double) b.freq.load());
                bo->setProperty ("gain", (double) b.gainDb.load());
                bo->setProperty ("q", (double) b.q.load());
                bo->setProperty ("on", b.on.load());
                bands.add (juce::var (bo));
            }
            e->setProperty ("bands", bands);
            o->setProperty ("eq", juce::var (e));
        }
        {
            juce::Array<juce::var> pts;
            for (auto& ap : t.volAuto) { juce::Array<juce::var> pr; pr.add ((juce::int64) ap.pos); pr.add ((double) ap.db); pts.add (juce::var (pr)); }
            o->setProperty ("volAuto", pts);
            static const char* keys[] = { "volAuto", "panAuto", "sendAAuto", "sendBAuto" };
            for (int pi = 1; pi < kNumAutoParams; ++pi)
            {
                juce::Array<juce::var> lp;
                for (auto& ap : t.lane (pi)) { juce::Array<juce::var> pr; pr.add ((juce::int64) ap.pos); pr.add ((double) ap.db); lp.add (juce::var (pr)); }
                o->setProperty (keys[pi], lp);
            }
            o->setProperty ("autoParam", t.live->autoParam.load());
        }
        o->setProperty ("sendA", (double) t.live->sendDb[0].load());
        o->setProperty ("sendB", (double) t.live->sendDb[1].load());

        o->setProperty ("clips", clipsToVar (t.clips));
        if (! t.versions.empty())
        {
            juce::Array<juce::var> vs;
            for (auto& v : t.versions)
            {
                auto* vo = new juce::DynamicObject();
                vo->setProperty ("id", v.id);
                vo->setProperty ("name", v.name);
                if (v.id != t.activeVersion) vo->setProperty ("clips", clipsToVar (v.clips));   // i clip della versione attiva sono quelli della traccia
                vs.add (juce::var (vo));
            }
            o->setProperty ("versions", vs);
            o->setProperty ("activeVersion", t.activeVersion);
        }
        tracks.add (juce::var (o));
    }
    root->setProperty ("tracks", tracks);
    juce::Array<juce::var> groups;
    for (auto& g : session.groups)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", g.id);
        o->setProperty ("name", g.name);
        o->setProperty ("collapsed", g.collapsed);
        groups.add (juce::var (o));
    }
    root->setProperty ("trackGroups", groups);
    return juce::var (root);
}

bool Project::save (const juce::File& f, juce::String& error)
{
    const auto json = juce::JSON::toString (toVar(), false);
    if (! f.replaceWithText (json))
    {
        error = TRANS ("Impossibile scrivere ") + f.getFullPathName();
        return false;
    }
    sessionFile = f;
    dirty = false;
    sendChangeMessage();
    return true;
}

bool Project::saveCopy (const juce::File& f, juce::String& error) const
{
    f.getParentDirectory().createDirectory();
    if (! f.replaceWithText (juce::JSON::toString (toVar(), false)))
    {
        error = TRANS ("Impossibile scrivere ") + f.getFullPathName();
        return false;
    }
    return true;
}

ConformStats Project::conform (const ConformPlan& plan, bool apply)
{
    ConformStats st;
    const auto& segs = plan.segments;
    Session work = session;
    int64_t nextId = nextClipId;
    constexpr int64_t kFade = 240;   // 5 ms a 48 kHz: breve dissolvenza sui nuovi tagli

    auto inCoverage = [&] (int64_t p) { for (auto& c : plan.coverage) if (p >= c.first && p < c.second) return true; return false; };
    auto segmentAt = [&] (int64_t p) -> const ConformSegment* { for (auto& s : segs) if (p >= s.oldStart && p < s.oldEnd) return &s; return nullptr; };

    for (auto& t : work.tracks)
    {
        std::vector<Clip> out;
        for (auto& c : t.clips)
        {
            // punti di taglio: confini dei segmenti e delle zone coperte dentro il clip
            std::vector<int64_t> cuts { c.start, c.end() };
            for (auto& s : segs) { if (s.oldStart > c.start && s.oldStart < c.end()) cuts.push_back (s.oldStart); if (s.oldEnd > c.start && s.oldEnd < c.end()) cuts.push_back (s.oldEnd); }
            for (auto& r : plan.coverage) { if (r.first > c.start && r.first < c.end()) cuts.push_back (r.first); if (r.second > c.start && r.second < c.end()) cuts.push_back (r.second); }
            std::sort (cuts.begin(), cuts.end());
            cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());

            std::vector<Clip> pieces;
            bool changed = false;
            for (size_t i = 0; i + 1 < cuts.size(); ++i)
            {
                const int64_t a = cuts[i], b = cuts[i + 1];
                Clip piece = c;
                piece.offset = c.offset + (a - c.start);
                piece.length = b - a;
                piece.fadeIn = a == c.start ? c.fadeIn : 0;
                piece.fadeOut = b == c.end() ? c.fadeOut : 0;
                if (const auto* s = segmentAt (a))
                {
                    piece.start = s->newStart + (a - s->oldStart);
                    changed = changed || piece.start != a;
                }
                else if (inCoverage (a)) { st.droppedSeconds += (double) (b - a) / sampleRate; changed = true; continue; }   // materiale tolto dal nuovo montaggio
                else piece.start = a;                                                                                            // fuori dal montaggio: non cambia
                if (a != c.start) piece.fadeIn = juce::jmax (piece.fadeIn, juce::jmin<int64_t> (kFade, piece.length / 2));
                if (b != c.end()) piece.fadeOut = juce::jmax (piece.fadeOut, juce::jmin<int64_t> (kFade, piece.length / 2));
                pieces.push_back (piece);
            }
            if (! changed) { out.push_back (c); continue; }
            ++st.clipsTouched;
            if (pieces.empty()) ++st.clipsDropped;
            bool first = true;
            for (auto& pc : pieces)
            {
                if (first) { pc.id = c.id; first = false; } else pc.id = nextId++;
                ++st.clipPieces;
                out.push_back (pc);
            }
        }
        t.clips = std::move (out);
    }

    auto mapPos = [&] (int64_t pos, bool& removed) -> int64_t
    {
        removed = false;
        if (const auto* s = segmentAt (pos)) return s->newStart + (pos - s->oldStart);
        if (inCoverage (pos)) removed = true;
        return pos;
    };
    for (auto& cue : work.cues)
    {
        bool removed = false;
        const int64_t ni = mapPos (cue.inPos, removed);
        if (removed)
        {
            ++st.cuesOut;
            cue.status = 3;
            if (! cue.notes.contains ("Fuori dal nuovo montaggio")) cue.notes = (cue.notes.isEmpty() ? juce::String() : cue.notes + " ") + "[Fuori dal nuovo montaggio]";
            continue;
        }
        const int64_t delta = ni - cue.inPos;
        bool r2 = false;
        const int64_t no = mapPos (cue.outPos - 1, r2) + 1;
        cue.outPos = r2 || no <= ni ? cue.outPos + delta : no;
        cue.inPos = ni;
        if (delta != 0) ++st.cuesMoved;
    }
    for (auto& m : work.markers)
    {
        bool removed = false;
        const int64_t np = mapPos (m.pos, removed);
        if (np != m.pos) { m.pos = np; ++st.markersMoved; }
    }
    if (plan.markInserted)
    {
        int n = 0;
        for (auto& r : plan.inserted)
        {
            Marker m;
            m.id = nextMarkerId + n;
            m.pos = r.first;
            m.name = "Inserito " + juce::String (++n) + " (" + juce::String ((double) (r.second - r.first) / sampleRate, 1) + " s)";
            m.colour = juce::Colour (0xff4cd27a);
            work.markers.push_back (m);
            ++st.insertedMarkers;
        }
        std::sort (work.markers.begin(), work.markers.end(), [] (const Marker& a, const Marker& b) { return a.pos < b.pos; });
    }

    if (apply)
    {
        beginEdit();
        session = std::move (work);
        nextClipId = nextId;
        nextMarkerId += st.insertedMarkers;
        selection.clear();
        changed();
    }
    return st;
}

bool Project::hasOfflineMedia() const noexcept
{
    if (missingVideoPath.isNotEmpty() && session.video == nullptr) return true;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
            if (c.asset != nullptr && c.asset->offline) return true;
    return false;
}

void Project::replaceAssets (const std::map<const Asset*, AssetPtr>& replacements)
{
    if (replacements.empty()) return;
    beginEdit();
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
        {
            if (c.asset == nullptr) continue;
            auto it = replacements.find (c.asset.get());
            if (it == replacements.end() || it->second == nullptr) continue;
            c.asset = it->second;
            c.offset = juce::jlimit<int64_t> (0, c.asset->length(), c.offset);
            c.length = juce::jmin (c.length, c.asset->length() - c.offset);
        }
    changed();
}

void Project::relinkVideo (const juce::File& f, std::function<void (juce::String)> done)
{
    const double offset = missingVideoOffset;
    runAsync ([=, this]() -> std::function<void()>
    {
        setTask (TRANS ("Ricollego il video"), 0.01f);
        auto info = video::probe (f);
        if (! info.ok) { setTask ({}, -1.0f); return [=] { if (done) done (info.error); }; }
        auto va = std::make_shared<VideoAsset>();
        va->file = f;
        va->duration = info.duration;
        va->fps = info.fps;
        va->width = info.width;
        va->height = info.height;
        va->thumbInterval = juce::jmax (1.0, std::ceil (info.duration / 240.0));
        va->thumbs = video::thumbnails (f, va->thumbInterval, (int) std::ceil (info.duration / va->thumbInterval), 56,
                                        [this] (float p) { setTask (TRANS ("Ricollego il video: miniature"), 0.02f + 0.95f * p); });
        va->offsetSec = offset;
        setTask ({}, -1.0f);
        return [=, this]
        {
            beginEdit();
            session.video = va;
            missingVideoPath.clear();
            changed();
            if (done) done ({});
        };
    });
}

void Project::newSession (bool withDefaultTrack)
{
    session = Session();
    missingVideoPath.clear();
    missingVideoOffset = 0.0;
    nextMarkerId = 1;
    nextCueId = 1;
    undoStack.clear();
    redoStack.clear();
    selection.clear();
    nextTrackId = 1;
    range = {};
    if (withDefaultTrack) addTrackInternal();   // una nuova sessione parte con una sola traccia
    selectedTrackId = session.tracks.empty() ? -1 : session.tracks.front().id;
    adrNaming = "{personaggio}_{scena}_{cue}_T{take}";
    notes.clear();
    sessionFile = juce::File();
    publish();
    dirty = false;
    sendChangeMessage();
}

void Project::loadSession (const juce::File& f, std::function<void (juce::String)> done)
{
    const double rate = sampleRate;
    auto* fm = &formats;

    runAsync ([=, this]() -> std::function<void()>
    {
        juce::String errors, lostVideoPath;
        int missingCount = 0;
        double lostVideoOffset = 0.0;
        auto parsed = juce::JSON::parse (f);
        auto result = std::make_shared<Session>();
        const int loudStd = (int) parsed.getProperty ("loudnessStandard", 1);
        const juce::String pNotes = parsed.getProperty ("notes", "").toString();
        const int ptcFormat = (int) parsed.getProperty ("tcFormat", 2);
        const double ptcStart = (double) parsed.getProperty ("tcStart", 0.0);
        const bool prulerTc = (bool) parsed.getProperty ("rulerTimecode", true);
        const double pPre = (double) parsed.getProperty ("adrPreRoll", 3.0), pPost = (double) parsed.getProperty ("adrPostRoll", 1.0);
        const int pBeeps = (int) parsed.getProperty ("adrBeeps", 3);
        const bool pStreamer = (bool) parsed.getProperty ("adrStreamer", true);
        const int pOverlay = (int) parsed.getProperty ("adrOverlay", 31);
        const int pDialogueSize = juce::jlimit (12, 120, (int) parsed.getProperty ("adrDialogueSize", 30));
        const juce::String pNaming = parsed.getProperty ("adrNaming", "{personaggio}_{scena}_{cue}_T{take}").toString();
        const int64_t pExportIn = (juce::int64) parsed.getProperty ("exportIn", -1), pExportOut = (juce::int64) parsed.getProperty ("exportOut", -1);
        bool ok = parsed.isObject();
        if (! ok) errors = TRANS ("File di sessione non valido.");

        if (ok)
        {
            const double savedRate = (double) parsed.getProperty ("sampleRate", rate);
            const double scale = rate / (savedRate > 0 ? savedRate : rate);
            auto sc = [scale] (const juce::var& v) { return (int64_t) std::llround ((double) (juce::int64) v * scale); };
            std::map<juce::String, AssetPtr> cache;
            // i percorsi relativi (pacchetti e archivi) partono dalla cartella del file di sessione
            auto resolvePath = [&f] (const juce::String& p) { return juce::File::isAbsolutePath (p) ? juce::File (p) : f.getParentDirectory().getChildFile (p); };

            result->master->volumeDb = (float) (double) parsed.getProperty ("masterDb", 0.0);
            result->master->limiterOn = (bool) parsed.getProperty ("limiterOn", true);
            result->master->ceilingDb = (float) (double) parsed.getProperty ("ceilingDb", -1.0);
            insertsFromVar (parsed.getProperty ("masterInserts", {}), *result->master);
            if (auto* aa = parsed.getProperty ("aux", {}).getArray())
                for (int k = 0; k < 2 && k < aa->size(); ++k)
                {
                    const auto& av = (*aa)[k];
                    result->aux[k]->volumeDb = (float) (double) av.getProperty ("volumeDb", 0.0);
                    result->aux[k]->mute = (bool) av.getProperty ("mute", false);
                    insertsFromVar (av.getProperty ("inserts", {}), *result->aux[k]);
                }
            if (auto* ga = parsed.getProperty ("groups", {}).getArray())
                for (int k = 0; k < Session::kNumGroups && k < ga->size(); ++k)
                {
                    const auto& gv = (*ga)[k];
                    result->group[k]->volumeDb = (float) (double) gv.getProperty ("volumeDb", 0.0);
                    result->group[k]->mute = (bool) gv.getProperty ("mute", false);
                    insertsFromVar (gv.getProperty ("inserts", {}), *result->group[k]);
                }
            if (auto* ca = parsed.getProperty ("adrCues", {}).getArray())
                for (auto& cv : *ca)
                {
                    AdrCue c;
                    c.id = (int) cv.getProperty ("id", 0);
                    c.number = (int) cv.getProperty ("number", 0);
                    c.character = cv.getProperty ("character", "").toString();
                    c.scene = cv.getProperty ("scene", "").toString();
                    c.reel = cv.getProperty ("reel", "").toString();
                    c.dialogue = cv.getProperty ("dialogue", "").toString();
                    c.notes = cv.getProperty ("notes", "").toString();
                    c.inPos = sc (cv.getProperty ("in", 0));
                    c.outPos = sc (cv.getProperty ("out", 0));
                    c.status = (int) cv.getProperty ("status", 0);
                    c.activeTake = (int) cv.getProperty ("activeTake", 0);
                    result->cues.push_back (c);
                }
            if (auto* ma = parsed.getProperty ("markers", {}).getArray())
                for (auto& mv : *ma)
                {
                    Marker mk;
                    mk.pos = sc (mv.getProperty ("pos", 0));
                    if (mv.hasProperty ("end")) mk.end = sc (mv.getProperty ("end", -1));
                    mk.name = mv.getProperty ("name", "Marker").toString();
                    mk.colour = juce::Colour::fromString (mv.getProperty ("colour", "ffffc94d").toString());
                    result->markers.push_back (mk);
                }
            if (auto* ga = parsed.getProperty ("trackGroups", {}).getArray())
                for (auto& gv : *ga)
                {
                    TrackGroup g;
                    g.id = (int) gv.getProperty ("id", 0);
                    g.name = gv.getProperty ("name", "Gruppo").toString();
                    g.collapsed = (bool) gv.getProperty ("collapsed", false);
                    if (g.id > 0) result->groups.push_back (g);
                }
            auto loadClips = [&] (const juce::var& list, std::vector<Clip>& dst)
            {
                if (auto* ca = list.getArray())
                        for (auto& cv : *ca)
                        {
                            const auto path = cv.getProperty ("file", "").toString();
                            auto it = cache.find (path);
                            if (it == cache.end())
                            {
                                juce::String err;
                                auto a = loadAsset (*fm, resolvePath (path), rate, err);
                                if (a == nullptr)
                                {
                                    // file mancante: il clip resta, senza audio, e si potra' ricollegare
                                    a = std::make_shared<Asset>();
                                    a->file = resolvePath (path);
                                    a->name = a->file.getFileNameWithoutExtension();
                                    a->offline = true;
                                    ++missingCount;
                                    errors += (errors.isEmpty() ? "" : "\n") + juce::String (TRANS ("File non trovato: ")) + path;
                                }
                                it = cache.emplace (path, a).first;
                            }
                            if (it->second == nullptr) continue;

                            Clip c;
                            c.asset = it->second;
                            c.start = sc (cv.getProperty ("start", 0));
                            c.offset = sc (cv.getProperty ("offset", 0));
                            c.length = sc (cv.getProperty ("length", 0));
                            c.gainDb = (float) (double) cv.getProperty ("gainDb", 0.0);
                            c.fadeIn = sc (cv.getProperty ("fadeIn", 0));
                            c.fadeOut = sc (cv.getProperty ("fadeOut", 0));
                            c.cueId = (int) cv.getProperty ("cueId", 0);
                            c.takeNo = (int) cv.getProperty ("takeNo", 0);
                            c.muted = (bool) cv.getProperty ("muted", false);
                            c.rating = juce::jlimit (0, 5, (int) cv.getProperty ("rating", 0));
                            c.rejected = (bool) cv.getProperty ("rejected", false);
                            c.invert = (bool) cv.getProperty ("invert", false);
                            if (cv.hasProperty ("sync")) c.syncPoint = sc (cv.getProperty ("sync", 0));
                            if (auto* pts = cv.getProperty ("env", {}).getArray())
                                for (auto& pv : *pts)
                                    if (auto* one = pv.getArray(); one != nullptr && one->size() == 2)
                                        c.env.push_back ({ sc ((*one)[0]), (float) (double) (*one)[1] });
                            c.xShape = juce::jlimit (0, 2, (int) cv.getProperty ("xShape", 0));
                            if (c.asset->offline) c.asset->offlineLength = juce::jmax (c.asset->offlineLength, c.offset + c.length);   // la lunghezza minima che i clip richiedono
                            c.offset = juce::jlimit<int64_t> (0, c.asset->length(), c.offset);
                            c.length = juce::jlimit<int64_t> (0, c.asset->length() - c.offset, c.length);
                            if (c.length > 0) dst.push_back (c);
                        }
            };
            if (auto* ta = parsed.getProperty ("tracks", {}).getArray())
                for (auto& tv : *ta)
                {
                    Track t;
                    t.name = tv.getProperty ("name", "Audio").toString();
                    t.groupId = (int) tv.getProperty ("group", 0);
                    t.locked = (bool) tv.getProperty ("locked", false);
                    if (const auto ff = tv.getProperty ("frozenFile", {}).toString(); ff.isNotEmpty())
                    {
                        juce::String fe;
                        t.frozen = loadAsset (*fm, resolvePath (ff), rate, fe);
                        if (t.frozen == nullptr) errors += (errors.isEmpty() ? "" : "\n") + juce::String (TRANS ("File del Freeze non trovato (la traccia torna in tempo reale): ")) + ff;
                    }
                    t.hidden = (bool) tv.getProperty ("hidden", false);
                    t.showLanes = (bool) tv.getProperty ("lanes", false);
                    t.colour = juce::Colour::fromString (tv.getProperty ("colour", "ffe5584c").toString());
                    t.live->volumeDb = (float) (double) tv.getProperty ("volumeDb", 0.0);
                    t.live->pan = (float) (double) tv.getProperty ("pan", 0.0);
                    t.live->mute = (bool) tv.getProperty ("mute", false);
                    t.live->solo = (bool) tv.getProperty ("solo", false);
                    t.live->inputMode = (int) tv.getProperty ("inputMode", 0);
                    t.live->height = juce::jlimit (48, 400, (int) tv.getProperty ("height", 96));
                    insertsFromVar (tv.getProperty ("inserts", {}), *t.live);
                    t.live->autoMode = (int) tv.getProperty ("autoMode", 0);
                    t.live->invert = (bool) tv.getProperty ("invert", false);
                    t.live->trimDb = juce::jlimit (-24.0f, 24.0f, (float) (double) tv.getProperty ("trim", 0.0));
                    t.live->trackType = juce::jlimit (0, (int) kNumTrackTypes - 1, (int) tv.getProperty ("trackType", 0));
                    if (tv.hasProperty ("comp"))
                    {
                        const auto cv = tv.getProperty ("comp", {});
                        auto& k = t.live->comp;
                        k.enabled = (bool) cv.getProperty ("on", false);
                        k.thresholdDb = juce::jlimit (-60.0f, 0.0f, (float) (double) cv.getProperty ("thr", -18.0));
                        k.ratio = juce::jlimit (1.0f, 20.0f, (float) (double) cv.getProperty ("ratio", 3.0));
                        k.attackMs = juce::jlimit (0.1f, 200.0f, (float) (double) cv.getProperty ("atk", 10.0));
                        k.releaseMs = juce::jlimit (5.0f, 2000.0f, (float) (double) cv.getProperty ("rel", 120.0));
                        k.kneeDb = juce::jlimit (0.0f, 24.0f, (float) (double) cv.getProperty ("knee", 6.0));
                        k.makeupDb = juce::jlimit (-12.0f, 24.0f, (float) (double) cv.getProperty ("makeup", 0.0));
                    }
                    t.live->limiterOn = (bool) tv.getProperty ("limiter", false);   // le sessioni vecchie non lo avevano
                    t.live->ceilingDb = juce::jlimit (-24.0f, 0.0f, (float) (double) tv.getProperty ("ceiling", -1.0));
                    t.live->outBus = juce::jlimit (0, Session::kNumGroups, (int) tv.getProperty ("outBus", 0));
                    if (tv.hasProperty ("eq"))
                    {
                        const auto ev = tv.getProperty ("eq", {});
                        t.live->eq.enabled = (bool) ev.getProperty ("on", false);
                        if (auto* ba = ev.getProperty ("bands", {}).getArray())
                            for (int bi = 0; bi < ParamEq::kBands && bi < ba->size(); ++bi)
                            {
                                const auto& bv = (*ba)[bi];
                                auto& b = t.live->eq.band[bi];
                                b.type = juce::jlimit (0, (int) ParamEq::kNumTypes - 1, (int) bv.getProperty ("type", 0));
                                b.freq = juce::jlimit (20.0f, 20000.0f, (float) (double) bv.getProperty ("freq", 1000.0));
                                b.gainDb = juce::jlimit (-24.0f, 24.0f, (float) (double) bv.getProperty ("gain", 0.0));
                                b.q = juce::jlimit (0.1f, 20.0f, (float) (double) bv.getProperty ("q", 1.0));
                                b.on = (bool) bv.getProperty ("on", true);
                            }
                    }
                    {
                        static const char* keys[] = { "volAuto", "panAuto", "sendAAuto", "sendBAuto" };
                        for (int pi = 0; pi < kNumAutoParams; ++pi)
                            if (auto* pa = tv.getProperty (keys[pi], {}).getArray())
                                for (auto& pv : *pa)
                                    if (auto* pr = pv.getArray(); pr != nullptr && pr->size() == 2)
                                    {
                                        AutoPoint ap;
                                        ap.pos = sc ((*pr)[0]);
                                        ap.db = clampAutoValue (pi, (float) (double) (*pr)[1]);
                                        t.lane (pi).push_back (ap);
                                    }
                        t.live->autoParam = juce::jlimit (0, kNumAutoParams - 1, (int) tv.getProperty ("autoParam", 0));
                    }
                    t.live->sendDb[0] = (float) (double) tv.getProperty ("sendA", -80.0);
                    t.live->sendDb[1] = (float) (double) tv.getProperty ("sendB", -80.0);

                    loadClips (tv.getProperty ("clips", {}), t.clips);
                    if (auto* va = tv.getProperty ("versions", {}).getArray())
                    {
                        for (auto& vv : *va)
                        {
                            Track::Version v;
                            v.id = (int) vv.getProperty ("id", 0);
                            v.name = vv.getProperty ("name", "").toString();
                            if (v.id > 0) { loadClips (vv.getProperty ("clips", {}), v.clips); t.versions.push_back (std::move (v)); }
                        }
                        t.activeVersion = (int) tv.getProperty ("activeVersion", t.versions.empty() ? 0 : t.versions.front().id);
                    }
                    result->tracks.push_back (std::move (t));
                }
            if (result->tracks.empty() && ! parsed.hasProperty ("tracks")) { ok = false; errors = TRANS ("La sessione non contiene tracce."); }   // una sessione senza tracce e' valida
            for (auto& t : result->tracks)   // un gruppo che non esiste piu' non lascia tracce nascoste
            {
                bool known = false;
                for (auto& g : result->groups) known = known || g.id == t.groupId;
                if (! known) t.groupId = 0;
            }

            const auto vpath = parsed.getProperty ("video", "").toString();
            if (ok && vpath.isNotEmpty())
            {
                juce::File vf (juce::File::isAbsolutePath (vpath) ? juce::File (vpath) : f.getParentDirectory().getChildFile (vpath));
                auto info = video::probe (vf);
                if (info.ok)
                {
                    auto va = std::make_shared<VideoAsset>();
                    va->file = vf;
                    va->duration = info.duration;
                    va->fps = info.fps;
                    va->width = info.width;
                    va->height = info.height;
                    va->thumbInterval = juce::jmax (1.0, std::ceil (info.duration / 240.0));
                    setTask (TRANS ("Carico il video: miniature"), 0.02f);
                    va->thumbs = video::thumbnails (vf, va->thumbInterval, (int) std::ceil (info.duration / va->thumbInterval), 56,
                                                    [this] (float p) { setTask (TRANS ("Carico il video: miniature"), 0.02f + 0.9f * p); });
                    setTask ({}, -1.0f);
                    va->offsetSec = (double) parsed.getProperty ("videoOffset", 0.0);
                    result->video = va;
                }
                else
                {
                    errors += (errors.isEmpty() ? "" : "\n") + juce::String (TRANS ("Video non trovato: ")) + vf.getFullPathName();
                    lostVideoPath = vpath;
                    lostVideoOffset = (double) parsed.getProperty ("videoOffset", 0.0);
                }
            }
        }

        return [=, this]
        {
            if (ok && std::abs (rate - sampleRate) < 0.5)
            {
                session = *result;
                nextTrackId = 1;
                nextClipId = 1;
                nextAutoId = 1;
                for (auto& t : session.tracks)
                {
                    t.id = nextTrackId++;
                    for (auto& c : t.clips) c.id = nextClipId++;
                    for (int pr = 0; pr < kNumAutoParams; ++pr) for (auto& ap : t.lane (pr)) ap.id = nextAutoId++;
                }
                nextMarkerId = 1;
                for (auto& m : session.markers) m.id = nextMarkerId++;
                nextCueId = 1;
                for (auto& c : session.cues) nextCueId = juce::jmax (nextCueId, c.id + 1);
                undoStack.clear();
                redoStack.clear();
                selection.clear();
                selectedTrackId = session.tracks.empty() ? -1 : session.tracks.front().id;
                sessionFile = f;
                loudnessStandard = loudStd;
                notes = pNotes;
                tcFormat = juce::jlimit (0, numTcFormats - 1, ptcFormat);
                tcStartSec = ptcStart;
                rulerTimecode = prulerTc;
                adrPreRoll = pPre; adrPostRoll = pPost; adrBeeps = pBeeps; adrStreamer = pStreamer; adrOverlay = pOverlay; adrDialogueSize = pDialogueSize; adrNaming = pNaming; exportIn = pExportIn; exportOut = pExportOut;
                missingVideoPath = lostVideoPath;
                missingVideoOffset = lostVideoOffset;
                publish();
                dirty = false;
                quietUntil = juce::Time::getMillisecondCounterHiRes() + 6000.0;
                if (onSessionLoaded) onSessionLoaded();
                dirty = false;
            }
            if (done) done (errors);
        };
    });
}

} // namespace fc
