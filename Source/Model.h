#pragma once

#include <JuceHeader.h>
#include "Lang.h"
#include "Timecode.h"
#include "ParamEq.h"
#include "TrackComp.h"
#include "Limiter.h"
#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace fc
{
/** File di sessione: .adb (le sessioni salvate con le versioni precedenti, .arostudio, si aprono ancora). */
inline bool isSessionFile (const juce::File& f) { return f.hasFileExtension ("adb;arostudio"); }

inline float dbToGain (float db) noexcept { return db <= -79.5f ? 0.0f : juce::Decibels::decibelsToGain (db); }

//==============================================================================
struct PeakLevel
{
    int block = 64;
    std::vector<float> mn, mx;     // canale sinistro (o l'unico canale)
    std::vector<float> mn1, mx1;   // canale destro, solo per l'audio stereo
};

/** Audio caricato in memoria alla frequenza di sessione, con picchi pre-calcolati per la forma d'onda. */
struct Asset
{
    juce::File file;
    juce::String name;
    juce::AudioBuffer<float> audio;
    std::vector<PeakLevel> levels;

    // file non trovato all'apertura della sessione: il clip resta (con il percorso) ma non suona; si ricollega con File > Ricollega i media
    bool offline = false;
    int64_t offlineLength = 0;      // lunghezza minima ricordata dai clip della sessione
    int offlineChannels = 2;

    int64_t length() const noexcept { return offline ? offlineLength : audio.getNumSamples(); }
    int numChannels() const noexcept { return offline ? offlineChannels : audio.getNumChannels(); }
    void buildPeaks();
};
using AssetPtr = std::shared_ptr<Asset>;

AssetPtr loadAsset (juce::AudioFormatManager& fm, const juce::File& f, double rate, juce::String& error, const std::function<void (float)>& progress = {});

//==============================================================================
struct Clip
{
    int64_t id = 0;
    AssetPtr asset;
    int64_t start = 0;   // posizione sulla timeline (campioni)
    int64_t offset = 0;  // inizio all'interno dell'asset
    int64_t length = 0;
    float gainDb = 0.0f;
    int64_t fadeIn = 0, fadeOut = 0;
    // take ADR: i clip dello stesso cue sono sovrapposti e ne suona uno solo
    int cueId = 0;
    int takeNo = 0;
    bool muted = false;
    // crossfade automatico con i clip che si sovrappongono (calcolato da Session::computeCrossfades, non salvato)
    int64_t xfadeIn = 0, xfadeOut = 0;
    bool xLinear = false;      // stesso materiale continuo: dissolvenza a guadagno costante invece che a potenza costante

    int64_t end() const noexcept { return start + length; }
};

/** Mantiene vivo un oggetto per qualche secondo prima di rilasciarlo sul thread messaggi: il thread audio
    potrebbe ancora usarlo, e i plug-in non vanno mai distrutti dal thread audio. (Solo thread messaggi.) */
void retire (std::shared_ptr<const void> object);

//==============================================================================
/** Un plug-in VST3 inserito su una traccia o sul master. */
struct InsertSlot
{
    juce::PluginDescription desc;
    juce::String name;
    juce::String initialState;               // stato salvato (base64), usato finche' il plug-in non e' pronto
    juce::String error;                      // non vuoto se il plug-in non si e' potuto caricare
    std::atomic<bool> bypass { false };
    std::atomic<bool> ready { false };       // il plug-in e' istanziato e preparato: si puo' chiamare processBlock
    std::unique_ptr<juce::AudioProcessor> plugin;   // un AudioPluginInstance (VST3) oppure, nei test, un processore qualsiasi
};

struct InsertChain
{
    std::vector<std::shared_ptr<InsertSlot>> slots;
};

/** Inserto da ripristinare dopo il caricamento di una sessione. */
struct PendingInsert { juce::String descXml, stateB64; bool bypass = false; };

/** Parametri letti dal thread audio e scritti dalla UI (condivisi fra le copie della sessione). */
struct TrackLive
{
    std::atomic<float> volumeDb { 0.0f }, pan { 0.0f };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<float> meterL { 0.0f }, meterR { 0.0f };   // livello dopo il guadagno in ingresso (mostrato nell'intestazione): ingresso se la traccia e' armata, altrimenti il segnale della traccia
    std::atomic<bool> mute { false }, solo { false };
    std::atomic<bool> invert { false };    // inversione di polarita' (traccia)
    ParamEq eq;                            // equalizzatore parametrico (solo tracce)
    TrackComp comp;                        // compressore di traccia (dopo l'EQ, prima degli inserti)
    std::atomic<int> trackType { 0 };      // tipo di traccia (preset): 0 nessuno, 1 dialogo, 2 musica, 3 effetti
    // limiter per traccia (limiterOn/ceilingDb/grDb sono gli stessi del master); lo stato e' del solo thread audio
    Limiter trackLimiter;
    double trackLimiterRate = 0.0;
    bool trackLimiterActive = false;
    std::atomic<float> trimDb { 0.0f };    // guadagno in ingresso della traccia (prima di EQ e compressore), -24..+24 dB
    float trimGainLast = -1.0f;            // stato del thread audio: guadagno applicato nel blocco precedente
    std::atomic<int> outBus { 0 };         // uscita della traccia: 0 = master, 1..4 = bus di gruppo
    std::atomic<bool> monoMon { false }, dimMon { false };   // solo master: controllo d'ascolto (non influisce sull'export)
    std::atomic<int> autoMode { 0 };       // automazione: 0 spenta, 1 lettura, 2 scrittura (vale per tutti i parametri della traccia)
    std::atomic<int> autoParam { 0 };      // parametro mostrato e modificato sulla timeline (AutoParam)
    std::atomic<int> height { 96 };        // altezza della traccia nella timeline (pixel, non soggetta a undo)

    // linea di ritardo per la compensazione di latenza (PDC): la usa solo il thread audio / l'esportazione
    static constexpr int kPdcCap = 32768;
    std::vector<float> pdcBuf[2];
    int pdcPos = 0;

    TrackLive()
    {
        pdcBuf[0].assign ((size_t) kPdcCap, 0.0f);
        pdcBuf[1].assign ((size_t) kPdcCap, 0.0f);
    }

    // send verso i ritorni FX A e B (dB, post-fader); -80 = spento
    std::atomic<float> sendDb[2] { -80.0f, -80.0f };

    // limiter (usato solo dal master)
    std::atomic<bool> limiterOn { true };
    std::atomic<float> ceilingDb { -1.0f };
    std::atomic<float> grDb { 0.0f };      // riduzione di guadagno massima nell'ultimo blocco (dB, >= 0)

    // inserti plug-in: la catena e' immutabile, per modificarla se ne pubblica una nuova
    mutable juce::SpinLock chainLock;
    std::shared_ptr<const InsertChain> chain;
    std::vector<PendingInsert> pending;

    std::shared_ptr<const InsertChain> getChain() const
    {
        const juce::SpinLock::ScopedLockType l (chainLock);
        return chain;
    }
    std::shared_ptr<const InsertChain> tryGetChain() const noexcept   // thread audio
    {
        const juce::SpinLock::ScopedTryLockType l (chainLock);
        return l.isLocked() ? chain : nullptr;
    }
    void setChain (std::shared_ptr<const InsertChain> c)              // thread messaggi
    {
        std::shared_ptr<const InsertChain> old;
        {
            const juce::SpinLock::ScopedLockType l (chainLock);
            old = std::move (chain);
            chain = std::move (c);
        }
        if (old != nullptr) retire (old);
    }
    std::atomic<bool> armed { false };     // armata per la registrazione
    std::atomic<int> inputMode { 0 };      // 0 = ingresso 1, 1 = ingresso 2, 2 = stereo 1+2

    void copyFrom (const TrackLive& o)
    {
        volumeDb = o.volumeDb.load(); pan = o.pan.load();
        mute = o.mute.load(); solo = o.solo.load();
        armed = o.armed.load(); inputMode = o.inputMode.load(); height = o.height.load(); autoMode = o.autoMode.load(); autoParam = o.autoParam.load(); invert = o.invert.load(); outBus = o.outBus.load();
        trimDb = o.trimDb.load(); eq.copyFrom (o.eq); comp.copyFrom (o.comp); trackType = o.trackType.load();
        limiterOn = o.limiterOn.load(); ceilingDb = o.ceilingDb.load();
        sendDb[0] = o.sendDb[0].load(); sendDb[1] = o.sendDb[1].load();
    }
};

/** Punto dell'automazione del volume (interpolazione lineare in dB). */
struct AutoPoint
{
    int id = 0;
    int64_t pos = 0;
    float db = 0.0f;
};

/** Parametri automatizzabili di una traccia. Volume e send in dB, pan da -1 (sinistra) a +1 (destra). */
enum AutoParam { AutoVolume = 0, AutoPan, AutoSendA, AutoSendB, kNumAutoParams };
inline const char* autoParamName (int p) { static const char* n[] = { "VOL", "PAN", "SEND A", "SEND B" }; return n[juce::jlimit (0, kNumAutoParams - 1, p)]; }
inline float clampAutoValue (int p, float v) { return p == AutoPan ? juce::jlimit (-1.0f, 1.0f, v) : (p == AutoVolume ? juce::jlimit (-80.0f, 6.0f, v) : juce::jlimit (-80.0f, 0.0f, v)); }

/** Valore dell'automazione (dB, o pan) alla posizione indicata; i punti devono essere ordinati per posizione. */
inline float evalAutomationDb (const std::vector<AutoPoint>& pts, int64_t pos)
{
    if (pts.empty()) return 0.0f;
    if (pos <= pts.front().pos) return pts.front().db;
    if (pos >= pts.back().pos) return pts.back().db;
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1)
    {
        const size_t mid = (lo + hi) / 2;
        if (pts[mid].pos <= pos) lo = mid; else hi = mid;
    }
    const double t = (double) (pos - pts[lo].pos) / (double) juce::jmax<int64_t> (1, pts[hi].pos - pts[lo].pos);
    return (float) ((double) pts[lo].db + t * (double) (pts[hi].db - pts[lo].db));
}

