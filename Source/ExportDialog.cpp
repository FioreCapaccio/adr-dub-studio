#include "ExportDialog.h"
#include "Settings.h"

namespace fc
{
using namespace theme;

namespace
{
struct Preset { const char* name; const char* fmt; int depth; double rate; int norm; double value; double ceiling; bool dither; bool stems; bool delivery = false; };
// norm: 0 nessuna, 1 picco, 2 LUFS
const Preset presets[] = {
    { TRX ("Master (WAV 24 bit)"),                    "wav", 24, 0.0,     0, 0.0,   -1.0, false, false },
    { TRX ("Broadcast EBU R128 (-23 LUFS)"),          "wav", 24, 48000.0, 2, -23.0, -1.0, false, false },
    { TRX ("Streaming e social (-14 LUFS)"),          "wav", 24, 0.0,     2, -14.0, -1.0, false, false },
    { TRX ("Podcast (-16 LUFS, WAV 16 bit)"),         "wav", 16, 44100.0, 2, -16.0, -1.0, true,  false },
    { TRX ("CD (16 bit, 44,1 kHz, picco -0,3 dB)"),   "wav", 16, 44100.0, 1, -0.3,  -1.0, true,  false },
    { TRX ("Stem (un file per traccia)"),             "wav", 24, 0.0,     0, 0.0,   -1.0, false, true  },
    { TRX ("Consegna doppiaggio: MIX + stem DIA/ME/FX (-23 LUFS)"), "wav", 24, 48000.0, 2, -23.0, -1.0, false, false, true },
    { TRX ("TV USA, ATSC A/85 (-24 LUFS, -2 dBTP)"),  "wav", 24, 48000.0, 2, -24.0, -2.0, false, false },
    { TRX ("Streaming, dialoghi (-27 LUFS, -2 dBTP): verifica le specifiche"), "wav", 24, 48000.0, 2, -27.0, -2.0, false, false },
    { TRX ("Personalizzato"),                         "wav", 24, 0.0,     0, 0.0,   -1.0, true,  false }
};
constexpr int numPresets = (int) (sizeof (presets) / sizeof (presets[0]));
constexpr int customPreset = numPresets - 1;
} // namespace

ExportDialog::ExportDialog (bool loop, int64_t loopStart, int64_t loopEnd, bool sel, int64_t selStart, int64_t selEnd, const juce::String& selLabel,
                            std::function<void (const ExportOptions&)> onAccept)
    : hasLoopRegion (loop), hasSelRegion (sel), ls (loopStart), le (loopEnd), ss (selStart), se (selEnd), accept (std::move (onAccept))
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (presetLbl, TRANS ("PRESET")); label (formatLbl, TRANS ("FORMATO")); label (depthLbl, TRANS ("PROFONDITA'")); label (rateLbl, TRANS ("FREQUENZA"));
    label (rangeLbl, TRANS ("INTERVALLO")); label (normLbl, TRANS ("NORMALIZZAZIONE")); label (valueLbl, TRANS ("VALORE")); label (ceilLbl, TRANS ("TETTO TRUE-PEAK"));

    for (int i = 0; i < numPresets; ++i) preset.addItem (juce::translate (presets[i].name), i + 1);
    format.addItem (TRANS ("WAV"), 1); format.addItem (TRANS ("AIFF"), 2); format.addItem (TRANS ("FLAC"), 3);
    depth.addItem (TRANS ("16 bit"), 1); depth.addItem (TRANS ("24 bit"), 2); depth.addItem (TRANS ("32 bit"), 3);
    rate.addItem (TRANS ("Come la sessione"), 1); rate.addItem (TRANS ("44,1 kHz"), 2); rate.addItem (TRANS ("48 kHz"), 3); rate.addItem (TRANS ("88,2 kHz"), 4); rate.addItem (TRANS ("96 kHz"), 5);
    range.addItem (TRANS ("Tutta la sessione"), 1); range.addItem (TRANS ("Regione di loop"), 2);
    range.addItem (hasSelRegion ? TRANS ("Intervallo scelto: ") + selLabel : juce::String (TRANS ("Intervallo scelto (barra sopra il righello)")), 3);
    range.setItemEnabled (2, hasLoopRegion);
    range.setItemEnabled (3, hasSelRegion);
    norm.addItem (TRANS ("Nessuna"), 1); norm.addItem (TRANS ("Picco a..."), 2); norm.addItem (TRANS ("Loudness a..."), 3);

    for (auto* c : { &preset, &format, &depth, &rate, &range, &norm }) addAndMakeVisible (*c);
    for (auto* t : { &value, &ceiling })
    {
        t->setInputRestrictions (8, "-0123456789.,");
        t->setJustification (juce::Justification::centred);
        addAndMakeVisible (*t);
    }
    addAndMakeVisible (dither);
    addAndMakeVisible (stems);
    addAndMakeVisible (excludeGuide);
    addAndMakeVisible (delivery);
    excludeGuide.setToggleState (true, juce::dontSendNotification);

    summary.setFont (font (11.5f));
    summary.setColour (juce::Label::textColourId, dim);
    summary.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (summary);
    addAndMakeVisible (exportBtn);
    addAndMakeVisible (cancelBtn);

