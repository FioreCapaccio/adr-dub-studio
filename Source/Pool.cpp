#include "Pool.h"

namespace fc::pool
{
std::vector<Entry> collect (const Session& s)
{
    std::vector<Entry> out;
    auto add = [&out] (const AssetPtr& a, int64_t clipId)
    {
        if (a == nullptr) return;
        auto it = std::find_if (out.begin(), out.end(), [&a] (const Entry& e) { return e.asset == a; });
        if (it == out.end()) { out.push_back ({ a, 0, {} }); it = out.end() - 1; }
        if (clipId != 0) { ++it->clips; it->clipIds.push_back (clipId); }
    };
    for (auto& t : s.tracks)
    {
        for (auto& c : t.clips) add (c.asset, c.id);
        add (t.frozen, 0);
    }
    return out;
}

std::vector<juce::File> unusedFiles (const Session& s, const juce::File& folder)
{
    std::set<juce::String> used;
    for (auto& e : collect (s)) if (e.asset->file != juce::File()) used.insert (e.asset->file.getFullPathName());
    std::vector<juce::File> out;
    for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "*"))
    {
        const auto ext = f.getFileExtension().toLowerCase();
        if ((ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac") && used.count (f.getFullPathName()) == 0) out.push_back (f);
    }
    return out;
}

AssetPtr convertToWav24 (juce::AudioFormatManager& fm, const Asset& a, double rate, const juce::File& folder, juce::String& error)
{
    if (a.offline || a.audio.getNumSamples() == 0) { error = TRANS ("Il file non e' disponibile."); return nullptr; }
    folder.createDirectory();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (a.name) + "_24bit", ".wav", false);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w;
        if (os != nullptr) w.reset (wav.createWriterFor (os.get(), rate, (unsigned) a.audio.getNumChannels(), 24, {}, 0));
        if (w == nullptr) { error = TRANS ("Impossibile scrivere ") + file.getFullPathName(); return nullptr; }
        os.release();
        w->writeFromAudioSampleBuffer (a.audio, 0, a.audio.getNumSamples());
    }
    return loadAsset (fm, file, rate, error);
}
} // namespace fc::pool

namespace fc
{
using namespace theme;

PoolComponent::PoolComponent (Project& p, std::function<juce::File()> mf, std::function<void()> rl, std::function<void (juce::String)> msg)
    : project (p), mediaFolder (std::move (mf)), relink (std::move (rl)), message (std::move (msg))
{
    auto& h = table.getHeader();
    h.addColumn (TRANS ("Nome"), 1, 220, 80);
    h.addColumn (TRANS ("Clip"), 2, 50, 40);
    h.addColumn (TRANS ("Durata"), 3, 80, 60);
    h.addColumn (TRANS ("Canali"), 4, 60, 50);
    h.addColumn (TRANS ("Stato"), 5, 110, 70);
    h.addColumn (TRANS ("Percorso"), 6, 420, 100);
    table.setMultipleSelectionEnabled (true);
    table.setRowHeight (22);
    addAndMakeVisible (table);
    summary.setColour (juce::Label::textColourId, dim);
    summary.setFont (font (12.0f));
    addAndMakeVisible (summary);
    for (auto* b : { &selectBtn, &finderBtn, &relinkBtn, &convertBtn, &unusedBtn }) { b->setWantsKeyboardFocus (false); addAndMakeVisible (*b); }
    selectBtn.setTooltip (TRANS ("Seleziona nella timeline tutti i clip che usano i file scelti"));
    finderBtn.setTooltip (TRANS ("Mostra il file nel Finder / Esplora risorse"));
    relinkBtn.setTooltip (TRANS ("Cerca in una cartella i file mancanti (File > Ricollega i media)"));
    convertBtn.setTooltip (TRANS ("Scrive una copia WAV a 24 bit alla frequenza della sessione nella cartella dei media e la usa al posto del file scelto (annullabile)"));
    unusedBtn.setTooltip (TRANS ("Elenca i file della cartella dei media che nessun clip usa e permette di metterli nel cestino"));

    selectBtn.onClick = [this]
    {
        project.selection.clear();
        for (auto& e : selectedEntries()) for (auto id : e.clipIds) project.selection.insert (id);
        project.sendChangeMessage();
    };
    finderBtn.onClick = [this] { for (auto& e : selectedEntries()) if (e.asset->file != juce::File()) { e.asset->file.revealToUser(); break; } };
    relinkBtn.onClick = [this] { if (relink) relink(); };
    convertBtn.onClick = [this]
    {
        std::map<const Asset*, AssetPtr> repl;
        juce::String err;
        for (auto& e : selectedEntries())
            if (auto n = pool::convertToWav24 (project.getFormats(), *e.asset, project.getSampleRate(), mediaFolder(), err)) { n->name = e.asset->name; repl[e.asset.get()] = n; }
        if (! repl.empty()) project.replaceAssets (repl);
        if (err.isNotEmpty() && message) message (err);
    };
    unusedBtn.onClick = [this]
    {
        const auto files = pool::unusedFiles (project.getSession(), mediaFolder());
        if (files.empty()) { if (message) message (TRANS ("Nella cartella dei media non ci sono file inutilizzati.")); return; }
        juce::String list;
        for (size_t i = 0; i < files.size() && i < 15; ++i) list << files[i].getFileName() << "\n";
        if (files.size() > 15) list << "... (" << juce::String ((int) files.size() - 15) << ")\n";
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, TRANS ("File non usati"),
                                            juce::String ((int) files.size()) + TRANS (" file nella cartella dei media non sono usati da nessun clip (per esempio take scartati):\n\n") + list + TRANS ("\nMetterli nel cestino?"),
                                            TRANS ("Metti nel cestino"), TRANS ("Annulla"), this,
                                            juce::ModalCallbackFunction::create ([this, files, safe = juce::Component::SafePointer<PoolComponent> (this)] (int r)
                                            {
                                                if (safe == nullptr || r != 1) return;
                                                for (auto& f : files) f.moveToTrash();
                                                refresh();
                                            }));
    };
    project.addChangeListener (this);
    refresh();
    setSize (980, 380);
}

