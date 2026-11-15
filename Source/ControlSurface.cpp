#include "ControlSurface.h"
#include "Settings.h"

namespace fc::surface
{
using namespace theme;

Event decode (const juce::MidiMessage& m)
{
    Event e;
    if (m.isPitchWheel() && m.getChannel() <= 8)
    {
        e.kind = Event::Fader; e.index = m.getChannel() - 1; e.value = (float) m.getPitchWheelValue() / 16383.0f;
    }
    else if (m.isController() && m.getControllerNumber() >= 0x10 && m.getControllerNumber() <= 0x17)
    {
        const int v = m.getControllerValue();
        e.kind = Event::Knob; e.index = m.getControllerNumber() - 0x10; e.value = (float) (v < 0x40 ? v : -(v - 0x40));
    }
    else if (m.isNoteOn())   // note off e note on con velocita' 0 (tasto rilasciato) sono ignorati
    {
        const int n = m.getNoteNumber();
        if (n < 0x20) { e.kind = n < 8 ? Event::Rec : n < 16 ? Event::Solo : n < 24 ? Event::Mute : Event::Select; e.index = n & 7; }
        else switch (n)
        {
            case 0x2E: e.kind = Event::BankDown; break;
            case 0x2F: e.kind = Event::BankUp; break;
            case 0x5B: e.kind = Event::Rewind; break;
            case 0x5C: e.kind = Event::Forward; break;
            case 0x5D: e.kind = Event::Stop; break;
            case 0x5E: e.kind = Event::Play; break;
            case 0x5F: e.kind = Event::Record; break;
            default: break;
        }
    }
    return e;
}

float faderToDb (float pos)
{
    juce::NormalisableRange<double> r (-80.0, 6.0, 0.1);
    r.setSkewForCentre (-18.0);
    return pos <= 0.0f ? -80.0f : (float) r.convertFrom0to1 (juce::jlimit (0.0, 1.0, (double) pos));
}

juce::String savedDevice() { return settings().getValue ("surface.device", ""); }

Surface::Surface (Project& p, Transport t) : project (p), tr (std::move (t)) {}

Surface::~Surface() { *alive = false; for (auto& i : inputs) i->stop(); }

juce::String Surface::open (const juce::String& id)
{
    for (auto& i : inputs) i->stop();
    inputs.clear();
    if (id.isEmpty()) return {};
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (id == "*" || d.identifier == id)
            if (auto in = juce::MidiInput::openDevice (d.identifier, this)) { in->start(); inputs.push_back (std::move (in)); }
    return inputs.empty() ? TRANS ("Impossibile aprire l'ingresso MIDI.") : juce::String();
}

void Surface::handleIncomingMidiMessage (juce::MidiInput* src, const juce::MidiMessage& m)
{
    const auto e = decode (m);
    if (e.kind == Event::None) return;
    {
        // il mixer ripete gli stessi messaggi su due porte (Master e Private): con "tutti gli ingressi" un tasto verrebbe contato due volte e si annullerebbe
        const juce::ScopedLock sl (lock);
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const juce::String raw = juce::String::toHexString (m.getRawData(), m.getRawDataSize());
        if (src != lastSrc && raw == lastRaw && now - lastMs < 30.0) return;
        lastSrc = src; lastRaw = raw; lastMs = now;
    }
    juce::MessageManager::callAsync ([this, e, a = alive] { if (*a) apply (e); });
}

Track* Surface::trackAt (int strip)
{
    int n = 0;
    for (auto& t : project.edit().tracks)
        if (! t.hidden && n++ == bank + strip) return &t;
    return nullptr;
}

juce::String Surface::lastEvent() const { const juce::ScopedLock sl (lock); return last; }

void Surface::apply (const Event& e)
{
    static const char* names[] = { "", "fader", "manopola", "rec", "solo", "mute", "select", "rew", "ff", "stop", "play", "record", "banco -", "banco +" };
    auto* t = e.kind >= Event::Fader && e.kind <= Event::Select ? trackAt (e.index) : nullptr;
    {
        juce::String s = names[e.kind];
        if (e.kind >= Event::Fader && e.kind <= Event::Select) s << " " << (e.index + 1) << (t != nullptr ? " -> " + t->name : " (" + TRANS ("nessuna traccia") + ")");
        const juce::ScopedLock sl (lock);
        last = s;
    }
    switch (e.kind)
    {
        case Event::Fader:  if (t) { t->live->volumeDb = faderToDb (e.value); project.liveChanged(); } break;
        case Event::Knob:   if (t) { t->live->pan = juce::jlimit (-1.0f, 1.0f, t->live->pan.load() + 0.05f * e.value); project.liveChanged(); } break;
        case Event::Rec:    if (t && tr.arm) tr.arm (*t); break;
        case Event::Solo:   if (t) { t->live->solo = ! t->live->solo.load(); project.liveChanged(); } break;
        case Event::Mute:   if (t) { t->live->mute = ! t->live->mute.load(); project.liveChanged(); } break;
        case Event::Select: if (t) { project.selectOnlyTrack (t->id); project.sendChangeMessage(); } break;
        case Event::Rewind:  if (tr.rewind) tr.rewind(); break;
        case Event::Forward: if (tr.forward) tr.forward(); break;
        case Event::Stop:    if (tr.stop) tr.stop(); break;
        case Event::Play:    if (tr.play) tr.play(); break;
        case Event::Record:  if (tr.record) tr.record(); break;
        case Event::BankDown: bank = juce::jmax (0, bank - 8); break;
        case Event::BankUp:  { int vis = 0; for (auto& x : project.getSession().tracks) vis += ! x.hidden; if (bank + 8 < vis) bank += 8; break; }
        default: break;
    }
}

Panel::Panel (Surface& s) : surface (s)
{
    deviceLbl.setText (TRANS ("CONTROLLER MIDI (MACKIE CONTROL)"), juce::dontSendNotification);
    deviceLbl.setFont (font (12.0f, true));
    deviceLbl.setColour (juce::Label::textColourId, dim);
    infos = juce::MidiInput::getAvailableDevices();
    devices.addItem (TRANS ("(disattivato)"), 1);
    devices.addItem (TRANS ("Tutti gli ingressi MIDI"), 2);
    for (int i = 0; i < infos.size(); ++i) devices.addItem (infos[i].name, i + 3);
    const auto saved = savedDevice();
    devices.setSelectedId (saved.isEmpty() ? 1 : saved == "*" ? 2 : 1, juce::dontSendNotification);
    for (int i = 0; i < infos.size(); ++i) if (infos[i].identifier == saved) devices.setSelectedId (i + 3, juce::dontSendNotification);
    status.setFont (font (14.0f, true));
    hint.setFont (font (12.0f));
    hint.setColour (juce::Label::textColourId, dim);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setText (TRANS ("Fader 1-8: volume delle 8 tracce visibili. Manopole: pan. Tasti Rec, Solo, Mute, Select per traccia; Stop, Play, Record per il trasporto; « e » per la battuta ADR (o il marker, se non ci sono battute) precedente/successiva; "
                         "Banco -/+ per passare alle 8 tracce successive. I fader non sono motorizzati: se la posizione non corrisponde al volume, il volume salta al primo movimento. "
                         "Se il mixer ha piu' porte MIDI e non risponde, scegli \"Tutti gli ingressi MIDI\"."), juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &deviceLbl, &devices, &status, &hint }) addAndMakeVisible (*c);
    loading = false;
    devices.onChange = [this] { apply(); };
    apply();
    startTimerHz (5);
}