    preset.onChange = [this] { if (! applying) applyPreset (preset.getSelectedId() - 1); };
    for (auto* c : { &format, &depth, &rate, &range, &norm }) c->onChange = [this] { if (! applying) markCustom(); updateEnabled(); };
    value.onTextChange = [this] { if (! applying) markCustom(); updateEnabled(); };
    ceiling.onTextChange = [this] { if (! applying) markCustom(); };
    dither.onClick = [this] { if (! applying) markCustom(); };
    stems.onClick = [this] { if (stems.getToggleState()) delivery.setToggleState (false, juce::dontSendNotification); if (! applying) markCustom(); updateEnabled(); };
    delivery.onClick = [this] { if (delivery.getToggleState()) stems.setToggleState (false, juce::dontSendNotification); if (! applying) markCustom(); updateEnabled(); };
    excludeGuide.onClick = [this] { if (! applying) markCustom(); };
    exportBtn.onClick = [this] { close (true); };
    cancelBtn.onClick = [this] { close (false); };

    load();
    updateEnabled();
}

void ExportDialog::applyPreset (int idx)
{
    if (idx < 0 || idx >= numPresets || idx == customPreset) return;
    const auto& p = presets[idx];
    applying = true;
    format.setSelectedId (juce::String (p.fmt) == "aiff" ? 2 : (juce::String (p.fmt) == "flac" ? 3 : 1), juce::dontSendNotification);
    depth.setSelectedId (p.depth == 16 ? 1 : (p.depth == 24 ? 2 : 3), juce::dontSendNotification);
    rate.setSelectedId (p.rate == 0.0 ? 1 : (p.rate == 44100.0 ? 2 : (p.rate == 48000.0 ? 3 : (p.rate == 88200.0 ? 4 : 5))), juce::dontSendNotification);
    norm.setSelectedId (p.norm + 1, juce::dontSendNotification);
    value.setText (juce::String (p.value, 1), juce::dontSendNotification);
    ceiling.setText (juce::String (p.ceiling, 1), juce::dontSendNotification);
    dither.setToggleState (p.dither, juce::dontSendNotification);
    stems.setToggleState (p.stems, juce::dontSendNotification);
    delivery.setToggleState (p.delivery, juce::dontSendNotification);
    applying = false;
    updateEnabled();
}

void ExportDialog::markCustom()
{
    applying = true;
    preset.setSelectedId (customPreset + 1, juce::dontSendNotification);
    applying = false;
}

void ExportDialog::updateEnabled()
{
    const int n = norm.getSelectedId() - 1;
    const bool isStem = stems.getToggleState(), isDelivery = delivery.getToggleState();
    norm.setEnabled (! isStem);
    value.setEnabled (n > 0 && ! isStem);
    ceiling.setEnabled (n == 2 && ! isStem);
    range.setEnabled (true);
    valueLbl.setText (n == 1 ? TRANS ("PICCO (dBFS)") : (n == 2 ? "OBIETTIVO (LUFS)" : "VALORE"), juce::dontSendNotification);
    // FLAC non supporta 32 bit
    depth.setItemEnabled (3, format.getSelectedId() != 3);
    if (format.getSelectedId() == 3 && depth.getSelectedId() == 3) depth.setSelectedId (2, juce::dontSendNotification);

    juce::String s;
    if (isDelivery) s = TRANS ("In una sola esportazione: il MIX (con il bus master) e uno stem per ogni bus di ruolo (Dialoghi, M&E, Effetti), tutti con lo stesso guadagno, piu' un rapporto HTML con picco, true-peak e loudness di ogni file. Scegli la cartella di destinazione.");
    else if (isStem) s = TRANS ("Ogni traccia con clip viene esportata in un file separato, dopo fader e inserti ma senza limiter e plug-in del master. "
                    "I file sono allineati tra loro.");
    else if (n == 2) s = "Il programma viene misurato e riportato all'obiettivo. Se questo fa superare il tetto di true-peak interviene un limiter trasparente.";
    else if (n == 1) s = "Il guadagno viene regolato in modo che il picco massimo arrivi al valore indicato.";
    else s = TRANS ("Nessuna modifica di livello: il file e' il mix cosi' come lo senti.");
    if (depth.getSelectedId() == 1) s << " A 16 bit viene aggiunto un leggero dither per evitare la distorsione di quantizzazione.";
    summary.setText (s, juce::dontSendNotification);
    exportBtn.setButtonText (isStem || isDelivery ? TRANS ("Scegli cartella...") : TRANS ("Esporta..."));
}