/** Gruppo di tracce (cartella): le tracce con lo stesso groupId, di norma vicine, si mostrano sotto una barra comune e si comprimono insieme. */
struct TrackGroup
{
    int id = 0;
    juce::String name;
    bool collapsed = false;
};

struct Track
{
    int id = 0;
    int groupId = 0;   // 0 = nessun gruppo
    bool locked = false;   // traccia bloccata: i suoi clip non si modificano (come Lock di Nuendo)
    bool hidden = false;   // traccia nascosta nella timeline e nel mixer (non nel mix)
    juce::String name;
    juce::Colour colour;
    std::shared_ptr<TrackLive> live = std::make_shared<TrackLive>();
    std::vector<Clip> clips;
    std::vector<AutoPoint> volAuto, panAuto, sendAAuto, sendBAuto;   // curve di automazione (ordinate per posizione)

    std::vector<AutoPoint>& lane (int p) noexcept { return p == AutoPan ? panAuto : (p == AutoSendA ? sendAAuto : (p == AutoSendB ? sendBAuto : volAuto)); }
    const std::vector<AutoPoint>& lane (int p) const noexcept { return p == AutoPan ? panAuto : (p == AutoSendA ? sendAAuto : (p == AutoSendB ? sendBAuto : volAuto)); }
};

