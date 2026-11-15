#include "ThemeEditor.h"

namespace fc
{
using namespace theme;

Palette transformPalette (const Palette& in, float hueDeg, float satFactor, float lumDelta)
{
    Palette p = in;
    auto colour = [&] (juce::Colour c)
    {
        return c.withRotatedHue (hueDeg / 360.0f).withMultipliedSaturation (satFactor);
    };
    auto surface = [&] (juce::Colour c)
    {
        c = colour (c);
        return c.withBrightness (juce::jlimit (0.0f, 1.0f, c.getBrightness() + lumDelta));
    };
    p.bg = surface (in.bg); p.panel = surface (in.panel); p.panelHi = surface (in.panelHi); p.panelTop = surface (in.panelTop);
    p.lane = surface (in.lane); p.laneAlt = surface (in.laneAlt); p.grid = surface (in.grid); p.border = colour (in.border);
    p.text = colour (in.text); p.dim = colour (in.dim); p.accent = colour (in.accent); p.blue = colour (in.blue); p.playhead = colour (in.playhead);
    p.green = colour (in.green); p.yellow = colour (in.yellow); p.muteCol = colour (in.muteCol); p.soloCol = colour (in.soloCol);
    return p;
}

struct ThemeEditor::Selector : juce::ColourSelector
{
    Selector() : juce::ColourSelector (showColourAtTop | showSliders | showColourspace, 6, 8) {}
    int getNumSwatches() const override { return 16; }
    juce::Colour getSwatchColour (int i) const override
    {
        static const juce::uint32 c[] = { 0xffffffff, 0xffc0c0c0, 0xff808080, 0xff404040, 0xff000000, 0xff04051a, 0xff0a1038, 0xff00d9ff,
                                          0xffff2e93, 0xffff4d3a, 0xffff9f43, 0xffffe600, 0xff00ff85, 0xff00b8ff, 0xff6f7be0, 0xffb36fd6 };
        return juce::Colour (c[juce::jlimit (0, 15, i)]);
    }
    void setSwatchColour (int, const juce::Colour&) override {}
};

const std::vector<ThemeEditor::Role>& ThemeEditor::roles()
{
    static const std::vector<Role> r = {
        { "Sfondo", &Palette::bg }, { "Pannelli", &Palette::panel }, { "Pannelli in evidenza", &Palette::panelHi }, { "Corsie delle tracce", &Palette::lane },
        { "Bordi", &Palette::border }, { "Testo", &Palette::text }, { "Testo secondario", &Palette::dim }, { "Registrazione e accento", &Palette::accent },
        { "Selezione", &Palette::blue }, { "Testina", &Palette::playhead }, { "Verde (ok)", &Palette::green }, { "Giallo", &Palette::yellow },
        { "Mute", &Palette::muteCol }, { "Solo", &Palette::soloCol } };
    return r;
}

ThemeEditor::ThemeEditor()
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (baseLbl, TRANS ("PARTI DA")); label (roleLbl, TRANS ("COLORE DA CAMBIARE"));
    label (hueLbl, TRANS ("TINTA")); label (satLbl, TRANS ("SATURAZIONE")); label (lumLbl, TRANS ("LUMINOSITA' DEI FONDI"));

    work = initial = Palette::current();
    base.addItem (TRANS ("(il tema attuale)"), 1);
    int id = 2;
    for (auto& t : ThemeManager::builtins()) if (t.id != "custom") base.addItem (t.name, id++);
    base.setSelectedId (1, juce::dontSendNotification);
    for (size_t i = 0; i < roles().size(); ++i) role.addItem (TRANS (roles()[i].label), (int) i + 1);
    role.setSelectedId (1, juce::dontSendNotification);

