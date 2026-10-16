#include "Model.h"
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
        error = "Formato non supportato o file vuoto: " + f.getFileName();
        return nullptr;
    }
    if (r->lengthInSamples > 2000000000LL)
    {
        error = "File troppo lungo: " + f.getFileName();
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

void Project::undo()
{
    if (undoStack.empty()) return;
    redoStack.push_back (session);
    session = std::move (undoStack.back());
    undoStack.pop_back();
    changed();
}

void Project::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back (session);
    session = std::move (redoStack.back());
    redoStack.pop_back();
    changed();
}

void Project::publish()
{
    if (onPublish)
    {
        session.sampleRate = sampleRate;
        auto copy = std::make_shared<Session> (session);
        copy->computeCrossfades();
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
    dirty = true;
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
    if (i < 0 || session.tracks.size() <= 1) return;
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
                errs.add ("La frequenza di campionamento e' cambiata durante l'importazione.");
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

void Project::setVideo (const juce::File& f, std::function<void (juce::String)> done)
{
    const double rate = sampleRate;
    auto* fm = &formats;

    runAsync ([=, this]() -> std::function<void()>
    {
        setTask ("Carico il video: lettura delle informazioni", 0.01f);
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
        setTask ("Carico il video: miniature", 0.03f);
        va->thumbs = video::thumbnails (f, va->thumbInterval, (int) std::ceil (info.duration / va->thumbInterval), 56,
                                        [this] (float p) { setTask ("Carico il video: miniature", 0.03f + 0.37f * p); });

        AssetPtr audio;
        juce::String audioNote;
        if (info.hasAudio)
        {
            juce::String err;
            setTask ("Carico il video: audio", 0.4f);
            audio = loadAsset (*fm, f, rate, err, [this] (float p) { setTask ("Carico il video: audio", 0.4f + 0.6f * p); });
            if (audio == nullptr) audioNote = "Il video e' stato caricato, ma non riesco a estrarne l'audio: " + err;
        }

        setTask ({}, -1.0f);
        return [=, this]
        {
            if (std::abs (rate - sampleRate) > 0.5) { if (done) done ("La frequenza di campionamento e' cambiata."); return; }
            beginEdit();
            session.video = va;
            if (tcFormat == 2 && std::abs (va->fps - 25.0) > 0.01) tcFormat = tcFormatForFps (va->fps);
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
                t->name = "Audio video";
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

void Project::deleteTake (int cueId, int takeNo)
{
    auto* cue = findCue (cueId);
    if (cue == nullptr) return;
    beginEdit();
    for (auto& t : session.tracks)
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [cueId, takeNo] (const Clip& c) { return c.cueId == cueId && c.takeNo == takeNo; }), t.clips.end());
    if (cue->activeTake == takeNo)
    {
        int best = 0;
        for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) best = juce::jmax (best, c.takeNo);
        cue->activeTake = best;
        for (auto& t : session.tracks) for (auto& c : t.clips) if (c.cueId == cueId) c.muted = (c.takeNo != best);
        if (best == 0 && cue->status == 1) cue->status = 0;
    }
    changed();
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
            if (c.cueId == cueId) c.muted = (c.takeNo != takeNo);
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

void Project::moveClipTo (int64_t clipId, int trackIndex, int64_t start)
{
    int ti = -1;
    auto* c = findClip (clipId, &ti);
    if (c == nullptr) return;
    trackIndex = juce::jlimit (0, (int) session.tracks.size() - 1, trackIndex);
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
        for (size_t ci = 0; ci < session.tracks[ti].clips.size(); ++ci)
        {
            auto& c = session.tracks[ti].clips[ci];
            if (contains (c) && (! onlySelected || selection.count (c.id) > 0)) targets.emplace_back (ti, ci);
        }
    if (targets.empty()) return;

    beginEdit();
    for (auto [ti, ci] : targets)
    {
        auto& clips = session.tracks[ti].clips;
        Clip right = clips[ci];
        const bool wasSelected = selection.count (right.id) > 0;
        const int64_t cut = pos - clips[ci].start;
        right.id = nextClipId++;
        right.start = pos;
        right.offset += cut;
        right.length -= cut;
        right.fadeIn = 0;
        right.fadeOut = juce::jmin (right.fadeOut, right.length);
        clips[ci].length = cut;
        clips[ci].fadeOut = 0;
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
    if (c == nullptr || pos <= c->start + 16 || pos >= c->end() - 16) return;

    beginEdit();
    auto& clips = session.tracks[(size_t) ti].clips;
    const size_t idx = (size_t) (c - clips.data());
    Clip right = clips[idx];
    const bool wasSelected = selection.count (right.id) > 0;
    const int64_t cut = pos - clips[idx].start;
    right.id = nextClipId++;
    right.start = pos;
    right.offset += cut;
    right.length -= cut;
    right.fadeIn = 0;
    right.fadeOut = juce::jmin (right.fadeOut, right.length);
    clips[idx].length = cut;
    clips[idx].fadeOut = 0;
    clips[idx].fadeIn = juce::jmin (clips[idx].fadeIn, cut);
    clips.push_back (right);
    selection = { right.id };
    (void) wasSelected;
    changed();
}

void Project::rippleDeleteSelected()
{
    if (selection.empty()) return;
    beginEdit();
    for (auto& t : session.tracks)
    {
        std::vector<std::pair<int64_t, int64_t>> removed;
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
            if (inside (c) && (! onlySelected || selection.count (c.id) > 0))
            {
                if (! any) { beginEdit(); any = true; }
                if (trimStart)
                {
                    const int64_t d = pos - c.start;
                    c.start = pos;
                    c.offset += d;
                    c.length -= d;
                    c.fadeIn = 0;
                    c.fadeOut = juce::jmin (c.fadeOut, c.length);
                }
                else
                {
                    c.length = pos - c.start;
                    c.fadeOut = 0;
                    c.fadeIn = juce::jmin (c.fadeIn, c.length);
                }
            }
    if (any) changed();
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
    beginEdit();
    for (auto& t : session.tracks)
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
            if (selection.count (c.id))
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
    auto* c = findClip (id);
    if (c == nullptr || c->asset == nullptr) return;
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
    auto* c = findClip (id);
    if (c == nullptr || c->asset == nullptr || c->length <= 0) { if (done) done ("Clip non valido."); return; }
    if (c->asset->offline) { if (done) done ("Il file di questo clip non e' stato trovato: ricollegalo con File > Ricollega i media."); return; }
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

void Project::setClipGain (int64_t id, float db)
{
    if (auto* c = findClip (id))
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
            o->setProperty ("name", m.name);
            o->setProperty ("colour", m.colour.toString());
            marks.add (juce::var (o));
        }
        root->setProperty ("markers", marks);
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

    juce::Array<juce::var> tracks;
    for (auto& t : session.tracks)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", t.name);
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

        juce::Array<juce::var> clips;
        for (auto& c : t.clips)
        {
            auto* co = new juce::DynamicObject();
            co->setProperty ("file", c.asset->file.getFullPathName());
            co->setProperty ("start", (juce::int64) c.start);
            co->setProperty ("offset", (juce::int64) c.offset);
            co->setProperty ("length", (juce::int64) c.length);
            co->setProperty ("gainDb", (double) c.gainDb);
            co->setProperty ("fadeIn", (juce::int64) c.fadeIn);
            co->setProperty ("fadeOut", (juce::int64) c.fadeOut);
            if (c.cueId != 0) { co->setProperty ("cueId", c.cueId); co->setProperty ("takeNo", c.takeNo); co->setProperty ("muted", c.muted); }
            clips.add (juce::var (co));
        }
        o->setProperty ("clips", clips);
        tracks.add (juce::var (o));
    }
    root->setProperty ("tracks", tracks);
    return juce::var (root);
}

bool Project::save (const juce::File& f, juce::String& error)
{
    const auto json = juce::JSON::toString (toVar(), false);
    if (! f.replaceWithText (json))
    {
        error = "Impossibile scrivere " + f.getFullPathName();
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
        error = "Impossibile scrivere " + f.getFullPathName();
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
        setTask ("Ricollego il video", 0.01f);
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
                                        [this] (float p) { setTask ("Ricollego il video: miniature", 0.02f + 0.95f * p); });
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
    if (withDefaultTrack) addTrackInternal();   // una nuova sessione parte con una sola traccia
    selectedTrackId = session.tracks.empty() ? -1 : session.tracks.front().id;
    adrNaming = "{personaggio}_{scena}_{cue}_T{take}";
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
        const int ptcFormat = (int) parsed.getProperty ("tcFormat", 2);
        const double ptcStart = (double) parsed.getProperty ("tcStart", 0.0);
        const bool prulerTc = (bool) parsed.getProperty ("rulerTimecode", true);
        const double pPre = (double) parsed.getProperty ("adrPreRoll", 3.0), pPost = (double) parsed.getProperty ("adrPostRoll", 1.0);
        const int pBeeps = (int) parsed.getProperty ("adrBeeps", 3);
        const bool pStreamer = (bool) parsed.getProperty ("adrStreamer", true);
        const int pDialogueSize = juce::jlimit (12, 120, (int) parsed.getProperty ("adrDialogueSize", 30));
        const juce::String pNaming = parsed.getProperty ("adrNaming", "{personaggio}_{scena}_{cue}_T{take}").toString();
        const int64_t pExportIn = (juce::int64) parsed.getProperty ("exportIn", -1), pExportOut = (juce::int64) parsed.getProperty ("exportOut", -1);
        bool ok = parsed.isObject();
        if (! ok) errors = "File di sessione non valido.";

        if (ok)
        {
            const double savedRate = (double) parsed.getProperty ("sampleRate", rate);
            const double scale = rate / (savedRate > 0 ? savedRate : rate);
            auto sc = [scale] (const juce::var& v) { return (int64_t) std::llround ((double) (juce::int64) v * scale); };
            std::map<juce::String, AssetPtr> cache;

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
                    mk.name = mv.getProperty ("name", "Marker").toString();
                    mk.colour = juce::Colour::fromString (mv.getProperty ("colour", "ffffc94d").toString());
                    result->markers.push_back (mk);
                }
            if (auto* ta = parsed.getProperty ("tracks", {}).getArray())
                for (auto& tv : *ta)
                {
                    Track t;
                    t.name = tv.getProperty ("name", "Audio").toString();
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

                    if (auto* ca = tv.getProperty ("clips", {}).getArray())
                        for (auto& cv : *ca)
                        {
                            const auto path = cv.getProperty ("file", "").toString();
                            auto it = cache.find (path);
                            if (it == cache.end())
                            {
                                juce::String err;
                                auto a = loadAsset (*fm, juce::File (path), rate, err);
                                if (a == nullptr)
                                {
                                    // file mancante: il clip resta, senza audio, e si potra' ricollegare
                                    a = std::make_shared<Asset>();
                                    a->file = juce::File (path);
                                    a->name = a->file.getFileNameWithoutExtension();
                                    a->offline = true;
                                    ++missingCount;
                                    errors += (errors.isEmpty() ? "" : "\n") + juce::String ("File non trovato: ") + path;
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
                            if (c.asset->offline) c.asset->offlineLength = juce::jmax (c.asset->offlineLength, c.offset + c.length);   // la lunghezza minima che i clip richiedono
                            c.offset = juce::jlimit<int64_t> (0, c.asset->length(), c.offset);
                            c.length = juce::jlimit<int64_t> (0, c.asset->length() - c.offset, c.length);
                            if (c.length > 0) t.clips.push_back (c);
                        }
                    result->tracks.push_back (std::move (t));
                }
            if (result->tracks.empty()) { ok = false; errors = "La sessione non contiene tracce."; }

            const auto vpath = parsed.getProperty ("video", "").toString();
            if (ok && vpath.isNotEmpty())
            {
                juce::File vf (vpath);
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
                    setTask ("Carico il video: miniature", 0.02f);
                    va->thumbs = video::thumbnails (vf, va->thumbInterval, (int) std::ceil (info.duration / va->thumbInterval), 56,
                                                    [this] (float p) { setTask ("Carico il video: miniature", 0.02f + 0.9f * p); });
                    setTask ({}, -1.0f);
                    va->offsetSec = (double) parsed.getProperty ("videoOffset", 0.0);
                    result->video = va;
                }
                else
                {
                    errors += (errors.isEmpty() ? "" : "\n") + juce::String ("Video non trovato: ") + vf.getFullPathName();
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
                selectedTrackId = session.tracks.front().id;
                sessionFile = f;
                loudnessStandard = loudStd;
                tcFormat = juce::jlimit (0, numTcFormats - 1, ptcFormat);
                tcStartSec = ptcStart;
                rulerTimecode = prulerTc;
                adrPreRoll = pPre; adrPostRoll = pPost; adrBeeps = pBeeps; adrStreamer = pStreamer; adrDialogueSize = pDialogueSize; adrNaming = pNaming; exportIn = pExportIn; exportOut = pExportOut;
                missingVideoPath = lostVideoPath;
                missingVideoOffset = lostVideoOffset;
                publish();
                dirty = false;
                if (onSessionLoaded) onSessionLoaded();
            }
            if (done) done (errors);
        };
    });
}

} // namespace fc
