#pragma once

#include "ExportEngine.h"
#include "Theme.h"

namespace fc
{
/** Finestra di esportazione con preset e opzioni avanzate. */
class ExportDialog : public juce::Component
{
public:
    /** hasLoop: esiste una regione di loop valida (loopStart/loopEnd in campioni). onAccept riceve le opzioni scelte. */
    /** Oltre alla regione di loop si puo' esportare l'intervallo scelto sulla barra sopra il righello (selLabel lo descrive). */
    ExportDialog (bool hasLoop, int64_t loopStart, int64_t loopEnd, bool hasSel, int64_t selStart, int64_t selEnd, const juce::String& selLabel,
                  std::function<void (const ExportOptions&)> onAccept);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void applyPreset (int preset);
    void markCustom();
    void updateEnabled();
    ExportOptions collect() const;
    void save() const;
    void load();
    void close (bool accepted);

    bool hasLoopRegion, hasSelRegion;
    int64_t ls, le, ss, se;
    std::function<void (const ExportOptions&)> accept;

    juce::Label presetLbl, formatLbl, depthLbl, rateLbl, rangeLbl, normLbl, valueLbl, ceilLbl;
    juce::ComboBox preset, format, depth, rate, range, norm;
    juce::TextEditor value, ceiling;
    juce::ToggleButton dither { TRANS ("Dither a 16 bit") }, stems { TRANS ("Un file per traccia (stem, senza bus master)") },
                       excludeGuide { TRANS ("Escludi la traccia Guida (dialogo originale)") },
                       delivery { TRANS ("Pacchetto di consegna: MIX + stem Dialoghi / M&E / Effetti + rapporto") };
    juce::TextButton exportBtn { TRANS ("Esporta...") }, cancelBtn { "Annulla" };
    juce::Label summary;
    bool applying = false;
};

} // namespace fc
