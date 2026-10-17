#include "ExternalSync.h"
#include "Settings.h"

namespace fc::mtc
{
using namespace theme;

namespace
{
int fmtForRateCode (int code)   // 0: 24, 1: 25, 2: 29,97 drop, 3: 30
{
    static const int idx[] = { 1, 2, 4, 5 };
    return idx[juce::jlimit (0, 3, code)];
}
} // namespace

void Decoder::complete (double nowMs)
{
    // 8 quarter-frame trasportano il timecode di 2 fotogrammi prima: a fine ciclo si aggiungono
    f = pieces[0] | (pieces[1] << 4);
    s = pieces[2] | (pieces[3] << 4);
    m = pieces[4] | (pieces[5] << 4);
    h = pieces[6] | ((pieces[7] & 1) << 4);
    rateCode = (pieces[7] >> 1) & 3;
    baseFmt = fmtForRateCode (rateCode);
    const auto& fmt = tcFormats()[baseFmt];
    double secs = 0.0;
    if (timecodeToSeconds (juce::String::formatted ("%02d:%02d:%02d:%02d", h, m, s, juce::jmin (f, fmt.nominal - 1)), fmt, secs))
    {
        valid = true;
        baseSeconds = secs + 2.0 / fmt.fps;
        baseMs = nowMs;
    }
}

void Decoder::feed (const juce::MidiMessage& msg, double nowMs)
{
    const auto* d = msg.getRawData();
    const int n = msg.getRawDataSize();
    if (n >= 2 && d[0] == 0xF1)
    {
        const int piece = (d[1] >> 4) & 7, value = d[1] & 0x0F;
        if (piece != expectedNext) { received = 0; }          // un buco: si riparte dal primo
        if (piece == 0) received = 0;
        pieces[piece] = value;
        ++received;
        expectedNext = (piece + 1) & 7;
        lastQuarterMs = nowMs;
        running = true;
        if (piece == 7 && received >= 8) complete (nowMs);
        return;
    }
    // full-frame: F0 7F 7F 01 01 hh mm ss ff F7 (hh = 0rrhhhhh)
    if (n >= 10 && d[0] == 0xF0 && d[1] == 0x7F && d[3] == 0x01 && d[4] == 0x01)
    {
        rateCode = (d[5] >> 5) & 3;
        h = d[5] & 0x1F; m = d[6]; s = d[7]; f = d[8];
        baseFmt = fmtForRateCode (rateCode);
        const auto& fmt = tcFormats()[baseFmt];
        double secs = 0.0;
        if (timecodeToSeconds (juce::String::formatted ("%02d:%02d:%02d:%02d", h, m, s, juce::jmin (f, fmt.nominal - 1)), fmt, secs))
        {
            valid = true;
            baseSeconds = secs;
            baseMs = nowMs;
            running = false;
            lastQuarterMs = -1.0;
        }
    }
}

Decoder::State Decoder::state (double nowMs) const
{
    State st;
    st.valid = valid;
    st.fmtIndex = baseFmt;
    st.running = valid && lastQuarterMs >= 0.0 && nowMs - lastQuarterMs < 150.0;
    st.seconds = baseSeconds + (st.running ? (nowMs - baseMs) / 1000.0 : 0.0);
    if (valid) st.text = secondsToTimecode (st.seconds, tcFormats()[baseFmt]);
    return st;
}

Decision decide (const Decoder::State& st, double tcStartSec, double offsetSec, double rate, bool playing, int64_t pos, Memory& mem, double driftFrames)
{
    Decision d;
    if (st.running)
    {
        const double fps = tcFormats()[st.fmtIndex].fps;
        d.position = (int64_t) std::llround ((st.seconds - tcStartSec + offsetSec) * rate);
        if (d.position < 0) d.position = 0;
        if (! playing)
        {
            mem.badTicks = 0;
            mem.startedByMtc = true;
            d.action = Action::LocateAndPlay;
        }
        else if (std::abs ((double) (pos - d.position)) > driftFrames / fps * rate)
        {
            if (++mem.badTicks >= 2) { mem.badTicks = 0; d.action = Action::Locate; }
        }
        else mem.badTicks = 0;
        return d;
    }
    mem.badTicks = 0;
    if (playing && mem.startedByMtc) { d.action = Action::Stop; mem.startedByMtc = false; }
    return d;
}

//==============================================================================
Sync::Sync (AudioEngine& e, std::function<double()> start) : engine (e), tcStart (std::move (start)) { startTimerHz (40); }

Sync::~Sync()
{
    stopTimer();
    if (input != nullptr) input->stop();
}

juce::String Sync::open (const juce::String& id)
{
    if (input != nullptr) { input->stop(); input.reset(); }
    if (id.isEmpty()) return {};
    input = juce::MidiInput::openDevice (id, this);
    if (input == nullptr) return TRANS ("Impossibile aprire l'ingresso MIDI.");
    input->start();
    return {};
}

void Sync::setEnabled (bool on)
{
    enabled = on;
    memory = {};
    locked = false;
}

void Sync::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    const juce::ScopedLock sl (lock);
    decoder.feed (m, juce::Time::getMillisecondCounterHiRes());
}

