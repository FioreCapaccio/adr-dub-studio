#include "AraEdit.h"

#if ADR_ARA
namespace fc::ara
{
using namespace juce;

namespace
{
struct Reader { const AudioBuffer<float>* audio; };

class AudioAccess : public ARA::Host::AudioAccessControllerInterface
{
public:
    explicit AudioAccess (const AudioBuffer<float>& a) : audio (a) {}
    ARA::ARAAudioReaderHostRef createAudioReaderForSource (ARA::ARAAudioSourceHostRef, bool) noexcept override
    { return reinterpret_cast<ARA::ARAAudioReaderHostRef> (new Reader { &audio }); }

    bool readAudioSamples (ARA::ARAAudioReaderHostRef r, ARA::ARASamplePosition pos, ARA::ARASampleCount n, void* const buffers[]) noexcept override
    {
        auto& a = *reinterpret_cast<Reader*> (r)->audio;
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
        {
            auto* dst = static_cast<float*> (buffers[ch]);
            for (ARA::ARASampleCount i = 0; i < n; ++i)
            {
                const auto p = pos + i;
                dst[i] = (p >= 0 && p < a.getNumSamples()) ? a.getSample (ch, (int) p) : 0.0f;
            }
        }
        return true;
    }
    void destroyAudioReader (ARA::ARAAudioReaderHostRef r) noexcept override { delete reinterpret_cast<Reader*> (r); }
private:
    const AudioBuffer<float>& audio;
};

// Il documento non si salva nella sessione: l'audio modificato si rende e si applica al clip.
class Archiving : public ARA::Host::ArchivingControllerInterface
{
public:
    ARA::ARASize getArchiveSize (ARA::ARAArchiveReaderHostRef) noexcept override { return 0; }
    bool readBytesFromArchive (ARA::ARAArchiveReaderHostRef, ARA::ARASize, ARA::ARASize, ARA::ARAByte[]) noexcept override { return false; }
    bool writeBytesToArchive (ARA::ARAArchiveWriterHostRef, ARA::ARASize, ARA::ARASize, const ARA::ARAByte[]) noexcept override { return true; }
    void notifyDocumentArchivingProgress (float) noexcept override {}
    void notifyDocumentUnarchivingProgress (float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID (ARA::ARAArchiveReaderHostRef) noexcept override { return "adr.dubstudio.archive"; }
};

class ModelUpdates : public ARA::Host::ModelUpdateControllerInterface
{
public:
    void notifyAudioSourceAnalysisProgress (ARA::ARAAudioSourceHostRef, ARA::ARAAnalysisProgressState, float) noexcept override {}
    void notifyAudioSourceContentChanged (ARA::ARAAudioSourceHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyAudioModificationContentChanged (ARA::ARAAudioModificationHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyPlaybackRegionContentChanged (ARA::ARAPlaybackRegionHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
};

// il tasto play dell'interfaccia del plug-in chiede all'host di suonare
class Transport : public ARA::Host::PlaybackControllerInterface
{
public:
    std::atomic<bool> playing { false };
    std::atomic<double> seconds { 0.0 };   // posizione richiesta dal plug-in (aggiornata dal thread audio mentre suona)
    std::atomic<int64> pos { 0 };
    std::atomic<double> rate { 44100.0 };
    void requestStartPlayback() noexcept override { playing = true; }
    void requestStopPlayback() noexcept override { playing = false; }
    void requestSetPlaybackPosition (ARA::ARATimePosition t) noexcept override { pos = (int64) (t * rate.load()); }
    void requestSetCycleRange (ARA::ARATimePosition, ARA::ARATimeDuration) noexcept override {}
    void requestEnableCycle (bool) noexcept override {}
};

struct Head : AudioPlayHead
{
    int64 pos = 0; double rate = 44100; bool playing = true;
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setTimeInSamples (pos); p.setTimeInSeconds ((double) pos / rate); p.setIsPlaying (playing);
        p.setBpm (120.0); p.setTimeSignature (TimeSignature { 4, 4 }); p.setPpqPosition ((double) pos / rate * 2.0); p.setPpqPositionOfLastBarStart (0.0);
        return p;
    }
};
}

//==============================================================================
struct Session::Impl : public AudioIODeviceCallback
{
    Source src;
    std::unique_ptr<AudioPluginInstance> inst;
    ARAFactoryWrapper factory;
    Transport* transport = nullptr;   // di proprieta' del controller del documento
    std::unique_ptr<ARAHostDocumentController> dc;
    std::unique_ptr<ARAHostModel::MusicalContext> ctx;
    std::unique_ptr<ARAHostModel::RegionSequence> seq;
    std::unique_ptr<ARAHostModel::AudioSource> as;
    std::unique_ptr<ARAHostModel::AudioModification> am;
    std::unique_ptr<ARAHostModel::PlaybackRegion> region;
    ARAHostModel::PlugInExtensionInstance ext;
    ARAHostModel::PlaybackRendererInterface pb;
    ARAHostModel::EditorRendererInterface ed;
    Head head;
    AudioDeviceManager* dev = nullptr;
    AudioBuffer<float> blk;
    MidiBuffer midi;