/** Video di riferimento della sessione (l'audio originale, se c'e', viene estratto in una traccia). */
struct VideoAsset
{
    juce::File file;
    double duration = 0.0;      // secondi
    double fps = 25.0;
    int width = 0, height = 0;
    std::vector<juce::Image> thumbs;   // riempite in modo asincrono dopo il caricamento
    double thumbInterval = 1.0;        // secondi per miniatura
    double offsetSec = 0.0;            // > 0: il video parte dopo l'audio (ritardato); < 0: in anticipo
};

struct Marker
{
    int id = 0;
    int64_t pos = 0;      // campioni
    juce::String name;
    juce::Colour colour { 0xffffc94d };
};

/** Cue ADR (sostituzione di dialoghi). */
struct AdrCue
{
    int id = 0;
    int number = 0;                 // numero progressivo mostrato nella lista
    juce::String character, scene, reel, dialogue, notes;
    int64_t inPos = 0, outPos = 0;  // campioni sulla timeline
    int status = 0;                 // 0 da registrare, 1 registrato, 2 approvato, 3 da rifare
    int activeTake = 0;             // numero del take che suona (0 = nessuno)
};

inline juce::String adrStatusName (int s)
{
    static const char* n[] = { TRX ("Da registrare"), TRX ("Registrato"), TRX ("Approvato"), TRX ("Da rifare") };
    return juce::translate (n[juce::jlimit (0, 3, s)]);
}