Decoder::State Sync::state() const
{
    const juce::ScopedLock sl (lock);
    return decoder.state (juce::Time::getMillisecondCounterHiRes());
}

void Sync::timerCallback()
{
    if (! enabled || input == nullptr) { locked = false; return; }
    const auto st = state();
    const auto d = decide (st, tcStart(), offsetSec, engine.getSampleRate(), engine.isPlaying(), engine.getPosition(), memory);
    locked = st.running;
    switch (d.action)
    {
        case Action::LocateAndPlay: engine.setPosition (d.position); engine.play(); break;
        case Action::Locate:        engine.setPosition (d.position); break;
        case Action::Stop:          engine.stop(); break;
        default: break;
    }
}

//==============================================================================
Panel::Panel (Sync& s) : sync (s)
{
    auto label = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (font (12.0f, true));
        l.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (l);
    };
    label (deviceLbl, TRANS ("INGRESSO MIDI")); label (offsetLbl, TRANS ("SPOSTAMENTO (MILLISECONDI)"));
    infos = juce::MidiInput::getAvailableDevices();
    for (int i = 0; i < infos.size(); ++i) devices.addItem (infos[i].name, i + 1);
    devices.setTextWhenNothingSelected (TRANS ("(nessun ingresso MIDI)"));
    const auto saved = settings().getValue ("mtc.device", "");
    for (int i = 0; i < infos.size(); ++i) if (infos[i].identifier == saved) devices.setSelectedId (i + 1, juce::dontSendNotification);
    offset.setText (settings().getValue ("mtc.offsetMs", "0"), false);
    offset.setInputRestrictions (8, "-0123456789.,");
    offset.setJustification (juce::Justification::centred);
    enable.setToggleState (sync.isEnabled(), juce::dontSendNotification);
    status.setFont (font (14.0f, true));
    status.setJustificationType (juce::Justification::centredLeft);
    hint.setFont (font (12.0f));
    hint.setColour (juce::Label::textColourId, dim);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setText (TRANS ("Quando arriva il timecode MTC il trasporto si posiziona e parte; se si ferma, si ferma; se la deriva supera 2 fotogrammi si riallinea. "
                  "La posizione zero della sessione corrisponde all'inizio sessione (File > Impostazioni sessione). Riceve MTC, non lo genera, e non legge l'LTC."),
                  juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &devices, &offset, &enable, &status, &hint }) addAndMakeVisible (*c);
    loading = false;
    devices.onChange = [this] { apply(); };
    offset.onTextChange = [this] { apply(); };
    enable.onClick = [this] { apply(); };
    startTimerHz (10);
}

void Panel::apply()
{
    if (loading) return;
    const int sel = devices.getSelectedId() - 1;
    juce::String err;
    if (sel >= 0 && sel < infos.size())
    {
        err = sync.open (infos[sel].identifier);
        settings().setValue ("mtc.device", infos[sel].identifier);
    }
    sync.setOffsetMs (offset.getText().replaceCharacter (',', '.').getDoubleValue());
    settings().setValue ("mtc.offsetMs", offset.getText());
    settings().saveIfNeeded();
    if (err.isNotEmpty()) { enable.setToggleState (false, juce::dontSendNotification); status.setText (err, juce::dontSendNotification); sync.setEnabled (false); return; }
    sync.setEnabled (enable.getToggleState() && sel >= 0);
}

void Panel::timerCallback()
{
    const auto st = sync.state();
    juce::String t;
    juce::Colour c = dim;
    if (! sync.isEnabled()) t = st.valid ? TRANS ("Non agganciato (ultimo timecode ricevuto ") + st.text + ")" : TRANS ("Non agganciato");
    else if (! st.valid) t = TRANS ("In attesa del timecode...");
    else if (st.running) { t = TRANS ("AGGANCIATO") + "  " + st.text + "  (" + juce::String (tcFormats()[st.fmtIndex].name) + ")"; c = green; }
    else { t = TRANS ("Timecode fermo a ") + st.text; c = yellow; }
    status.setColour (juce::Label::textColourId, c);
    status.setText (t, juce::dontSendNotification);
}

void Panel::resized()
{
    auto r = getLocalBounds().reduced (18);
    auto row = [&r] (juce::Label& l, juce::Component& c)
    {
        auto line = r.removeFromTop (28);
        l.setBounds (line.removeFromLeft (210));
        c.setBounds (line);
        r.removeFromTop (8);
    };
    row (deviceLbl, devices);
    row (offsetLbl, offset);
    enable.setBounds (r.removeFromTop (26));
    r.removeFromTop (10);
    status.setBounds (r.removeFromTop (28));
    r.removeFromTop (8);
    hint.setBounds (r);
}
} // namespace fc::mtc