ExportOptions ExportDialog::collect() const
{
    ExportOptions o;
    o.format = format.getSelectedId() == 2 ? "aiff" : (format.getSelectedId() == 3 ? "flac" : "wav");
    o.bitDepth = depth.getSelectedId() == 1 ? 16 : (depth.getSelectedId() == 2 ? 24 : 32);
    static const double rates[] = { 0.0, 44100.0, 48000.0, 88200.0, 96000.0 };
    o.outRate = rates[juce::jlimit (0, 4, rate.getSelectedId() - 1)];
    const bool useSel = range.getSelectedId() == 3 && hasSelRegion;
    o.loopRange = useSel || (range.getSelectedId() == 2 && hasLoopRegion);
    o.rangeStart = useSel ? ss : ls;
    o.rangeEnd = useSel ? se : le;
    const int n = norm.getSelectedId() - 1;
    o.norm = n == 1 ? ExportOptions::Norm::Peak : (n == 2 ? ExportOptions::Norm::Lufs : ExportOptions::Norm::None);
    o.normTarget = value.getText().replaceCharacter (',', '.').getDoubleValue();
    o.ceilingDb = ceiling.getText().replaceCharacter (',', '.').getDoubleValue();
    if (o.norm == ExportOptions::Norm::Peak) o.normTarget = juce::jmin (o.normTarget, 0.0);
    if (o.norm == ExportOptions::Norm::Lufs) o.normTarget = juce::jlimit (-40.0, 0.0, o.normTarget);
    o.ceilingDb = juce::jlimit (-12.0, -0.1, o.ceilingDb);
    o.dither = dither.getToggleState();
    o.stems = stems.getToggleState();
    o.delivery = delivery.getToggleState();
    o.excludeGuide = excludeGuide.getToggleState();
    if (preset.getSelectedId() - 1 != customPreset) o.presetName = preset.getText();
    return o;
}

void ExportDialog::save() const
{
    auto& s = settings();
    s.setValue ("export.preset", preset.getSelectedId());
    s.setValue ("export.format", format.getSelectedId());
    s.setValue ("export.depth", depth.getSelectedId());
    s.setValue ("export.rate", rate.getSelectedId());
    s.setValue ("export.norm", norm.getSelectedId());
    s.setValue ("export.value", value.getText());
    s.setValue ("export.ceiling", ceiling.getText());
    s.setValue ("export.dither", dither.getToggleState());
    s.setValue ("export.stems", stems.getToggleState());
    s.setValue ("export.delivery", delivery.getToggleState());
    s.setValue ("export.excludeGuide", excludeGuide.getToggleState());
    s.saveIfNeeded();
}

void ExportDialog::load()
{
    auto& s = settings();
    applying = true;
    if (! s.containsKey ("export.preset"))
    {
        preset.setSelectedId (1, juce::dontSendNotification);
        applying = false;
        applyPreset (0);
        range.setSelectedId (hasSelRegion ? 3 : 1, juce::dontSendNotification);
        return;
    }
    preset.setSelectedId (s.getIntValue ("export.preset", 1), juce::dontSendNotification);
    format.setSelectedId (s.getIntValue ("export.format", 1), juce::dontSendNotification);
    depth.setSelectedId (s.getIntValue ("export.depth", 2), juce::dontSendNotification);
    rate.setSelectedId (s.getIntValue ("export.rate", 1), juce::dontSendNotification);
    norm.setSelectedId (s.getIntValue ("export.norm", 1), juce::dontSendNotification);
    value.setText (s.getValue ("export.value", "-1.0"), juce::dontSendNotification);
    ceiling.setText (s.getValue ("export.ceiling", "-1.0"), juce::dontSendNotification);
    dither.setToggleState (s.getBoolValue ("export.dither", true), juce::dontSendNotification);
    stems.setToggleState (s.getBoolValue ("export.stems", false), juce::dontSendNotification);
    delivery.setToggleState (s.getBoolValue ("export.delivery", false), juce::dontSendNotification);
    excludeGuide.setToggleState (s.getBoolValue ("export.excludeGuide", true), juce::dontSendNotification);
    range.setSelectedId (hasSelRegion ? 3 : 1, juce::dontSendNotification);
    applying = false;
}

void ExportDialog::close (bool accepted)
{
    if (accepted) save();
    const auto o = collect();
    auto cb = accept;
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (accepted ? 1 : 0);
    if (accepted && cb) juce::MessageManager::callAsync ([cb, o] { cb (o); });
}

void ExportDialog::paint (juce::Graphics& g) { g.fillAll (panel); }

void ExportDialog::resized()
{
    auto r = getLocalBounds().reduced (18);
    auto row = [&r] (juce::Label& l, juce::Component& c, int h = 28)
    {
        auto line = r.removeFromTop (h);
        l.setBounds (line.removeFromLeft (150));
        c.setBounds (line);
        r.removeFromTop (8);
    };
    row (presetLbl, preset);
    r.removeFromTop (6);
    row (formatLbl, format);
    row (depthLbl, depth);
    row (rateLbl, rate);
    row (rangeLbl, range);
    row (normLbl, norm);
    row (valueLbl, value);
    row (ceilLbl, ceiling);
    dither.setBounds (r.removeFromTop (24));
    stems.setBounds (r.removeFromTop (24));
    delivery.setBounds (r.removeFromTop (24));
    excludeGuide.setBounds (r.removeFromTop (24));
    r.removeFromTop (6);
    auto bottom = r.removeFromBottom (34);
    summary.setBounds (r);
    cancelBtn.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (8);
    exportBtn.setBounds (bottom.removeFromRight (170));
}

} // namespace fc
