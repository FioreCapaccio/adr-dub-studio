#include "SecondCard.h"
#include "Settings.h"

namespace fc
{
using namespace theme;

SecondCard::SecondCard()
{
    cueFifo.prepare (2);
    micFifo.prepare (1);
}

SecondCard::~SecondCard() { shutdown(); }

void SecondCard::shutdown()
{
    if (! initialised) return;
    manager.removeChangeListener (this);
    manager.removeAudioCallback (this);
    manager.closeAudioDevice();
    initialised = false;
}

void SecondCard::loadSettings()
{
    auto& s = settings();
    outOn = s.getBoolValue ("card2.out", false);
    inOn = s.getBoolValue ("card2.in", false);
    outPair = juce::jmax (0, s.getIntValue ("card2.pair", 0));
    micChannel = juce::jmax (0, s.getIntValue ("card2.mic", 0));
}

void SecondCard::saveSettings() const
{
    auto& s = settings();
    s.setValue ("card2.out", outOn.load());
    s.setValue ("card2.in", inOn.load());
    s.setValue ("card2.pair", outPair.load());
    s.setValue ("card2.mic", micChannel.load());
    if (initialised)
        if (auto xml = manager.createStateXml()) s.setValue ("card2.xml", xml->toString());
    s.saveIfNeeded();
}

juce::String SecondCard::ensureOpen()
{
    if (! initialised)
    {
        std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (settings().getValue ("card2.xml", "")));
        // senza stato salvato non si apre nessuna scheda: initialise() da solo aprirebbe quelle predefinite (cioe' quelle della scheda principale, microfono compreso)
        if (xml == nullptr) xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        manager.initialise (2, 2, xml.get(), false);
        manager.addAudioCallback (this);
        manager.addChangeListener (this);
        initialised = true;
    }
    return isOpen() ? juce::String() : TRANS ("Scegli la seconda scheda audio in Impostazioni > Scheda del direttore.");
}

void SecondCard::changeListenerCallback (juce::ChangeBroadcaster*) { saveSettings(); }

void SecondCard::setMainFormat (double rate, int blockSize)
{
    mainRate = rate; mainBlock = blockSize;
    updateTargets();
}

void SecondCard::updateTargets()
{
    // un po' piu' di due blocchi del piu' grande: abbastanza per non restare senza dati, abbastanza poco da non sentire il ritardo
    const int t = juce::jmax (512, 2 * juce::jmax (mainBlock.load(), cardBlock.load()) + 128);
    cueFifo.setTarget (t);
    micFifo.setTarget (t);
}

void SecondCard::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    cardRate = d->getCurrentSampleRate();
    cardBlock = d->getCurrentBufferSizeSamples();
    updateTargets();
    cueFifo.reset(); micFifo.reset();
    cueFifo.clearCounters(); micFifo.clearCounters();
}

void SecondCard::audioDeviceStopped() {}

void SecondCard::pushCue (const float* l, const float* r, int n) noexcept
{
    const float* ch[2] = { l, r };
    cueFifo.write (ch, n);
}

void SecondCard::pullMic (float* dest, int n) noexcept
{
    float* o[1] = { dest };
    micFifo.read (o, n, cardRate.load() / juce::jmax (1.0, mainRate.load()));
}

void SecondCard::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n, const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);
    if (inOn.load())
    {
        const int ch = micChannel.load();
        if (ch < numIn && in[ch] != nullptr) { const float* src[1] = { in[ch] }; micFifo.write (src, n); }
    }
    if (outOn.load())
    {
        const int first = 2 * outPair.load();
        if (first + 1 < numOut && out[first] != nullptr && out[first + 1] != nullptr)
        {
            float* dst[2] = { out[first], out[first + 1] };
            cueFifo.read (dst, n, mainRate.load() / juce::jmax (1.0, cardRate.load()));
        }
    }
}

int SecondCard::numActiveOutputs() const
{
    auto* d = manager.getCurrentAudioDevice();
    return d != nullptr ? d->getActiveOutputChannels().countNumberOfSetBits() : 0;
}

int SecondCard::numActiveInputs() const
{
    auto* d = manager.getCurrentAudioDevice();
    return d != nullptr ? d->getActiveInputChannels().countNumberOfSetBits() : 0;
}

juce::String SecondCard::statusText() const
{
    auto* d = manager.getCurrentAudioDevice();
    if (d == nullptr) return TRANS ("Nessuna seconda scheda aperta.");
    juce::String s = d->getName() + ", " + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz, " + juce::String (d->getCurrentBufferSizeSamples()) + " campioni";
    if (std::abs (d->getCurrentSampleRate() - mainRate.load()) > 1.0) s += " (" + TRANS ("frequenza diversa dalla principale: ricampionata") + ")";
    const double ms = 1000.0 * (double) cueFifo.getTarget() / juce::jmax (1.0, mainRate.load());
    s += "\n" + TRANS ("Ritardo aggiunto dal collegamento fra le due schede: circa ") + juce::String (ms, 0) + " ms";
    const int bad = cueFifo.getUnderruns() + cueFifo.getOverruns() + micFifo.getUnderruns() + micFifo.getOverruns();
    s += "\n" + (bad == 0 ? TRANS ("Le due schede sono agganciate.") : TRANS ("Interruzioni dall'apertura: ") + juce::String (bad));
    return s;
}