    ~Impl() override
    {
        detachPreview();
        // Il distruttore di PlaybackRegionRegistry di JUCE cancella dalla mappa mentre la scorre (UB, crash): si tolgono le regioni a mano.
        if (inst != nullptr) inst->releaseResources();
        if (region != nullptr && ext.isValid()) { ed.remove (*region); pb.remove (*region); }
        ed = {};          // prima le regioni dai renderer, poi il plug-in, poi il documento (ordine inverso di dichiarazione)
        pb = {};
        ext = {};
        inst.reset();
    }

    void detachPreview() { if (dev != nullptr) { dev->removeAudioCallback (this); dev = nullptr; } }

    //==============================================================================
    void audioDeviceAboutToStart (AudioIODevice* d) override
    {
        const double r = d->getCurrentSampleRate();
        const int bs = d->getCurrentBufferSizeSamples();
        transport->rate = r;
        head.rate = r;
        inst->releaseResources();
        inst->setNonRealtime (false);
        inst->setRateAndBufferSizeDetails (r, bs);
        inst->prepareToPlay (r, bs);
        blk.setSize (jmax (2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels()), bs, false, false, true);
    }
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n, const AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOut; ++ch) FloatVectorOperations::clear (out[ch], n);
        if (! transport->playing || n > blk.getNumSamples()) return;
        const int64 endPos = (int64) ((double) src.audio.getNumSamples() / src.rate * transport->rate.load());
        const int64 p = transport->pos;
        if (p >= endPos) { transport->playing = false; return; }
        head.pos = p;
        blk.clear();
        AudioBuffer<float> view (blk.getArrayOfWritePointers(), blk.getNumChannels(), n);
        inst->setPlayHead (&head);
        inst->processBlock (view, midi);
        for (int ch = 0; ch < numOut; ++ch) FloatVectorOperations::copy (out[ch], view.getReadPointer (jmin (ch, view.getNumChannels() - 1)), n);
        transport->pos = p + n;
    }
};

Session::Session (std::shared_ptr<Impl> i) : impl (std::move (i)) {}
Session::~Session() = default;

AudioProcessor& Session::plugin() { return *impl->inst; }
String Session::pluginName() const { return impl->inst->getName(); }
bool Session::hasEditor() const { return impl->inst->hasEditor(); }
AudioProcessorEditor* Session::createEditor() { return impl->inst->createEditorIfNeeded(); }

void Session::attachPreview (AudioDeviceManager& m)
{
    impl->detachPreview();
    impl->dev = &m;
    m.addAudioCallback (impl.get());
}

AudioBuffer<float> Session::render()
{
    auto& s = *impl;
    s.detachPreview();
    const int bs = 512, n = s.src.audio.getNumSamples();
    s.inst->releaseResources();
    s.inst->setNonRealtime (true);
    s.inst->setRateAndBufferSizeDetails (s.src.rate, bs);
    s.inst->prepareToPlay (s.src.rate, bs);
    s.head.rate = s.src.rate;
    s.inst->setPlayHead (&s.head);
    const int latency = jmax (0, s.inst->getLatencySamples());
    const int chans = jmax (2, s.inst->getTotalNumInputChannels(), s.inst->getTotalNumOutputChannels());
    AudioBuffer<float> blk (chans, bs), out (2, n);
    out.clear();
    MidiBuffer midi;
    for (int pos = 0; pos < n + latency; pos += bs)
    {
        const int len = jmin (bs, n + latency - pos);
        s.head.pos = pos;
        blk.clear();
        AudioBuffer<float> view (blk.getArrayOfWritePointers(), chans, len);
        s.inst->processBlock (view, midi);
        for (int ch = 0; ch < 2; ++ch)
        {
            const int from = jmax (0, latency - pos), dst = pos + from - latency;
            if (from < len) out.copyFrom (ch, dst, view, jmin (ch, chans - 1), from, len - from);
        }
    }
    s.inst->releaseResources();
    return out;
}

void Session::create (Source source, const File& vst3, std::function<void (std::unique_ptr<Session>, String)> done)
{
    auto impl = std::make_shared<Impl>();
    impl->src = std::move (source);
    auto fmt = std::make_shared<VST3PluginFormat>();
    OwnedArray<PluginDescription> descs;
    fmt->findAllTypesForFile (descs, vst3.getFullPathName());
    if (descs.isEmpty()) { done (nullptr, TRANS ("Il file non contiene un plug-in VST3 valido.")); return; }

    fmt->createPluginInstanceAsync (*descs[0], impl->src.rate, 512,
        [impl, fmt, done] (std::unique_ptr<AudioPluginInstance> p, const String& err)
        {
            if (p == nullptr) { done (nullptr, TRANS ("Non riesco a caricare il plug-in: ") + err); return; }
            impl->inst = std::move (p);
            createARAFactoryAsync (*impl->inst, [impl, done] (ARAFactoryWrapper f)
            {
                auto& s = *impl;
                if (f.get() == nullptr) { done (nullptr, TRANS ("Questo plug-in non e' un plug-in ARA: non si puo' usare per modificare un clip.")); return; }
                s.factory = std::move (f);
                auto tr = std::make_unique<Transport>();
                s.transport = tr.get();
                s.dc = ARAHostDocumentController::create (s.factory, "ADR Dub Studio", std::make_unique<AudioAccess> (s.src.audio), std::make_unique<Archiving>(),
                                                          nullptr, std::make_unique<ModelUpdates>(), std::move (tr));
                if (s.dc == nullptr) { done (nullptr, TRANS ("Il plug-in non accetta il documento ARA.")); return; }

                const double len = s.src.audio.getNumSamples() / s.src.rate;
                auto& d = s.dc->getDocumentController();
                auto mcp = ARAHostModel::MusicalContext::getEmptyProperties(); mcp.name = "ADR"; mcp.orderIndex = 0; mcp.color = nullptr;
                s.ctx = std::make_unique<ARAHostModel::MusicalContext> (reinterpret_cast<ARA::ARAMusicalContextHostRef> (1), d, mcp);
                auto rsp = ARAHostModel::RegionSequence::getEmptyProperties(); rsp.name = "Traccia"; rsp.orderIndex = 0; rsp.musicalContextRef = s.ctx->getPluginRef(); rsp.color = nullptr;
                s.seq = std::make_unique<ARAHostModel::RegionSequence> (reinterpret_cast<ARA::ARARegionSequenceHostRef> (2), d, rsp);
                const auto name = s.src.name.toRawUTF8();
                auto asp = ARAHostModel::AudioSource::getEmptyProperties(); asp.name = name; asp.persistentID = "clip-1";
                asp.sampleCount = s.src.audio.getNumSamples(); asp.sampleRate = s.src.rate; asp.channelCount = s.src.audio.getNumChannels(); asp.merits64BitSamples = false;
                s.as = std::make_unique<ARAHostModel::AudioSource> (reinterpret_cast<ARA::ARAAudioSourceHostRef> (3), d, asp);
                s.as->enableAudioSourceSamplesAccess (true);
                auto amp = ARAHostModel::AudioModification::getEmptyProperties(); amp.name = name; amp.persistentID = "clip-1-mod";
                s.am = std::make_unique<ARAHostModel::AudioModification> (reinterpret_cast<ARA::ARAAudioModificationHostRef> (4), d, *s.as, amp);
                auto prp = ARAHostModel::PlaybackRegion::getEmptyProperties();
                prp.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
                prp.startInModificationTime = 0.0; prp.durationInModificationTime = len;
                prp.startInPlaybackTime = 0.0; prp.durationInPlaybackTime = len;
                prp.musicalContextRef = nullptr; prp.regionSequenceRef = s.seq->getPluginRef(); prp.name = name; prp.color = nullptr;
                s.region = std::make_unique<ARAHostModel::PlaybackRegion> (reinterpret_cast<ARA::ARAPlaybackRegionHostRef> (5), d, *s.am, prp);

                const auto roles = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole;
                s.ext = s.dc->bindDocumentToPluginInstance (*s.inst, roles, roles);
                s.pb = s.ext.getPlaybackRendererInterface();
                s.ed = s.ext.getEditorRendererInterface();
                if (! s.ext.isValid() || ! s.pb.isValid()) { done (nullptr, TRANS ("Il plug-in non offre il motore di riproduzione ARA.")); return; }
                s.pb.add (*s.region);
                if (s.ed.isValid()) s.ed.add (*s.region);
                done (std::unique_ptr<Session> (new Session (impl)), {});
            });
        });
}

//==============================================================================
namespace
{
struct Panel : public Component
{
    Panel (AudioProcessorEditor* e, std::function<void()> apply, std::function<void()> cancel) : editor (e)
    {
        addAndMakeVisible (editor);
        hint.setText (TRANS ("Modifica il clip nell'interfaccia del plug-in, poi premi Applica: l'audio modificato sostituisce quello del clip (si annulla con Modifica > Annulla)."), dontSendNotification);
        hint.setJustificationType (Justification::centredLeft);
        hint.setFont (FontOptions (12.0f));
        addAndMakeVisible (hint);
        applyBtn.setButtonText (TRANS ("Applica al clip"));
        cancelBtn.setButtonText (TRANS ("Chiudi senza applicare"));
        applyBtn.onClick = std::move (apply);
        cancelBtn.onClick = std::move (cancel);
        addAndMakeVisible (applyBtn);
        addAndMakeVisible (cancelBtn);
        setSize (jmax (640, editor->getWidth()), editor->getHeight() + barH);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (barH).reduced (8, 6);
        cancelBtn.setBounds (bar.removeFromRight (160));
        bar.removeFromRight (8);
        applyBtn.setBounds (bar.removeFromRight (140));
        hint.setBounds (bar);
        editor->setBounds (r.withSizeKeepingCentre (jmin (r.getWidth(), editor->getWidth()), r.getHeight()));
    }
    static constexpr int barH = 44;
    AudioProcessorEditor* editor;
    Label hint;
    TextButton applyBtn, cancelBtn;
};

class Window : public DocumentWindow
{
public:
    Window (std::unique_ptr<Session> s, const String& title, std::function<void (AudioBuffer<float>)> onApply, std::function<void()> onClosed)
        : DocumentWindow (title, Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          session (std::move (s)), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        auto* editor = session->hasEditor() ? session->createEditor() : nullptr;
        if (editor == nullptr) editor = new GenericAudioProcessorEditor (session->plugin());
        auto* panel = new Panel (editor,
            [this, onApply = std::move (onApply)] { auto out = session->render(); onApply (std::move (out)); requestClose(); },
            [this] { requestClose(); });
        setContentOwned (panel, true);
        setResizable (false, false);
        setVisible (true);
    }
    ~Window() override { clearContentComponent(); session.reset(); }   // l'editor prima del plug-in
    void closeButtonPressed() override { requestClose(); }
    Session& get() { return *session; }
private:
    void requestClose() { if (! closing) { closing = true; MessageManager::callAsync ([c = closed] { c(); }); } }
    std::unique_ptr<Session> session;
    std::function<void()> closed;
    bool closing = false;
};

std::unique_ptr<Window> currentWindow;
}

void openWindow (Source src, const File& vst3, AudioDeviceManager& dm, Component* centreAround,
                 std::function<void (AudioBuffer<float>)> onApply, std::function<void (String)> onError)
{
    if (currentWindow != nullptr) { currentWindow->toFront (true); return; }
    const auto title = src.name;
    Session::create (std::move (src), vst3, [&dm, centreAround = Component::SafePointer<Component> (centreAround), title, onApply = std::move (onApply), onError = std::move (onError)] (std::unique_ptr<Session> s, String err) mutable
    {
        if (s == nullptr) { if (onError) onError (err); return; }
        const auto name = s->pluginName() + " - " + title;
        s->attachPreview (dm);
        currentWindow = std::make_unique<Window> (std::move (s), name, std::move (onApply), [] { currentWindow.reset(); });
        if (centreAround != nullptr) currentWindow->centreAroundComponent (centreAround, currentWindow->getWidth(), currentWindow->getHeight());
    });
}
}
#endif
