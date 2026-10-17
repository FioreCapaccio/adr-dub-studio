#include "PluginHost.h"
#include "PluginUi.h"

namespace fc
{
using namespace theme;

//==============================================================================
PluginWindow::PluginWindow (std::shared_ptr<InsertSlot> s, const juce::String& title, std::function<void (PluginWindow*)> onClose, std::function<void()> onBypassChanged)
    : juce::DocumentWindow (title, bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      slot (std::move (s)), closeCallback (std::move (onClose))
{
    setUsingNativeTitleBar (true);

    auto& p = *slot->plugin;
    juce::AudioProcessorEditor* editor = nullptr;
    if (auto* fx = dynamic_cast<BuiltinFx*> (&p)) editor = new BuiltinEditor (*fx, slot->bypass, std::move (onBypassChanged));
    else if (p.hasEditor()) editor = p.createEditorIfNeeded();
    if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (p);

    setContentOwned (editor, true);
    const bool generic = dynamic_cast<juce::GenericAudioProcessorEditor*> (editor) != nullptr;
    setResizable (editor->isResizable() || generic, false);   // l'editor generico (plug-in senza interfaccia) e' scorrevole: si puo' allargare
    if (generic) setResizeLimits (360, 160, 700, 1000);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

//==============================================================================
PluginHost::PluginHost (AudioEngine& e) : engine (e)
{
    formats.addFormat (new juce::VST3PluginFormat());
}

PluginHost::~PluginHost()
{
    *alive = false;
    closeAllWindows();
}

void PluginHost::closeAllWindows() { windows.clear(); }

juce::Array<juce::File> PluginHost::findInstalledVST3()
{
    juce::Array<juce::File> result;
    std::function<void (const juce::File&, int)> scan = [&] (const juce::File& dir, int depth)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
        {
            const juce::File f = entry.getFile();
            if (f.getFileName().startsWithChar ('.')) continue;
            if (f.hasFileExtension ("vst3")) result.add (f);
            else if (f.isDirectory() && depth < 2) scan (f, depth + 1);
        }
    };
#if JUCE_WINDOWS
    scan (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("Common Files/VST3"), 0);
    scan (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getParentDirectory().getChildFile ("Local/Programs/Common/VST3"), 0);
#else
    scan (juce::File ("/Library/Audio/Plug-Ins/VST3"), 0);
    scan (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3"), 0);
#endif

    struct ByName
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileNameWithoutExtension().compareIgnoreCase (b.getFileNameWithoutExtension());
        }
    } sorter;
    result.sort (sorter);
    return result;
}

//==============================================================================
juce::String PluginHost::prepareInstance (juce::AudioProcessor& p)
{
    // layout stereo sul bus principale, bus ausiliari (sidechain) disattivati
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.size() > 0) layout.getChannelSet (true, 0) = juce::AudioChannelSet::stereo();
    if (layout.outputBuses.size() > 0) layout.getChannelSet (false, 0) = juce::AudioChannelSet::stereo();
    for (int i = 1; i < layout.inputBuses.size(); ++i) layout.getChannelSet (true, i) = juce::AudioChannelSet::disabled();
    for (int i = 1; i < layout.outputBuses.size(); ++i) layout.getChannelSet (false, i) = juce::AudioChannelSet::disabled();
    p.setBusesLayout (layout);

    if (p.getTotalNumInputChannels() != 2 || p.getTotalNumOutputChannels() != 2)
        return TRANS ("Il plug-in non supporta il layout stereo (e' uno strumento o un effetto mono?).");

    const double rate = engine.getSampleRate();
    p.setRateAndBufferSizeDetails (rate, kBlock);
    p.prepareToPlay (rate, kBlock);
    return {};
}

void PluginHost::instantiate (std::shared_ptr<InsertSlot> slot, std::function<void (juce::String)> done)
{
    auto finish = [this, slot, done] (std::unique_ptr<juce::AudioProcessor> inst, const juce::String& err)
    {
        juce::String problem;
        if (inst == nullptr) problem = err.isNotEmpty() ? err : juce::String (TRANS ("Impossibile caricare il plug-in."));
        else problem = prepareInstance (*inst);

        if (problem.isEmpty())
        {
            if (slot->initialState.isNotEmpty())
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (slot->initialState))
                    inst->setStateInformation (mb.getData(), (int) mb.getSize());
                slot->initialState.clear();
            }
            slot->plugin = std::move (inst);
            slot->ready = true;
        }
        else
            slot->error = problem;

        if (onChanged) onChanged();
        if (done) done (problem);
    };

    if (slot->desc.pluginFormatName == BuiltinFx::formatName)   // effetto integrato: nessun caricamento da file
    {
        finish (BuiltinFx::create (slot->desc), TRANS ("Effetto integrato sconosciuto."));
        return;
    }

    auto flag = alive;
    formats.createPluginInstanceAsync (slot->desc, engine.getSampleRate(), kBlock,
        [flag, finish] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
        {
            if (! *flag) return;
            finish (std::move (inst), err);
        });
}