/** ReConform: pezzo di timeline del vecchio montaggio che nel nuovo sta in un'altra posizione (campioni). */
struct ConformSegment { int64_t oldStart = 0, oldEnd = 0, newStart = 0; };
struct ConformPlan
{
    std::vector<ConformSegment> segments;                 // ordinati per oldStart
    std::vector<std::pair<int64_t, int64_t>> coverage;     // zone coperte dal vecchio montaggio: fuori da qui nulla cambia
    std::vector<std::pair<int64_t, int64_t>> inserted;     // materiale nuovo senza audio (campioni nel nuovo montaggio)
    bool markInserted = true;
};
struct ConformStats
{
    int clipsTouched = 0, clipPieces = 0, clipsDropped = 0, cuesMoved = 0, cuesOut = 0, markersMoved = 0, insertedMarkers = 0;
    double droppedSeconds = 0.0;
};

struct Session
{
    double sampleRate = 48000.0;   // frequenza del progetto (serve ai filtri)
    std::vector<Track> tracks;
    std::vector<TrackGroup> groups;
    std::vector<Marker> markers;
    std::vector<AdrCue> cues;
    std::shared_ptr<VideoAsset> video;
    std::shared_ptr<TrackLive> master = std::make_shared<TrackLive>();
    std::shared_ptr<TrackLive> aux[2] { std::make_shared<TrackLive>(), std::make_shared<TrackLive>() };   // ritorni FX A e B
    static constexpr int kNumGroups = 4;
    std::shared_ptr<TrackLive> group[kNumGroups] { std::make_shared<TrackLive>(), std::make_shared<TrackLive>(), std::make_shared<TrackLive>(), std::make_shared<TrackLive>() };   // bus di gruppo

    int64_t endSample() const noexcept
    {
        int64_t e = 0;
        for (auto& t : tracks) for (auto& c : t.clips) e = juce::jmax (e, c.end());
        return e;
    }

    /** Fine della riproduzione: oltre l'audio contano la fine del video e l'uscita dell'ultimo cue (un video senza audio non deve fermarsi dopo un secondo). */
    int64_t playEnd() const noexcept
    {
        int64_t e = endSample();
        if (video != nullptr) e = juce::jmax (e, (int64_t) std::llround ((video->duration + juce::jmax (0.0, video->offsetSec)) * sampleRate));
        for (auto& c : cues) e = juce::jmax (e, c.outPos);
        return e;
    }

