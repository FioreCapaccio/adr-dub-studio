#pragma once

#include "Model.h"
#include "Theme.h"

namespace fc::pool
{
struct Entry
{
    AssetPtr asset;
    int clips = 0;                 // clip che lo usano
    std::vector<int64_t> clipIds;
};

/** I file audio usati dalla sessione, senza duplicati, nell'ordine in cui compaiono. (Il file congelato di una traccia conta come usato.) */
std::vector<Entry> collect (const Session&);

/** File audio nella cartella (senza sottocartelle) che nessun clip della sessione usa. */
std::vector<juce::File> unusedFiles (const Session&, const juce::File& folder);

/** Scrive l'audio in memoria dell'asset come WAV 24 bit alla frequenza della sessione, in 'folder'. Ritorna il nuovo asset (nullptr + errore). */
AssetPtr convertToWav24 (juce::AudioFormatManager&, const Asset&, double rate, const juce::File& folder, juce::String& error);
} // namespace fc::pool

namespace fc
{
/** Pool dei media: elenco dei file usati dalla sessione con clip, durata, stato; seleziona i clip, mostra nel Finder, ricollega,
    converte e toglie i file non usati dalla cartella dei media. */
class PoolComponent : public juce::Component, private juce::TableListBoxModel, private juce::ChangeListener
{
public:
    PoolComponent (Project&, std::function<juce::File()> mediaFolder, std::function<void()> relink, std::function<void (juce::String)> showMessage);
    ~PoolComponent() override;
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override;
    void refresh();

private:
    int getNumRows() override { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int columnId, int w, int h, bool selected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    std::vector<pool::Entry> selectedEntries() const;

    Project& project;
    std::function<juce::File()> mediaFolder;
    std::function<void()> relink;
    std::function<void (juce::String)> message;
    juce::TableListBox table { {}, this };
    std::vector<pool::Entry> rows;
    juce::Label summary;
    juce::TextButton selectBtn { TRANS ("SELEZIONA I CLIP") }, finderBtn { TRANS ("MOSTRA NEL FINDER") }, relinkBtn { TRANS ("RICOLLEGA...") },
                     convertBtn { TRANS ("CONVERTI IN WAV 24 BIT") }, unusedBtn { TRANS ("FILE NON USATI...") };
};
} // namespace fc
