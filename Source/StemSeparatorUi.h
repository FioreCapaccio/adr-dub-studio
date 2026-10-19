#pragma once

#include "Model.h"
#include "StemEngine.h"
#include "Theme.h"

namespace fc::stems
{
/** Cio' che serve alla separazione di un elenco di clip. */
struct Request
{
    struct ClipSrc { int64_t clipId = 0; juce::String name; AssetPtr asset; int64_t offset = 0, length = 0; };
    std::vector<ClipSrc> clips;
    double rate = 48000.0;
    bool highQuality = false, mergeME = false, muteSources = true, splitVoices = false;
    juce::File folder;
    juce::AudioFormatManager* formats = nullptr;
};

struct Outcome
{
    bool ok = false, cancelled = false;
    juce::String message;
    std::vector<Project::StemClipSet> sets;
};

/** Il lavoro vero (senza interfaccia: lo usa anche il collaudo). setProgress ritorna false per annullare. */
Outcome process (const Request& req, const std::function<bool (double)>& setProgress, const std::function<void (const juce::String&)>& setStatusMessage, const std::function<bool()>& shouldExit);

/** Lavoro in un thread con barra di avanzamento e pulsante Annulla: scarica il motore se manca, separa ogni clip e carica i file prodotti. */
class Job : public juce::ThreadWithProgressWindow
{
public:
    Job (Request r, std::function<void (Outcome)> done);
    void run() override;
    void threadComplete (bool) override { if (done) done (outcome); }

private:
    Request req;
    std::function<void (Outcome)> done;
    Outcome outcome;
};

/** Finestra delle opzioni: riepilogo, 3 o 2 stem, qualita', silenziare gli originali. */
class OptionsPanel : public juce::Component
{
public:
    struct Choice { bool mergeME = false, highQuality = false, muteSources = true, splitVoices = false; };
    OptionsPanel (int numClips, double totalSeconds, bool needsDownload, int downloadMB, std::function<void (Choice)> onStart, std::function<void()> onCancel);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::Label title, info, note;
    juce::ToggleButton three { TRANS ("Tre stem: Dialoghi, Musica, Effetti") }, two { TRANS ("Due stem: Dialoghi, M&E (musica ed effetti insieme)") },
                       hq { TRANS ("Qualita' alta (piu' lenta: sovrapposizione maggiore)") }, mute { TRANS ("Silenzia i clip originali") },
                       voices { TRANS ("Dividi i dialoghi in voce maschile e voce femminile (dal tono)") };
    juce::TextButton go { TRANS ("Separa") }, cancel { TRANS ("Annulla") };
    std::function<void (Choice)> start;
    std::function<void()> cancelFn;
};
} // namespace fc::stems