    /** Calcola i crossfade impliciti dove due clip della stessa traccia si sovrappongono parzialmente. */
    void computeCrossfades()
    {
        for (auto& t : tracks)
        {
            for (auto& c : t.clips) { c.xfadeIn = 0; c.xfadeOut = 0; c.xLinear = false; }
            for (size_t i = 0; i < t.clips.size(); ++i)
                for (size_t j = 0; j < t.clips.size(); ++j)
                {
                    if (i == j) continue;
                    auto& a = t.clips[i];   // primo (inizia prima)
                    auto& b = t.clips[j];   // secondo
                    if (a.muted || b.muted) continue;
                    if (a.start < b.start && b.start < a.end() && a.end() <= b.end())
                    {
                        const int64_t ovl = a.end() - b.start;
                        b.xfadeIn = juce::jmax (b.xfadeIn, ovl);
                        a.xfadeOut = juce::jmax (a.xfadeOut, ovl);
                        const bool continuous = a.asset == b.asset && a.asset != nullptr && a.offset + (b.start - a.start) == b.offset;
                        if (continuous) { a.xLinear = true; b.xLinear = true; }
                    }
                }
        }
    }

    /** Copia con parametri live indipendenti (per l'esportazione offline). */
    Session cloneDetached() const
    {
        Session c;
        c.sampleRate = sampleRate;
        c.tracks = tracks;
        c.markers = markers;
        c.cues = cues;
        for (auto& t : c.tracks)
        {
            auto n = std::make_shared<TrackLive>();
            n->copyFrom (*t.live);
            n->chain = t.live->getChain();     // gli stessi plug-in (l'esportazione e' l'unico a usarli mentre il trasporto e' fermo)
            t.live = n;
        }
        c.master = std::make_shared<TrackLive>();
        c.master->copyFrom (*master);
        c.master->chain = master->getChain();
        for (int k = 0; k < 2; ++k)
        {
            c.aux[k] = std::make_shared<TrackLive>();
            c.aux[k]->copyFrom (*aux[k]);
            c.aux[k]->chain = aux[k]->getChain();
        }
        for (int k = 0; k < kNumGroups; ++k)
        {
            c.group[k] = std::make_shared<TrackLive>();
            c.group[k]->copyFrom (*group[k]);
            c.group[k]->chain = group[k]->getChain();
        }
        c.computeCrossfades();
        return c;
    }
};

//==============================================================================
/** Modello della sessione (solo thread messaggi): tracce, clip, selezione, undo/redo, salvataggio. */
class Project : public juce::ChangeBroadcaster
{
public:
    Project (juce::AudioFormatManager& fm, double rate);
    ~Project() override;

    std::function<void (std::shared_ptr<const Session>)> onPublish;
    std::function<void()> onSessionLoaded;   // dopo il caricamento di una sessione: ripristinare i plug-in

    const Session& getSession() const noexcept { return session; }
    Session& edit() noexcept { return session; }  // chiamare changed() dopo le modifiche

    juce::AudioFormatManager& getFormats() noexcept { return formats; }
    double getSampleRate() const noexcept { return sampleRate; }
    void setSampleRate (double newRate);

    // selezione
    std::set<int64_t> selection;
    int selectedTrackId = -1;
    // selezione di piu' tracce (Cmd/Maiusc + clic sull'intestazione): vale finche' la traccia principale ne fa parte
    std::set<int> trackSelection;
    int trackAnchorId = -1;
    std::vector<int> selectedTracks() const;            // nell'ordine delle tracce
    bool isTrackSelected (int trackId) const;
    void selectOnlyTrack (int trackId);
    void toggleTrackSelection (int trackId);
    void selectTrackRange (int trackId);                // dall'ultima traccia scelta senza Maiusc a questa

    // stato
    bool isDirty() const noexcept { return dirty; }
    juce::File getFile() const { return sessionFile; }
    int getBusyCount() const noexcept { return busy; }
    /** Avanzamento (0..1) dell'operazione lunga in corso (caricamento video), o -1 se non ce n'e'. */
    float getTaskProgress() const noexcept { return taskProgress.load(); }
    juce::String getTaskLabel() const { const juce::ScopedLock sl (taskLock); return taskLabel; }

    // undo/redo
    void beginEdit();
    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    void undo();
    void redo();

    /** Pubblica la sessione al motore audio e avvisa la UI. */
    void changed();
    /** I soli parametri live (fader, mute...) sono cambiati. */
    void liveChanged();