void Panel::timerCallback()
{
    if (surface.isOpen()) status.setText (TRANS ("Attivo - ultimo comando ricevuto: ") + (surface.lastEvent().isEmpty() ? TRANS ("nessuno") : surface.lastEvent()), juce::dontSendNotification);
}

void Panel::apply()
{
    if (loading) return;
    const int id = devices.getSelectedId();
    const juce::String dev = id <= 1 ? juce::String() : id == 2 ? juce::String ("*") : infos[id - 3].identifier;
    const auto err = surface.open (dev);
    settings().setValue ("surface.device", dev);
    settings().saveIfNeeded();
    status.setColour (juce::Label::textColourId, err.isNotEmpty() ? yellow : surface.isOpen() ? green : dim);
    status.setText (err.isNotEmpty() ? err : surface.isOpen() ? TRANS ("Attivo") : TRANS ("Disattivato"), juce::dontSendNotification);
}

void Panel::resized()
{
    auto r = getLocalBounds().reduced (18);
    deviceLbl.setBounds (r.removeFromTop (22));
    devices.setBounds (r.removeFromTop (28));
    r.removeFromTop (10);
    status.setBounds (r.removeFromTop (28));
    r.removeFromTop (8);
    hint.setBounds (r);
}
} // namespace fc::surface
