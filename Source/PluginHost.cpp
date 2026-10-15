#include "PluginHost.h"

namespace fc
{
using namespace theme;

//==============================================================================
PluginWindow::PluginWindow (std::shared_ptr<InsertSlot> s, const juce::String& title, std::function<void (PluginWindow*)> onClose)
    : juce::DocumentWindow (title, bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      slot (std::move (s)), closeCallback (std::move (onClose))
{
    setUsingNativeTitleBar (true);

    auto& p = *slot->plugin;
    juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
    if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (p);

    setContentOwned (editor, true);
    setResizable (editor->isResizable(), false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

//==============================================================================
PluginPicker::PluginPicker (juce::Array<juce::File> files, std::function<void (juce::File)> onPick)
    : all (std::move (files)), pick (std::move (onPick))
{
    search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Cerca (es. EQ, reverb, limiter...)"), dim);
    search.onTextChange = [this] { applyFilter(); };
    search.onReturnKey = [this] { choose (list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0); };
    search.onEscapeKey = [this] { closeDialog(); };
    addAndMakeVisible (search);

    list.setRowHeight (30);
    list.setColour (juce::ListBox::outlineColourId, border);
    list.setColour (juce::ListBox::backgroundColourId, bg);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    addButton.onClick = [this] { choose (list.getSelectedRow()); };
    cancelButton.onClick = [this] { closeDialog(); };
    addAndMakeVisible (addButton);
    addAndMakeVisible (cancelButton);

    applyFilter();
    if (shown.size() > 0) list.selectRow (0);
}

void PluginPicker::applyFilter()
{
    shown.clear();
    const auto terms = juce::StringArray::fromTokens (search.getText().trim(), " ", "");
    for (const auto& f : all)
    {
        const auto name = f.getFileNameWithoutExtension();
        bool ok = true;
        for (const auto& t : terms) ok = ok && name.containsIgnoreCase (t);
        if (ok) shown.add (f);
    }
    list.updateContent();
    list.repaint();
    if (shown.size() > 0) list.selectRow (0);
}

void PluginPicker::choose (int row)
{
    if (! juce::isPositiveAndBelow (row, shown.size())) return;
    auto file = shown[row];
    auto callback = pick;
    closeDialog();
    if (callback) callback (file);
}

void PluginPicker::closeDialog()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
}

void PluginPicker::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, shown.size())) return;
    if (selected) g.fillAll (blue.withAlpha (0.35f));
    g.setColour (text);
    g.setFont (font (14.0f));
    g.drawText (shown[row].getFileNameWithoutExtension(), 12, 0, w - 24, h, juce::Justification::centredLeft);
}

void PluginPicker::paint (juce::Graphics& g) { g.fillAll (panel); }

void PluginPicker::resized()
{
    auto r = getLocalBounds().reduced (14);
    search.setBounds (r.removeFromTop (32));
    r.removeFromTop (10);
    auto bottom = r.removeFromBottom (36);
    r.removeFromBottom (10);
    list.setBounds (r);
    cancelButton.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (8);
    addButton.setBounds (bottom.removeFromRight (110));
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
    scan (juce::File ("/Library/Audio/Plug-Ins/VST3"), 0);
    scan (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3"), 0);

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
        return "Il plug-in non supporta il layout stereo (e' uno strumento o un effetto mono?).";

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
        if (inst == nullptr) problem = err.isNotEmpty() ? err : juce::String ("Impossibile caricare il plug-in.");
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
        finish (BuiltinFx::create (slot->desc), "Effetto integrato sconosciuto.");
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
        const juce::String msg = "Impossibile usare " + file.getFileNameWithoutExtension() + ": plug-in non valido, non compatibile o strumento.";
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
            else if (onError) onError ("Errore su " + slot->name + ": " + problem);
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
        else if (onError) onError ("Errore su " + slot->name + ": " + problem);
        if (done) done (problem);
    });
}

/** Menu degli insert: effetti integrati, poi i VST3 installati. */
void PluginHost::pickAndAdd (std::shared_ptr<TrackLive> live, juce::Component* centreAround)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Effetti integrati");
    for (int k = 0; k < BuiltinFx::numKinds; ++k)
        m.addItem (k + 1, BuiltinFx::kindName ((BuiltinFx::Kind) k) + "   -   " + BuiltinFx::kindHelp ((BuiltinFx::Kind) k));
    m.addSeparator();
    m.addItem (100, "Plug-in VST3 installati...");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (centreAround),
                     [this, live, centreAround, flag = std::weak_ptr<bool> (alive)] (int r)
    {
        auto a = flag.lock();
        if (r == 0 || a == nullptr || ! *a) return;
        if (r == 100) openPicker (live, centreAround);
        else addBuiltin (live, (BuiltinFx::Kind) (r - 1));
    });
}

void PluginHost::openPicker (std::shared_ptr<TrackLive> live, juce::Component* centreAround)
{
    auto* picker = new PluginPicker (findInstalledVST3(), [this, live, flag = std::weak_ptr<bool> (alive)] (juce::File f)
    {
        juce::MessageManager::callAsync ([this, live, f, flag]
        {
            if (auto a = flag.lock()) if (*a) addFromFile (live, f);
        });
    });
    picker->setSize (520, 580);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (picker);
    o.dialogTitle = "Aggiungi plug-in VST3";
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
    });
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
                slot->error = "Descrizione del plug-in non valida.";
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
                    if (problem.isNotEmpty() && onError) onError ("Plug-in " + slot->name + " non caricato: " + problem);
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