    hue.setRange (-180.0, 180.0, 1.0); hue.setValue (0.0, juce::dontSendNotification); hue.setTextValueSuffix (juce::String::fromUTF8 (" \xc2\xb0"));
    sat.setRange (0.0, 200.0, 1.0);    sat.setValue (100.0, juce::dontSendNotification); sat.setTextValueSuffix (" %");
    lum.setRange (-30.0, 30.0, 1.0);   lum.setValue (0.0, juce::dontSendNotification);   lum.setTextValueSuffix (" %");
    for (auto* s : { &hue, &sat, &lum })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        s->setDoubleClickReturnValue (true, s == &sat ? 100.0 : 0.0);
        s->onValueChange = [this]
        {
            if (loading) return;
            // le modifiche si compongono: si applica solo la variazione rispetto all'ultima posizione dei cursori, cosi' i ritocchi ai singoli colori restano
            const float h = (float) hue.getValue(), sa = (float) sat.getValue() / 100.0f, l = (float) lum.getValue() / 100.0f;
            work = transformPalette (work, h - lastHue, juce::jmax (0.02f, sa) / juce::jmax (0.02f, lastSat), l - lastLum);
            lastHue = h; lastSat = sa; lastLum = l;
            commit();
        };
    }
    selector = std::make_unique<Selector>();
    selector->addChangeListener (this);
    resetBtn.onClick = [this] { loadBase(); };
    base.onChange = [this] { loadBase(); };
    role.onChange = [this] { loadRole(); };
    for (auto* c : std::initializer_list<juce::Component*> { &base, &role, &hue, &sat, &lum, selector.get(), &resetBtn }) addAndMakeVisible (*c);
    loadRole();
    loading = false;
}

ThemeEditor::~ThemeEditor() { selector->removeChangeListener (this); }

void ThemeEditor::loadBase()
{
    const int id = base.getSelectedId();
    if (id >= 2)
    {
        int k = 2;
        for (auto& t : ThemeManager::builtins())
            if (t.id != "custom" && k++ == id) work = t.palette;
    }
    else work = initial;
    loading = true;
    hue.setValue (0.0, juce::dontSendNotification); sat.setValue (100.0, juce::dontSendNotification); lum.setValue (0.0, juce::dontSendNotification);
    lastHue = 0.0f; lastSat = 1.0f; lastLum = 0.0f;
    loading = false;
    loadRole();
    commit();
}

void ThemeEditor::loadRole()
{
    const int i = role.getSelectedId() - 1;
    if (i < 0) return;
    const bool was = loading;
    loading = true;
    selector->setCurrentColour (work.*(roles()[(size_t) i].member), juce::dontSendNotification);
    loading = was;
}

void ThemeEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (loading) return;
    const int i = role.getSelectedId() - 1;
    if (i < 0) return;
    auto& m = roles()[(size_t) i].member;
    work.*m = selector->getCurrentColour();
    // i colori derivati seguono quelli da cui dipendono
    if (m == &Palette::panel || m == &Palette::panelHi) work.panelTop = work.panel.interpolatedWith (work.panelHi, 0.5f);
    if (m == &Palette::lane || m == &Palette::panel) work.laneAlt = work.lane.interpolatedWith (work.panel, 0.5f);
    if (m == &Palette::lane || m == &Palette::border) work.grid = work.lane.interpolatedWith (work.border, 0.85f);
    commit();
}

void ThemeEditor::commit()
{
    work.name = "Personalizzato";
    ThemeManager::saveCustom (work);   // salva, applica e seleziona il tema "Personalizzato"
}

void ThemeEditor::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto row = [&r] (juce::Label& l, juce::Component& c, int h = 26)
    {
        l.setBounds (r.removeFromTop (16));
        c.setBounds (r.removeFromTop (h));
        r.removeFromTop (8);
    };
    row (baseLbl, base, 26);
    row (hueLbl, hue, 22); row (satLbl, sat, 22); row (lumLbl, lum, 22);
    r.removeFromTop (6);
    row (roleLbl, role, 26);
    resetBtn.setBounds (r.removeFromBottom (28));
    r.removeFromBottom (8);
    selector->setBounds (r);
}
} // namespace fc