//==============================================================================
SecondCardPanel::SecondCardPanel (SecondCard& c) : card (c)
{
    card.loadSettings();
    card.ensureOpen();
    selector = std::make_unique<juce::AudioDeviceSelectorComponent> (card.manager, 0, 8, 0, 8, false, false, true, false);
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (pairLbl, TRANS ("USCITE PER LE CUFFIE")); label (micLbl, TRANS ("INGRESSO DEL MICROFONO"));
    hint.setFont (font (12.0f));
    hint.setColour (juce::Label::textColourId, dim);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setText (TRANS ("Scegli qui la seconda scheda e quali canali usare. Le cuffie dell'attore (mix, bip, talkback) e il microfono del direttore possono stare su questa scheda "
                         "invece che su quella principale: si accendono poi da ADR > Cuffie dell'attore e talkback."), juce::dontSendNotification);
    status.setFont (font (12.0f));
    status.setJustificationType (juce::Justification::topLeft);
    for (auto* c2 : std::initializer_list<juce::Component*> { &outBtn, &inBtn, &pair, &mic, &hint, &status, selector.get() }) addAndMakeVisible (*c2);
    outBtn.setToggleState (card.outOn.load(), juce::dontSendNotification);
    inBtn.setToggleState (card.inOn.load(), juce::dontSendNotification);
    refreshChannels();
    loading = false;
    outBtn.onClick = [this] { apply(); }; inBtn.onClick = [this] { apply(); };
    pair.onChange = [this] { apply(); }; mic.onChange = [this] { apply(); };
    card.manager.addChangeListener (this);
    startTimerHz (3);
    timerCallback();
}

SecondCardPanel::~SecondCardPanel()
{
    card.manager.removeChangeListener (this);
    stopTimer();
    if (! card.outOn.load() && ! card.inOn.load()) card.manager.closeAudioDevice();   // la scheda scelta resta salvata, ma non resta aperta (e il microfono acceso) se non serve
}

void SecondCardPanel::changeListenerCallback (juce::ChangeBroadcaster*) { refreshChannels(); }

void SecondCardPanel::refreshChannels()
{
    const bool was = loading;
    loading = true;
    pair.clear (juce::dontSendNotification); mic.clear (juce::dontSendNotification);
    for (int p = 0; 2 * p + 1 < card.numActiveOutputs(); ++p)
        pair.addItem (TRANS ("Uscite attive ") + juce::String (2 * p + 1) + " - " + juce::String (2 * p + 2), p + 1);
    for (int i = 0; i < card.numActiveInputs(); ++i) mic.addItem (TRANS ("Ingresso attivo ") + juce::String (i + 1), i + 1);
    if (pair.getNumItems() > 0) pair.setSelectedId (juce::jmin (card.outPair.load(), pair.getNumItems() - 1) + 1, juce::dontSendNotification);
    if (mic.getNumItems() > 0) mic.setSelectedId (juce::jmin (card.micChannel.load(), mic.getNumItems() - 1) + 1, juce::dontSendNotification);
    pair.setEnabled (pair.getNumItems() > 0); mic.setEnabled (mic.getNumItems() > 0);
    loading = was;
}

void SecondCardPanel::apply()
{
    if (loading) return;
    card.outOn = outBtn.getToggleState();
    card.inOn = inBtn.getToggleState();
    card.outPair = juce::jmax (0, pair.getSelectedId() - 1);
    card.micChannel = juce::jmax (0, mic.getSelectedId() - 1);
    card.cueFifo.reset(); card.micFifo.reset();
    card.saveSettings();
}

void SecondCardPanel::timerCallback()
{
    status.setText (card.statusText(), juce::dontSendNotification);
}

void SecondCardPanel::resized()
{
    auto all = getLocalBounds().reduced (16);
    auto r = all.removeFromLeft (290);   // a sinistra cosa fa la seconda scheda, a destra la scelta della scheda
    all.removeFromLeft (14);
    hint.setBounds (r.removeFromTop (92));
    r.removeFromTop (8);
    outBtn.setBounds (r.removeFromTop (24));
    inBtn.setBounds (r.removeFromTop (24));
    r.removeFromTop (6);
    pairLbl.setBounds (r.removeFromTop (16)); pair.setBounds (r.removeFromTop (26)); r.removeFromTop (6);
    micLbl.setBounds (r.removeFromTop (16)); mic.setBounds (r.removeFromTop (26)); r.removeFromTop (8);
    status.setBounds (r.removeFromTop (110));
    selector->setBounds (all);
}
} // namespace fc