    // ricerca
    int trackIndexForId (int id) const noexcept;
    Track* findTrack (int id) noexcept;
    Clip* findClip (int64_t id, int* trackIndex = nullptr) noexcept;

    // tracce
    int addTrack();
    /** Aggiunge una traccia gia' impostata: nome, ruolo (applica il preset), colore e bus di uscita (un solo passo di undo). */
    int addTrackAs (const juce::String& name, int type, juce::Colour colour, int outBus);
    void setTrackOutBus (int trackId, int bus);
    void removeTrack (int id);
    /** Applica a una traccia il preset del tipo scelto (EQ, compressore, limiter, livello) e ne ricorda il tipo. */
    void setTrackType (int trackId, int type);
    void moveTrackTo (int fromIndex, int toIndex);
    // gruppi di tracce
    int addGroup (const juce::String& name);                  // ritorna l'id (esiste gia' un gruppo con quel nome: lo riusa)
    void setTrackGroup (int trackId, int groupId);
    void setTracksGroup (const std::vector<int>& trackIds, int groupId);   // un solo passo di undo            // 0 = fuori dai gruppi; la traccia si sposta accanto alle altre del gruppo
    void renameGroup (int groupId, const juce::String& name);
    void removeGroup (int groupId);                           // le tracce restano, senza gruppo
    void setGroupCollapsed (int groupId, bool collapsed);     // solo vista: nessun passo di undo
    void setGroupMuteSolo (int groupId, bool solo, bool on);  // muto/solo di tutte le tracce del gruppo
    const TrackGroup* findGroup (int groupId) const noexcept;
    void duplicateTrack (int id);
    void renameTrack (int id, const juce::String& name);
    // blocco, visibilita', mute/solo globali: stato di lavoro, senza passo di undo
    void setTracksLocked (const std::vector<int>& ids, bool locked);
    void setTracksHidden (const std::vector<int>& ids, bool hidden);
    void showAllTracks();
    void hideEmptyTracks();
    bool trackLocked (int trackId) const noexcept;
    bool anyTrackMuted() const noexcept;
    bool anyTrackSoloed() const noexcept;
    void clearAllMute();    // "Annulla tutti i mute" di Nuendo
    void clearAllSolo();    // "Disattiva tutti i solo"
    void setTrackColour (int id, juce::Colour c);

    // registrazione: un clip per ogni take, posizionato compensando la latenza
    struct RecItem { int trackId; juce::File file; int64_t startPos; int64_t latency; int cueId = 0; juce::String takeName; };
    void addRecordings (std::vector<RecItem> items, std::function<void (juce::StringArray)> done);

    // take gia' registrate (importate da file): ognuna al suo cue e numero; 'best' = e' quella scelta, altrimenti suona la piu' alta
    struct TakeFile { int trackId = -1; juce::File file; int64_t startPos = 0; int cueId = 0; int takeNo = 1; bool best = false; };
    void importTakes (std::vector<TakeFile> items, std::function<void (juce::StringArray, int imported)> done);

    // automazione: le prime quattro lavorano sulla curva scelta della traccia (TrackLive::autoParam)
    int nextAutoPointId() noexcept { return nextAutoId++; }   // per chi scrive curve in blocco (ducking)
    /** Aggiunge in un solo passo di undo i marker (posizione, nome), per esempio dal rilevamento dei tagli del video. */
    void addMarkers (const std::vector<std::pair<int64_t, juce::String>>& list, juce::Colour colour);
    int addAutoPoint (int trackId, int64_t pos, float value);                    // con undo
    void removeAutoPoint (int trackId, int pointId);                             // con undo
    void clearAutomation (int trackId);                                          // con undo
    void moveAutoPoint (int trackId, int pointId, int64_t pos, float value);     // senza undo (trascinamento)
    /** Scrittura "touch" mentre si muove un controllo (fader, pan, send) durante la riproduzione. */
    void writeAuto (int trackId, int param, int64_t pos, float value);
    void writeVolumeAuto (int trackId, int64_t pos, float db) { writeAuto (trackId, AutoVolume, pos, db); }
    void endAutoTouch() { touchTrack = -1; }
    std::function<bool()> isPlayingFn;           // impostati da chi possiede il trasporto
    std::function<int64_t()> positionFn;

