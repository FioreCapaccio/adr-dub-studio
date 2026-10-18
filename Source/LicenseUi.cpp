#include "LicenseUi.h"

namespace fc::license
{
LicensePanel::LicensePanel (bool lockedIn, std::function<void()> onChanged, std::function<void()> onQuit, std::function<void()> onTerms, std::function<void()> onContinue)
    : locked (lockedIn), changed (std::move (onChanged)), quit (std::move (onQuit)), proceed (std::move (onContinue))
{
    title.setText ("ADR Dub Studio", juce::dontSendNotification);
    title.setFont (theme::font (20.0f, true));
    title.setColour (juce::Label::textColourId, theme::text);
    status.setFont (theme::font (14.0f, true));
    status.setJustificationType (juce::Justification::topLeft);
    msg.setFont (theme::font (12.5f));
    msg.setColour (juce::Label::textColourId, theme::dim);
    msg.setJustificationType (juce::Justification::topLeft);
    idLabel.setText (TRANS ("Machine ID (da inviare per ottenere la licenza)"), juce::dontSendNotification);
    idLabel.setFont (theme::font (12.0f));
    idLabel.setColour (juce::Label::textColourId, theme::dim);
    idBox.setReadOnly (true);
    idBox.setFont (theme::mono (14.0f));
    idBox.setText (machineId(), false);
    idBox.setSelectAllWhenFocused (true);

    copyBtn.onClick = [] { juce::SystemClipboard::copyTextToClipboard (machineId()); };
    importBtn.onClick = [this] { importFile(); };
    pasteBtn.onClick = [this] { pasteKey(); };
    termsBtn.onClick = std::move (onTerms);
    quitBtn.onClick = [this] { if (quit) quit(); };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &title, &status, &msg, &idLabel, &idBox, &copyBtn, &importBtn, &pasteBtn, &termsBtn })
        addAndMakeVisible (c);
    continueBtn.onClick = [this] { proceeded = true; if (proceed) proceed(); };
    buyBtn.onClick = [] { juce::URL (buyUrl()).launchInDefaultBrowser(); };
    addAndMakeVisible (buyBtn);
    if (locked && kHardLock) addAndMakeVisible (quitBtn);
    if (locked && ! kHardLock)
    {
        addAndMakeVisible (continueBtn);
        waiting = kNagSeconds;
        continueBtn.setEnabled (false);
        continueBtn.setButtonText (TRANS ("Continua") + " (" + juce::String (waiting) + ")");
        startTimer (1000);
    }
    setSize (560, 390);   // lo stato puo' andare su due righe
    refresh();
}

// chiudere la finestra con la X dopo l'attesa vale come premere Continua
LicensePanel::~LicensePanel()
{
    if (locked && ! kHardLock && ! proceeded && waiting <= 0 && proceed) proceed();
}

void LicensePanel::refresh()
{
    const auto s = current();
    status.setText (describe (s), juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, allowed (s) ? theme::green : theme::accent);
    buyBtn.setVisible (s.state != State::Licensed);   // con la licenza attiva non serve
    resized();
    msg.setText (locked && ! kHardLock ? TRANS ("Il programma continua a funzionare con tutte le funzioni: la licenza e' facoltativa. Se ti e' utile, acquistala (49 euro, una tantum) e importa il file .lic che ricevi per email.")
              : locked ? TRANS ("Per continuare serve una licenza. Invia il Machine ID per riceverla, poi importa il file .lic. Le sessioni gia' salvate restano al sicuro: non vengono toccate.")
                        : TRANS ("La licenza e' facoltativa. Per attivarla importa il file .lic ricevuto per email: e' valido solo su questo computer."),
                juce::dontSendNotification);
}

void LicensePanel::resized()
{
    auto r = getLocalBounds().reduced (20);
    title.setBounds (r.removeFromTop (30));
    status.setBounds (r.removeFromTop (44));
    msg.setBounds (r.removeFromTop (50));
    r.removeFromTop (4);
    idLabel.setBounds (r.removeFromTop (18));
    auto row = r.removeFromTop (30);
    copyBtn.setBounds (row.removeFromRight (80));
    row.removeFromRight (6);
    idBox.setBounds (row);
    r.removeFromTop (14);
    if (buyBtn.isVisible()) { buyBtn.setBounds (r.removeFromTop (34)); r.removeFromTop (10); }
    auto b1 = r.removeFromTop (30);
    importBtn.setBounds (b1.removeFromLeft (230));
    b1.removeFromLeft (8);
    pasteBtn.setBounds (b1.removeFromLeft (170));
    r.removeFromTop (10);
    auto b2 = r.removeFromTop (30);
    termsBtn.setBounds (b2.removeFromLeft (170));
    if (locked && kHardLock) quitBtn.setBounds (b2.removeFromRight (100));
    if (locked && ! kHardLock) continueBtn.setBounds (b2.removeFromRight (130));
}

void LicensePanel::paint (juce::Graphics& g) { g.fillAll (theme::panel); }

void LicensePanel::timerCallback()
{
    if (--waiting > 0) { continueBtn.setButtonText (TRANS ("Continua") + " (" + juce::String (waiting) + ")"); return; }
    stopTimer();
    continueBtn.setButtonText (TRANS ("Continua"));
    continueBtn.setEnabled (true);
}

void LicensePanel::importFile()
{
    chooser = std::make_shared<juce::FileChooser> (TRANS ("Scegli il file di licenza"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.lic;*.txt");
    juce::Component::SafePointer<LicensePanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult().existsAsFile()) safe->applyKey (fc.getResult().loadFileAsString());
    });
}

void LicensePanel::pasteKey() { applyKey (juce::SystemClipboard::getTextFromClipboard()); }

void LicensePanel::applyKey (const juce::String& text)
{
    juce::String err;
    if (! importKey (text, err))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Licenza"), err.isNotEmpty() ? err : TRANS ("Licenza non valida"));
        return;
    }
    refresh();
    if (changed) changed();
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Licenza"), TRANS ("Licenza attivata. Grazie!"));
}

EulaPanel::EulaPanel (bool askToAccept, std::function<void (bool)> onAnswer, const juce::String& body) : ask (askToAccept)
{
    text.setMultiLine (true, true);
    text.setReadOnly (true);
    text.setFont (theme::font (13.0f));
    text.setText (body.isNotEmpty() ? body : eulaText (lang::savedCode()), false);
    addAndMakeVisible (text);
    auto answer = std::make_shared<std::function<void (bool)>> (std::move (onAnswer));
    accept.onClick = [answer] { (*answer) (true); };
    reject.onClick = [answer] { (*answer) (false); };
    close.onClick = [answer] { (*answer) (true); };
    if (ask) { addAndMakeVisible (accept); addAndMakeVisible (reject); }
    else addAndMakeVisible (close);
    setSize (640, 520);
}

void EulaPanel::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto row = r.removeFromBottom (32);
    r.removeFromBottom (10);
    text.setBounds (r);
    if (ask) { accept.setBounds (row.removeFromRight (130)); row.removeFromRight (8); reject.setBounds (row.removeFromRight (130)); }
    else close.setBounds (row.removeFromRight (110));
}
} // namespace fc::license
