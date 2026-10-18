#pragma once

#include "License.h"
#include "Theme.h"

namespace fc::license
{
/** Finestra Licenza: stato, Machine ID da copiare, importazione del file .lic. Bloccata = prova scaduta/licenza non valida: l'unica uscita e' importare o chiudere l'app. */
class LicensePanel : public juce::Component
{
public:
    /** onChanged: chiamato dopo un'importazione riuscita. onQuit: solo se bloccata (pulsante Esci). */
    LicensePanel (bool locked, std::function<void()> onChanged, std::function<void()> onQuit, std::function<void()> onTerms);

    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void importFile();
    void pasteKey();
    void applyKey (const juce::String& text);

    bool locked;
    std::function<void()> changed, quit;
    juce::Label title, status, idLabel, msg;
    juce::TextEditor idBox;
    juce::TextButton copyBtn { TRANS ("Copia") }, importBtn { TRANS ("Importa licenza (.lic)...") }, pasteBtn { TRANS ("Incolla la chiave") },
                     termsBtn { TRANS ("Condizioni d'uso...") }, quitBtn { TRANS ("Esci") };
    std::shared_ptr<juce::FileChooser> chooser;
};

/** Condizioni d'uso al primo avvio: Accetta / Rifiuta (rifiutare chiude l'app). */
class EulaPanel : public juce::Component
{
public:
    EulaPanel (bool askToAccept, std::function<void (bool)> onAnswer);
    void resized() override;

private:
    juce::TextEditor text;
    juce::TextButton accept { TRANS ("Accetto") }, reject { TRANS ("Non accetto") }, close { TRANS ("Chiudi") };
    bool ask;
};
} // namespace fc::license