void PluginHost::append (TrackLive& live, std::shared_ptr<InsertSlot> slot)
{
    auto old = live.getChain();
    auto next = std::make_shared<InsertChain>();
    if (old != nullptr) next->slots = old->slots;
    next->slots.push_back (std::move (slot));
    live.setChain (next);
}

void PluginHost::addFromFile (std::shared_ptr<TrackLive> live, const juce::File& file, std::function<void (juce::String)> done)
{
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (formats.getFormat (i)->fileMightContainThisPluginType (file.getFullPathName()))
            formats.getFormat (i)->findAllTypesForFile (found, file.getFullPathName());

    // solo effetti
    for (int i = found.size(); --i >= 0;)
        if (found[i]->isInstrument) found.remove (i);

    if (found.isEmpty())
    {
        const juce::String msg = TRANS ("Impossibile usare ") + file.getFileNameWithoutExtension() + TRANS (": plug-in non valido, non compatibile o strumento.");
        if (onError) onError (msg);
        if (done) done (msg);
        return;
    }

    auto add = [this, live, done] (juce::PluginDescription desc)
    {
        auto slot = std::make_shared<InsertSlot>();
        slot->desc = desc;
        slot->name = desc.name;
        instantiate (slot, [this, live, slot, done] (juce::String problem)
        {
            if (problem.isEmpty())
            {
                append (*live, slot);
                if (onChanged) onChanged();
            }
            else if (onError) onError (TRANS ("Errore su ") + slot->name + ": " + problem);
            if (done) done (problem);
        });
    };

    if (found.size() == 1) { add (*found[0]); return; }

    juce::PopupMenu menu;
    for (int i = 0; i < found.size(); ++i) menu.addItem (i + 1, found[i]->name);
    std::vector<juce::PluginDescription> descs;
    for (auto* d : found) descs.push_back (*d);
    menu.showMenuAsync (juce::PopupMenu::Options(), [descs, add] (int r)
    {
        if (r > 0 && r <= (int) descs.size()) add (descs[(size_t) r - 1]);
    });
}

void PluginHost::addBuiltin (std::shared_ptr<TrackLive> live, BuiltinFx::Kind kind, std::function<void (juce::String)> done)
{
    auto slot = std::make_shared<InsertSlot>();
    slot->desc = BuiltinFx::describe (kind);
    slot->name = slot->desc.name;
    instantiate (slot, [this, live, slot, done] (juce::String problem)
    {
        if (problem.isEmpty())
        {
            append (*live, slot);
            if (onChanged) onChanged();
        }
        else if (onError) onError (TRANS ("Errore su ") + slot->name + ": " + problem);
        if (done) done (problem);
    });
}