PoolComponent::~PoolComponent() { project.removeChangeListener (this); }

void PoolComponent::refresh()
{
    rows = pool::collect (project.getSession());
    int offline = 0;
    double secs = 0.0;
    for (auto& e : rows) { offline += e.asset->offline ? 1 : 0; secs += (double) e.asset->length() / project.getSampleRate(); }
    summary.setText (juce::String ((int) rows.size()) + TRANS (" file, ") + juce::String (secs / 60.0, 1) + TRANS (" minuti in tutto, ") + juce::String (offline) + TRANS (" mancanti"), juce::dontSendNotification);
    table.updateContent();
    table.repaint();
}

void PoolComponent::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto top = r.removeFromTop (28);
    int x = top.getX();
    auto put = [&] (juce::Component& c, int w) { c.setBounds (x, top.getY(), w, top.getHeight()); x += w + 4; };
    put (selectBtn, 130); put (finderBtn, 140); put (relinkBtn, 100); put (convertBtn, 180); put (unusedBtn, 130);
    r.removeFromTop (4);
    summary.setBounds (r.removeFromBottom (20));
    table.setBounds (r);
}

std::vector<pool::Entry> PoolComponent::selectedEntries() const
{
    std::vector<pool::Entry> out;
    auto sel = table.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i) if (sel[i] < (int) rows.size()) out.push_back (rows[(size_t) sel[i]]);
    return out;
}

void PoolComponent::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll (selected ? blue.withAlpha (0.35f) : (row % 2 == 0 ? lane : laneAlt));
}

void PoolComponent::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    if (row >= (int) rows.size()) return;
    const auto& e = rows[(size_t) row];
    const double secs = (double) e.asset->length() / project.getSampleRate();
    juce::String t;
    juce::Colour c = text;
    switch (col)
    {
        case 1: t = e.asset->name; break;
        case 2: t = juce::String (e.clips); break;
        case 3: t = juce::String ((int) secs / 60) + ":" + juce::String (secs - 60.0 * ((int) secs / 60), 2).paddedLeft ('0', 5); break;
        case 4: t = juce::String (e.asset->numChannels()); break;
        case 5: t = e.asset->offline ? TRANS ("MANCANTE") : (e.asset->file == juce::File() ? TRANS ("in memoria") : TRANS ("ok")); if (e.asset->offline) c = accent; break;
        case 6: t = e.asset->file.getFullPathName(); c = dim; break;
        default: break;
    }
    g.setColour (c);
    g.setFont (font (12.0f));
    g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}
} // namespace fc