    // ADR
    int addCue (const AdrCue& cue);                                   // un solo passo di undo
    int addCues (std::vector<AdrCue> cues, bool replaceExisting);     // importazione
    void removeCue (int id);
    void editCue (int id, const std::function<void (AdrCue&)>& change);
    AdrCue* findCue (int id) noexcept;
    const AdrCue* findCue (int id) const noexcept;
    int takeCount (int cueId) const;
    int nextTakeNumber (int cueId) const;
    /** Rende attivo un take: gli altri clip dello stesso cue vengono silenziati. */
    void setActiveTake (int cueId, int takeNo);
    std::vector<int64_t> takeClipIds (int cueId) const;
    void deleteTake (int cueId, int takeNo);

    // marker
    int addMarker (int64_t pos, const juce::String& name = {});
    void removeMarker (int id);
    void renameMarker (int id, const juce::String& name);
    void setMarkerColour (int id, juce::Colour c);
    Marker* findMarker (int id) noexcept;
    int64_t nextMarkerPos (int64_t pos) const;       // -1 se non ce ne sono
    int64_t previousMarkerPos (int64_t pos) const;

    // video
    void setVideo (const juce::File& f, std::function<void (juce::String)> done);
    void clearVideo();
    void setVideoOffset (double seconds);   // non soggetto a undo, come i fader
    double getVideoFps() const noexcept { return session.video != nullptr ? session.video->fps : 25.0; }

    // clip
    void importFiles (juce::Array<juce::File> files, int trackId, int64_t start,
                      std::function<void (juce::StringArray)> done);
    void moveClipTo (int64_t clipId, int trackIndex, int64_t start); // senza undo (drag)
    void splitAt (int64_t pos, bool allTracks = false);   // clip selezionati sotto la testina, altrimenti tutti (o sempre tutti)
    void splitClip (int64_t clipId, int64_t pos);         // un solo clip (strumento Lama)
    void rippleDeleteSelected();                          // elimina i clip selezionati chiudendo il vuoto sulla stessa traccia
    void trimToPlayhead (bool trimStart, int64_t pos);    // taglia l'inizio/la fine dei clip alla testina
    void deleteSelected();
    void duplicateSelected();
    void selectAll();
    void normalizeClip (int64_t id);
    void setClipGain (int64_t id, float db);
    /** JOIN: i clip selezionati della stessa traccia (almeno due, non silenziati) diventano un solo clip, con gli spazi vuoti a silenzio; il nuovo file va in 'folder'. Un solo passo di undo. */
    void joinClips (const std::vector<int64_t>& ids, const juce::File& folder, std::function<void (juce::String)> done);
    /** Quanti gruppi di clip (per traccia) il JOIN unirebbe con questa selezione. */
    int joinableGroups (const std::set<int64_t>& ids) const;
    /** Cambia la durata del clip (ratio = durata finale / originale): ricalcola l'audio in un nuovo file in 'folder' e lo sostituisce (un passo di undo). */
    void stretchClip (int64_t id, double ratio, bool keepPitch, const juce::File& folder, std::function<void (juce::String)> done, int64_t newStart = -1);

    int64_t contentEnd() const noexcept { return session.endSample(); }

    // timecode della sessione (come in Pro Tools) e impostazioni ADR; non soggetti a undo
    int tcFormat = 2;              // indice in tcFormats()
    double tcStartSec = 0.0;       // timecode della posizione 0 della timeline, in secondi (es. 3600 = 01:00:00:00)
    bool rulerTimecode = true;     // il righello mostra il timecode invece dei minuti:secondi
    double adrPreRoll = 3.0, adrPostRoll = 1.0;   // secondi
    int adrBeeps = 3;              // numero di bip prima dell'ingresso (0 = nessuno)
    bool adrStreamer = true;       // streamer visivo sul video