/** Apre il browser: effetti integrati e VST3 in un'unica lista con ricerca. */
void PluginHost::pickAndAdd (std::shared_ptr<TrackLive> live, juce::Component* centreAround)
{
    auto alive_ = std::weak_ptr<bool> (alive);
    PluginBrowser::Callbacks cbs;
    cbs.builtin = [this, live, alive_] (BuiltinFx::Kind k)
    {
        juce::MessageManager::callAsync ([this, live, k, alive_] { if (auto a = alive_.lock()) if (*a) addBuiltin (live, k); });
    };
    cbs.vst3 = [this, live, alive_] (juce::File f)
    {
        juce::MessageManager::callAsync ([this, live, f, alive_] { if (auto a = alive_.lock()) if (*a) addFromFile (live, f); });
    };
    cbs.browse = [this, live, alive_]
    {
        auto chooser = std::make_shared<juce::FileChooser> (TRANS ("Scegli un plug-in VST3"), juce::File(), "*.vst3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
                              [this, live, alive_, chooser] (const juce::FileChooser& fc)
        {
            if (auto a = alive_.lock())
                if (*a && fc.getResult() != juce::File()) addFromFile (live, fc.getResult());
        });
    };

    auto* browser = new PluginBrowser (findInstalledVST3(), cbs);
    browser->setSize (600, 640);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (browser);
    o.dialogTitle = TRANS ("Aggiungi effetto o plug-in");
    o.dialogBackgroundColour = panel;
    o.componentToCentreAround = centreAround;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void PluginHost::removeSlot (TrackLive& live, int index)
{
    auto old = live.getChain();
    if (old == nullptr || ! juce::isPositiveAndBelow (index, (int) old->slots.size())) return;
    auto victim = old->slots[(size_t) index];
    for (int i = windows.size(); --i >= 0;)
        if (windows[i]->slot == victim) windows.remove (i);

    auto next = std::make_shared<InsertChain>();
    next->slots = old->slots;
    next->slots.erase (next->slots.begin() + index);
    live.setChain (next);
    retire (victim);
    if (onChanged) onChanged();
}

void PluginHost::moveSlot (TrackLive& live, int index, int delta)
{
    auto old = live.getChain();
    const int target = index + delta;
    if (old == nullptr || ! juce::isPositiveAndBelow (index, (int) old->slots.size())
        || ! juce::isPositiveAndBelow (target, (int) old->slots.size())) return;
    auto next = std::make_shared<InsertChain>();
    next->slots = old->slots;
    std::swap (next->slots[(size_t) index], next->slots[(size_t) target]);
    live.setChain (next);
    if (onChanged) onChanged();
}

void PluginHost::setBypass (TrackLive& live, int index, bool bypass)
{
    if (auto c = live.getChain())
        if (juce::isPositiveAndBelow (index, (int) c->slots.size()))
        {
            c->slots[(size_t) index]->bypass = bypass;
            if (onChanged) onChanged();
        }
}

void PluginHost::openEditor (std::shared_ptr<InsertSlot> slot, const juce::String& ownerName)
{
    if (slot == nullptr || ! slot->ready.load() || slot->plugin == nullptr) return;
    for (auto* w : windows)
        if (w->slot == slot) { w->toFront (true); return; }

    auto* w = new PluginWindow (slot, slot->name + (ownerName.isNotEmpty() ? "  -  " + ownerName : juce::String()),
                                [this, flag = std::weak_ptr<bool> (alive)] (PluginWindow* closing)
    {
        juce::MessageManager::callAsync ([this, closing, flag]
        {
            if (auto a = flag.lock()) if (*a) windows.removeObject (closing);
        });
    },
                                [this] { if (onChanged) onChanged(); });   // il bypass dell'editor aggiorna anche il LED nel mixer
    windows.add (w);
}

void PluginHost::pruneWindows (const Session& s)
{
    std::set<const InsertSlot*> existing;
    auto collect = [&existing] (const TrackLive& l)
    {
        if (auto c = l.getChain()) for (auto& sl : c->slots) existing.insert (sl.get());
    };
    for (auto& t : s.tracks) collect (*t.live);
    collect (*s.master);
    for (auto& x : s.aux) collect (*x);
    for (auto& x : s.group) collect (*x);
    for (int i = windows.size(); --i >= 0;)
        if (existing.count (windows[i]->slot.get()) == 0) windows.remove (i);
}

void PluginHost::restoreAll (const Session& s)
{
    auto handle = [this] (std::shared_ptr<TrackLive> live)
    {
        if (live->pending.empty()) return;
        auto pend = std::move (live->pending);
        live->pending.clear();

        auto chain = std::make_shared<InsertChain>();
        for (auto& p : pend)
        {
            auto slot = std::make_shared<InsertSlot>();
            slot->initialState = p.stateB64;
            slot->bypass = p.bypass;
            std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (p.descXml));
            if (xml == nullptr || ! slot->desc.loadFromXml (*xml))
            {
                slot->name = "?";
                slot->error = TRANS ("Descrizione del plug-in non valida.");
            }
            else
                slot->name = slot->desc.name;
            chain->slots.push_back (slot);
        }
        live->setChain (chain);
        for (auto& slot : chain->slots)
            if (slot->error.isEmpty())
                instantiate (slot, [this, slot] (juce::String problem)
                {
                    if (problem.isNotEmpty() && onError) onError ("Plug-in " + slot->name + TRANS (" non caricato: ") + problem);
                });
        if (onChanged) onChanged();
    };
    for (auto& t : s.tracks) handle (t.live);
    handle (s.master);
    for (auto& x : s.aux) handle (x);
    for (auto& x : s.group) handle (x);
}

void PluginHost::reprepareAll (const Session& s)
{
    const double rate = engine.getSampleRate();
    auto handle = [rate] (const TrackLive& l)
    {
        if (auto c = l.getChain())
            for (auto& sl : c->slots)
                if (sl->ready.load() && sl->plugin != nullptr)
                {
                    sl->plugin->setRateAndBufferSizeDetails (rate, kBlock);
                    sl->plugin->prepareToPlay (rate, kBlock);
                }
    };
    for (auto& t : s.tracks) handle (*t.live);
    handle (*s.master);
    for (auto& x : s.aux) handle (*x);
    for (auto& x : s.group) handle (*x);
}

} // namespace fc
