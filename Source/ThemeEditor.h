#pragma once

#include "ThemeManager.h"

namespace fc
{
/** Trasformazione globale di una palette: rotazione della tinta (gradi), moltiplicatore della saturazione, spostamento della luminosita' dei soli fondi (-1..+1). */
theme::Palette transformPalette (const theme::Palette&, float hueDegrees, float saturationFactor, float surfaceBrightnessDelta);

/** Finestra "Colori del tema": parte da un tema, ritocca tinta, saturazione e luminosita' con i cursori e cambia ogni colore con la tavolozza,
    i campioni e i cursori RGB. Ogni modifica si vede subito e si salva come tema "Personalizzato". */
class ThemeEditor : public juce::Component, private juce::ChangeListener
{
public:
    ThemeEditor();
    ~ThemeEditor() override;
    void resized() override;
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

private:
    struct Role { const char* label; juce::Colour theme::Palette::* member; };
    static const std::vector<Role>& roles();
    struct Selector;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void commit();
    void loadBase();
    void loadRole();

    theme::Palette work, initial;   // initial: il tema com'era all'apertura della finestra
    float lastHue = 0.0f, lastSat = 1.0f, lastLum = 0.0f;
    bool loading = true;
    juce::Label baseLbl, roleLbl, hueLbl, satLbl, lumLbl;
    juce::ComboBox base, role;
    juce::Slider hue, sat, lum;
    std::unique_ptr<Selector> selector;
    juce::TextButton resetBtn { TRANS ("Ripristina il tema di partenza") };
};
} // namespace fc