    const TcFormat& tcFmt() const noexcept { return tcFormats()[juce::jlimit (0, numTcFormats - 1, tcFormat)]; }
    double frameRate() const noexcept { return tcFmt().fps; }
    /** Secondi della timeline -> timecode della sessione. */
    juce::String formatTc (double timelineSeconds) const { return secondsToTimecode (timelineSeconds + tcStartSec, tcFmt()); }
    /** Timecode della sessione -> secondi della timeline (anche negativi). Ritorna false se il testo non e' un timecode. */
    bool parseTc (const juce::String& text, double& timelineSeconds) const
    {
        double abs = 0.0;
        if (! timecodeToSeconds (text, tcFmt(), abs)) return false;
        timelineSeconds = abs - tcStartSec;
        return true;
    }

    int64_t exportIn = -1, exportOut = -1;   // intervallo di esportazione scelto sul righello (campioni, -1 = nessuno)
    int adrDialogueSize = 30;      // dimensione (px) del dialogo mostrato sotto il video
    /** Schema dei nomi dei take registrati: {personaggio} {scena} {bobina} {cue} {take} {data} {progetto}. */
    juce::String adrNaming = "{personaggio}_{scena}_{cue}_T{take}";
    int selectedCueId = 0;         // cue ADR selezionato (stato dell'interfaccia)
    int adrTrackId = -1;           // traccia su cui si registra l'ADR (-1 = la traccia selezionata)

    int loudnessStandard = 1;   // indice in Loudness::standards() (1 = EBU R128); non soggetto a undo

    // salvataggio
    bool save (const juce::File& f, juce::String& error);
    /** Scrive una copia (es. salvataggio automatico) senza cambiare file corrente ne' stato "modificato". */
    bool saveCopy (const juce::File& f, juce::String& error) const;
    void setFile (const juce::File& f) { sessionFile = f; sendChangeMessage(); }
    void markDirty() { dirty = true; sendChangeMessage(); }
    void loadSession (const juce::File& f, std::function<void (juce::String)> done);
    void newSession (bool withDefaultTrack = true);

    /** ReConform: sposta clip, cue e marker secondo il piano. Con apply = false calcola solo il rapporto (nulla cambia). Un solo passo di undo. */
    ConformStats conform (const ConformPlan&, bool apply);

    // media mancanti
    juce::String missingVideoPath;     // video della sessione che non si e' trovato all'apertura (resta nel salvataggio)
    double missingVideoOffset = 0.0;
    bool hasOfflineMedia() const noexcept;
    /** Sostituisce, in tutti i clip, gli asset indicati (per esempio quelli offline appena ricollegati). Un solo passo di undo. */
    void replaceAssets (const std::map<const Asset*, AssetPtr>& replacements);
    /** Rilega il video mancante senza toccare l'audio della sessione; mantiene l'offset salvato. */
    void relinkVideo (const juce::File& f, std::function<void (juce::String)> done);

private:
    int addTrackInternal();
    void publish();
    /** Esegue work in un thread; la funzione restituita viene eseguita sul thread messaggi (se il progetto esiste ancora). */
    void runAsync (std::function<std::function<void()>()> work);
    void pruneSelection();
    juce::var toVar() const;

    juce::AudioFormatManager& formats;
    double sampleRate;
    Session session;
    std::vector<Session> undoStack, redoStack;
    int nextTrackId = 1;
    int nextMarkerId = 1;
    int nextCueId = 1;
    int nextAutoId = 1;
    int touchTrack = -1, touchParam = 0;
    int64_t touchLastPos = 0;
    int64_t nextClipId = 1;
    bool dirty = false;
    juce::File sessionFile;
    std::atomic<int> busy { 0 };
    std::atomic<float> taskProgress { -1.0f };
    mutable juce::CriticalSection taskLock;
    juce::String taskLabel;
    void setTask (const juce::String& label, float p) { { const juce::ScopedLock sl (taskLock); taskLabel = label; } taskProgress = p; }
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace fc
