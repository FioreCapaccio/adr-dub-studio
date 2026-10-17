#include "BuiltinFx.h"

namespace fc
{
namespace
{
const char* const kIds[BuiltinFx::numKinds] = { "filter", "deesser", "gate", "multiband", "reverb", "delay", "saturator", "chorus" };

juce::String u8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
float gainToDb (float g) noexcept { return 20.0f * std::log10 (g + 1.0e-9f); }

/** Lettura con interpolazione lineare, `delay` campioni indietro rispetto alla posizione di scrittura. */
float readFrac (const std::vector<float>& b, int writePos, float delay) noexcept
{
    const int n = (int) b.size();
    float rp = (float) writePos - delay;
    while (rp < 0.0f) rp += (float) n;
    int i0 = (int) rp;
    const float f = rp - (float) i0;
    i0 %= n;
    const int i1 = i0 + 1 == n ? 0 : i0 + 1;
    return b[(size_t) i0] * (1.0f - f) + b[(size_t) i1] * f;
}
} // namespace

//==============================================================================
BuiltinFx::BuiltinFx (Kind k)
    : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo()).withOutput ("Out", juce::AudioChannelSet::stereo())),
      kind (k)
{
    switch (kind)
    {
        case Filter:
            addFloat ("hp", TRANS ("Passa-alto"), 20.0f, 1000.0f, 20.0f, " Hz", 100.0f);        // 0: 20 Hz = spento
            addFloat ("lp", TRANS ("Passa-basso"), 1000.0f, 20000.0f, 20000.0f, " Hz", 5000.0f); // 1: 20 kHz = spento
            break;
        case DeEsser:
            addFloat ("freq", TRANS ("Frequenza"), 2000.0f, 12000.0f, 6500.0f, " Hz", 6000.0f);  // 0
            addFloat ("thr", TRANS ("Soglia"), -50.0f, 0.0f, -30.0f, " dB");                      // 1
            addFloat ("max", TRANS ("Riduzione max"), 0.0f, 18.0f, 9.0f, " dB");                  // 2
            break;
        case Gate:
            addFloat ("thr", TRANS ("Soglia"), -80.0f, 0.0f, -50.0f, " dB");                      // 0
            addFloat ("range", TRANS ("Attenuazione"), 0.0f, 80.0f, 40.0f, " dB");                // 1
            addFloat ("att", TRANS ("Attacco"), 0.1f, 50.0f, 1.0f, " ms", 5.0f);                  // 2
            addFloat ("rel", TRANS ("Rilascio"), 5.0f, 1000.0f, 120.0f, " ms", 150.0f);           // 3
            break;
        case Multiband:
            addFloat ("xo1", TRANS ("Crossover basso/medio"), 50.0f, 1000.0f, 200.0f, " Hz", 250.0f);   // 0
            addFloat ("xo2", TRANS ("Crossover medio/alto"), 1000.0f, 12000.0f, 3000.0f, " Hz", 3500.0f); // 1
            addFloat ("att", TRANS ("Attacco"), 0.5f, 100.0f, 10.0f, " ms", 15.0f);                      // 2
            addFloat ("rel", TRANS ("Rilascio"), 20.0f, 1000.0f, 150.0f, " ms", 200.0f);                 // 3
            addFloat ("out", TRANS ("Uscita"), -12.0f, 12.0f, 0.0f, " dB");                              // 4
            for (const char* band : { "low", "mid", "high" })                                   // 5.. : soglia, rapporto, guadagno di ogni banda
            {
                const juce::String id (band), nm = id == "low" ? TRANS ("Bassi") : id == "mid" ? TRANS ("Medi") : TRANS ("Alti");
                addFloat (id + "Thr", nm + TRANS (" - soglia"), -60.0f, 0.0f, -24.0f, " dB");
                addFloat (id + "Ratio", nm + TRANS (" - rapporto"), 1.0f, 20.0f, 3.0f, ":1", 4.0f);
                addFloat (id + "Gain", nm + TRANS (" - guadagno"), -12.0f, 12.0f, 0.0f, " dB");
            }
            break;
        case Reverb:
            addFloat ("size", TRANS ("Dimensione"), 0.0f, 1.0f, 0.5f);                            // 0
            addFloat ("damp", TRANS ("Smorzamento"), 0.0f, 1.0f, 0.5f);                           // 1
            addFloat ("width", TRANS ("Larghezza"), 0.0f, 1.0f, 1.0f);                            // 2
            addFloat ("mix", TRANS ("Mix"), 0.0f, 100.0f, 25.0f, " %");                           // 3
            break;
        case Delay:
            addFloat ("time", TRANS ("Tempo"), 1.0f, 1500.0f, 250.0f, " ms", 300.0f);             // 0
            addFloat ("fb", TRANS ("Feedback"), 0.0f, 90.0f, 35.0f, " %");                        // 1
            addFloat ("mix", TRANS ("Mix"), 0.0f, 100.0f, 30.0f, " %");                           // 2
            addBool ("pingpong", TRANS ("Ping-pong"), false);                                     // 3
            break;
        case Saturator:
            addFloat ("drive", TRANS ("Drive"), 0.0f, 30.0f, 10.0f, " dB");                       // 0
            addFloat ("tone", TRANS ("Tono (passa-basso)"), 1000.0f, 20000.0f, 20000.0f, " Hz", 5000.0f); // 1
            addFloat ("mix", TRANS ("Mix"), 0.0f, 100.0f, 100.0f, " %");                          // 2
            addFloat ("out", TRANS ("Uscita"), -12.0f, 12.0f, 0.0f, " dB");                       // 3
            break;
        case Chorus:
            addFloat ("rate", TRANS ("Frequenza"), 0.1f, 5.0f, 0.8f, " Hz", 1.0f);                // 0
            addFloat ("depth", u8 ("Profondit\xc3\xa0"), 0.0f, 10.0f, 4.0f, " ms");       // 1
            addFloat ("mix", TRANS ("Mix"), 0.0f, 100.0f, 50.0f, " %");                           // 2
            break;
        default: break;
    }
}

void BuiltinFx::addFloat (const juce::String& id, const juce::String& name, float lo, float hi, float def, const juce::String& unit, float centre)
{
    juce::NormalisableRange<float> r (lo, hi);
    if (centre > 0.0f) r.setSkewForCentre (centre);
    const int decimals = unit.isEmpty() ? 2 : unit.contains ("Hz") ? 0 : 1;   // senza unita' (0..1) due decimali, Hz interi, altrimenti uno; interi oltre 100
    auto attrs = juce::AudioParameterFloatAttributes().withLabel (unit)
                     .withStringFromValueFunction ([decimals] (float v, int) { return juce::String (v, std::abs (v) >= 100.0f ? 0 : decimals); });
    auto* p = new juce::AudioParameterFloat (juce::ParameterID (id, 1), name, r, def, attrs);
    addParameter (p);
    params.push_back (p);
}

void BuiltinFx::addBool (const juce::String& id, const juce::String& name, bool def)
{
    auto* p = new juce::AudioParameterBool (juce::ParameterID (id, 1), name, def);
    addParameter (p);
    params.push_back (p);
}

juce::RangedAudioParameter* BuiltinFx::param (const juce::String& id) const
{
    for (auto* p : params) if (p->getParameterID() == id) return p;
    return nullptr;
}

//==============================================================================
juce::String BuiltinFx::kindName (Kind k)
{
    switch (k)
    {
        case Filter:    return TRANS ("Filtro HP/LP");
        case DeEsser:   return TRANS ("De-esser");
        case Gate:      return TRANS ("Gate / Expander");
        case Multiband: return TRANS ("Compressore multibanda");
        case Reverb:    return TRANS ("Riverbero");
        case Delay:     return TRANS ("Delay");
        case Saturator: return TRANS ("Saturazione");
        case Chorus:    return TRANS ("Chorus");
        default:        return {};
    }
}

juce::String BuiltinFx::kindHelp (Kind k)
{
    switch (k)
    {
        case Filter:    return TRANS ("Taglia i bassi (rumble, vento, pop) e gli alti: filtri a 24 dB/ottava.");
        case DeEsser:   return TRANS ("Riduce le sibilanti (s, sc) del dialogo senza toccare il resto della voce.");
        case Gate:      return TRANS ("Abbassa il segnale sotto soglia: elimina rumore di fondo e rientri fra le battute.");
        case Multiband: return TRANS ("Comprime separatamente bassi, medi e alti: uniforma la voce senza pompare.");
        case Reverb:    return TRANS ("Riverbero stereo a stanza: per dare ambiente a un dialogo registrato in studio.");
        case Delay:     return TRANS ("Eco stereo con feedback e modalita' ping-pong.");
        case Saturator: return TRANS ("Saturazione morbida (tanh): scalda e fa emergere la voce o gli effetti.");
        case Chorus:    return TRANS ("Raddoppio modulato: allarga e ispessisce il suono.");
        default:        return {};
    }
}

juce::PluginDescription BuiltinFx::describe (Kind k)
{
    juce::PluginDescription d;
    d.name = kindName (k);
    d.descriptiveName = kindHelp (k);
    d.pluginFormatName = formatName;
    d.fileOrIdentifier = juce::String ("builtin:") + kIds[k];
    d.manufacturerName = "ADR Dub Studio";
    d.category = TRANS ("Effetto");
    d.version = "1.0";
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
    d.uniqueId = d.deprecatedUid = juce::String (kIds[k]).hashCode();
    return d;
}

std::unique_ptr<BuiltinFx> BuiltinFx::create (const juce::PluginDescription& d)
{
    if (d.pluginFormatName != formatName) return nullptr;
    for (int k = 0; k < numKinds; ++k)
        if (d.fileOrIdentifier == juce::String ("builtin:") + kIds[k])
            return std::make_unique<BuiltinFx> ((Kind) k);
    return nullptr;
}

juce::String BuiltinFx::kindCategory (Kind k)
{
    switch (k)
    {
        case Filter:    return TRANS ("Filtro");
        case DeEsser:
        case Gate:
        case Multiband: return TRANS ("Dinamica");
        case Reverb:
        case Delay:     return TRANS ("Spazio");
        case Saturator: return TRANS ("Colore");
        case Chorus:    return TRANS ("Modulazione");
        default:        return {};
    }
}

juce::String BuiltinFx::kindBadge (Kind k)
{
    static const char* const b[numKinds] = { "HP", "DS", "GT", "MB", "RV", "DL", "ST", "CH" };
    return b[k];
}

std::vector<BuiltinFx::Group> BuiltinFx::layout (Kind k)
{
    switch (k)
    {
        case Filter:    return { { TRANS ("Filtro"), { { "hp", TRANS ("Passa-alto") }, { "lp", TRANS ("Passa-basso") } }, -1 } };
        case DeEsser:   return { { TRANS ("De-esser"), { { "freq", TRANS ("Frequenza") }, { "thr", TRANS ("Soglia") }, { "max", TRANS ("Riduzione max") } }, 0 } };
        case Gate:      return { { TRANS ("Soglia"), { { "thr", TRANS ("Soglia") }, { "range", TRANS ("Attenuazione") } }, 0 },
                                 { TRANS ("Tempi"), { { "att", TRANS ("Attacco") }, { "rel", TRANS ("Rilascio") } }, -1 } };
        case Multiband: return { { TRANS ("Crossover"), { { "xo1", TRANS ("Bassi / medi") }, { "xo2", TRANS ("Medi / alti") } }, -1 },
                                 { TRANS ("Tempi e uscita"), { { "att", TRANS ("Attacco") }, { "rel", TRANS ("Rilascio") }, { "out", TRANS ("Uscita") } }, -1 },
                                 { TRANS ("Bassi"), { { "lowThr", TRANS ("Soglia") }, { "lowRatio", TRANS ("Rapporto") }, { "lowGain", TRANS ("Guadagno") } }, 0, true },
                                 { TRANS ("Medi"), { { "midThr", TRANS ("Soglia") }, { "midRatio", TRANS ("Rapporto") }, { "midGain", TRANS ("Guadagno") } }, 1 },
                                 { TRANS ("Alti"), { { "highThr", TRANS ("Soglia") }, { "highRatio", TRANS ("Rapporto") }, { "highGain", TRANS ("Guadagno") } }, 2 } };
        case Reverb:    return { { TRANS ("Ambiente"), { { "size", TRANS ("Dimensione") }, { "damp", TRANS ("Smorzamento") }, { "width", TRANS ("Larghezza") } }, -1 },
                                 { TRANS ("Uscita"), { { "mix", TRANS ("Mix") } }, -1 } };
        case Delay:     return { { TRANS ("Eco"), { { "time", TRANS ("Tempo") }, { "fb", TRANS ("Feedback") }, { "mix", TRANS ("Mix") } }, -1 },
                                 { TRANS ("Stereo"), { { "pingpong", TRANS ("Ping-pong") } }, -1 } };
        case Saturator: return { { TRANS ("Saturazione"), { { "drive", TRANS ("Drive") }, { "tone", TRANS ("Tono") } }, -1 },
                                 { TRANS ("Uscita"), { { "mix", TRANS ("Mix") }, { "out", TRANS ("Livello") } }, -1 } };
        case Chorus:    return { { TRANS ("Modulazione"), { { "rate", TRANS ("Frequenza") }, { "depth", u8 ("Profondit\xc3\xa0") } }, -1 },
                                 { TRANS ("Uscita"), { { "mix", TRANS ("Mix") } }, -1 } };
        default:        return {};
    }
}

float BuiltinFx::filterResponseDb (float hz) const
{
    // Butterworth del 4 ordine (la coppia di biquad): |H|^2 = 1 / (1 + (fc/f)^8) per l'HP, 1 / (1 + (f/fc)^8) per l'LP
    const float hp = get (0), lp = get (1);
    double p = 1.0;
    if (hp > 20.5f) p *= 1.0 / (1.0 + std::pow ((double) hp / hz, 8.0));
    if (lp < 19500.0f) p *= 1.0 / (1.0 + std::pow ((double) hz / lp, 8.0));
    return (float) (10.0 * std::log10 (p));
}

bool BuiltinFx::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

double BuiltinFx::getTailLengthSeconds() const
{
    switch (kind)
    {
        case Reverb: return 4.0;
        case Delay:  return 6.0;
        case Chorus: return 0.05;
        default:     return 0.0;
    }
}

//==============================================================================
void BuiltinFx::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement x ("fx");
    for (auto* p : params) x.setAttribute (p->getParameterID(), (double) p->getValue());
    copyXmlToBinary (x, dest);
}

void BuiltinFx::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
        for (auto* p : params)
            if (x->hasAttribute (p->getParameterID()))
                p->setValueNotifyingHost ((float) x->getDoubleAttribute (p->getParameterID()));
}

//==============================================================================
void BuiltinFx::prepareToPlay (double r, int)
{
    rate = r;
    reverb.setSampleRate (rate);
    const double seconds = kind == Delay ? 1.6 : kind == Chorus ? 0.1 : 0.0;
    for (auto& rg : ring) rg.assign ((size_t) (seconds * rate) + 4, 0.0f);
    reset();
}

void BuiltinFx::reset()
{
    for (auto& ch : hp) for (auto& f : ch) f.reset();
    for (auto& ch : lp) for (auto& f : ch) f.reset();
    lastHp = lastLp = -1.0f;
    env = 0.0f; gainNow = 1.0f; gateOpen = false;
    for (auto& f : mbf) for (auto& ch : f) for (auto& s : ch) s.reset();
    mbGr = {}; mbXo1 = mbXo2 = -1.0f;
    for (auto& m : meters) m = 0.0f;
    reverb.reset();
    for (auto& rg : ring) std::fill (rg.begin(), rg.end(), 0.0f);
    ringPos = 0;
    delayNow = -1.0f;
    fbLp = {};
    lfoPhase = 0.0;
}

void BuiltinFx::processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (b.getNumChannels() < 2 || b.getNumSamples() == 0) return;

    switch (kind)
    {
        case Filter:    doFilter (b); break;
        case DeEsser:   doDeEsser (b); break;
        case Gate:      doGate (b); break;
        case Multiband: doMultiband (b); break;
        case Reverb:    doReverb (b); break;
        case Delay:     doDelay (b); break;
        case Saturator: doSaturator (b); break;
        case Chorus:    doChorus (b); break;
        default: break;
    }
}

//==============================================================================
// 4 ordine Butterworth = due biquad con Q 0.5412 e 1.3066.
void BuiltinFx::doFilter (juce::AudioBuffer<float>& b)
{
    const float h = get (0), l = juce::jmin (get (1), (float) (rate * 0.45));
    if (h != lastHp)
    {
        lastHp = h;
        for (int c = 0; c < 2; ++c)
        {
            hp[(size_t) c][0].setCoefficients (juce::IIRCoefficients::makeHighPass (rate, h, 0.5412));
            hp[(size_t) c][1].setCoefficients (juce::IIRCoefficients::makeHighPass (rate, h, 1.3066));
        }
    }
    if (l != lastLp)
    {
        lastLp = l;
        for (int c = 0; c < 2; ++c)
        {
            lp[(size_t) c][0].setCoefficients (juce::IIRCoefficients::makeLowPass (rate, l, 0.5412));
            lp[(size_t) c][1].setCoefficients (juce::IIRCoefficients::makeLowPass (rate, l, 1.3066));
        }
    }
    const bool useHp = h > 20.5f, useLp = get (1) < 19500.0f;
    for (int c = 0; c < 2; ++c)
    {
        float* d = b.getWritePointer (c);
        if (useHp) { hp[(size_t) c][0].processSamples (d, b.getNumSamples()); hp[(size_t) c][1].processSamples (d, b.getNumSamples()); }
        if (useLp) { lp[(size_t) c][0].processSamples (d, b.getNumSamples()); lp[(size_t) c][1].processSamples (d, b.getNumSamples()); }
    }
}

// Crossover Linkwitz-Riley a 4 poli: basso + alto sommati sono piatti in ampiezza; solo l'alto (le sibilanti) viene ridotto.
void BuiltinFx::doDeEsser (juce::AudioBuffer<float>& b)
{
    const float f = juce::jmin (get (0), (float) (rate * 0.45)), thr = get (1), maxRed = get (2);
    if (f != lastHp)
    {
        lastHp = f;
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 2; ++s)
            {
                hp[(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeHighPass (rate, f, 0.7071));
                lp[(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeLowPass (rate, f, 0.7071));
            }
    }
    const float atk = std::exp (-1.0f / (0.001f * (float) rate)), rel = std::exp (-1.0f / (0.05f * (float) rate));
    float* d[2] = { b.getWritePointer (0), b.getWritePointer (1) };
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        float lo[2], hi[2];
        for (int c = 0; c < 2; ++c)
        {
            lo[c] = lp[(size_t) c][1].processSingleSampleRaw (lp[(size_t) c][0].processSingleSampleRaw (d[c][i]));
            hi[c] = hp[(size_t) c][1].processSingleSampleRaw (hp[(size_t) c][0].processSingleSampleRaw (d[c][i]));
        }
        const float det = juce::jmax (std::abs (hi[0]), std::abs (hi[1]));
        const float k = det > env ? atk : rel;
        env = k * env + (1.0f - k) * det;
        const float over = gainToDb (env) - thr;
        const float g = over > 0.0f ? dbToGain (-juce::jmin (over * 0.75f, maxRed)) : 1.0f;   // rapporto fisso 4:1
        d[0][i] = lo[0] + hi[0] * g;
        d[1][i] = lo[1] + hi[1] * g;
        meters[0] = -gainToDb (g);
    }
}

void BuiltinFx::doGate (juce::AudioBuffer<float>& b)
{
    const float thr = get (0), closed = dbToGain (-get (1));
    const float attK = std::exp (-1.0f / (get (2) * 0.001f * (float) rate)), relK = std::exp (-1.0f / (get (3) * 0.001f * (float) rate));
    const float detK = std::exp (-1.0f / (0.02f * (float) rate));
    float* L = b.getWritePointer (0);
    float* R = b.getWritePointer (1);
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float lvl = juce::jmax (std::abs (L[i]), std::abs (R[i]));
        env = lvl > env ? lvl : env * detK;
        const float db = gainToDb (env);
        if (! gateOpen && db > thr) gateOpen = true;
        else if (gateOpen && db < thr - 4.0f) gateOpen = false;   // isteresi 4 dB: niente battimenti sulla soglia
        const float target = gateOpen ? 1.0f : closed;
        const float k = target > gainNow ? attK : relK;
        gainNow = target + (gainNow - target) * k;
        L[i] *= gainNow;
        R[i] *= gainNow;
    }
    meters[0] = -gainToDb (gainNow);
}

// Divisione in 3 bande con filtri Linkwitz-Riley a 4 poli. Il ramo alto passa anche da un all-pass a xo1 (LP+HP), cosi' le tre
// bande risommate hanno risposta piatta in ampiezza. Ogni banda ha un compressore feed-forward con detector stereo collegato.
void BuiltinFx::doMultiband (juce::AudioBuffer<float>& b)
{
    const float x1 = juce::jmin (get (0), (float) (rate * 0.2));
    const float x2 = juce::jmin (juce::jmax (get (1), x1 * 1.5f), (float) (rate * 0.45));
    if (x1 != mbXo1 || x2 != mbXo2)
    {
        mbXo1 = x1; mbXo2 = x2;
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 2; ++s)
            {
                mbf[0][(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeLowPass  (rate, x2, 0.7071));   // LP2
                mbf[1][(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeHighPass (rate, x2, 0.7071));   // HP2
                mbf[2][(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeLowPass  (rate, x1, 0.7071));   // LP1 (basso)
                mbf[3][(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeHighPass (rate, x1, 0.7071));   // HP1 (medio)
                mbf[4][(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeLowPass  (rate, x1, 0.7071));   // all-pass sull'alto
                mbf[5][(size_t) c][(size_t) s].setCoefficients (juce::IIRCoefficients::makeHighPass (rate, x1, 0.7071));
            }
    }
    const float atk = std::exp (-1.0f / (get (2) * 0.001f * (float) rate)), rel = std::exp (-1.0f / (get (3) * 0.001f * (float) rate));
    const float outG = dbToGain (get (4));
    float thr[3], slope[3], gain[3];
    for (int k = 0; k < 3; ++k)
    {
        thr[k] = get (5 + 3 * k);
        slope[k] = 1.0f - 1.0f / get (6 + 3 * k);
        gain[k] = dbToGain (get (7 + 3 * k));
    }
    auto lr = [this] (int f, int c, float v) { return mbf[(size_t) f][(size_t) c][1].processSingleSampleRaw (mbf[(size_t) f][(size_t) c][0].processSingleSampleRaw (v)); };

    float* d[2] = { b.getWritePointer (0), b.getWritePointer (1) };
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        float band[3][2];
        for (int c = 0; c < 2; ++c)
        {
            const float x = d[c][i];
            const float lowMid = lr (0, c, x), hi = lr (1, c, x);
            band[0][c] = lr (2, c, lowMid);
            band[1][c] = lr (3, c, lowMid);
            band[2][c] = lr (4, c, hi) + lr (5, c, hi);
        }
        for (int k = 0; k < 3; ++k)
        {
            const float lvl = gainToDb (juce::jmax (std::abs (band[k][0]), std::abs (band[k][1])));
            const float target = juce::jmax (0.0f, (lvl - thr[k]) * slope[k]);
            mbGr[(size_t) k] = target + (mbGr[(size_t) k] - target) * (target > mbGr[(size_t) k] ? atk : rel);
        }
        for (int c = 0; c < 2; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += band[k][c] * dbToGain (-mbGr[(size_t) k]) * gain[k];
            d[c][i] = sum * outG;
        }
    }
    for (size_t k = 0; k < 3; ++k) meters[k] = mbGr[k];
}

void BuiltinFx::doReverb (juce::AudioBuffer<float>& b)
{
    const float mix = get (3) * 0.01f;
    juce::Reverb::Parameters p;
    p.roomSize = get (0);
    p.damping = get (1);
    p.width = get (2);
    p.wetLevel = mix * 0.5f;           // juce::Reverb moltiplica dry per 2 e wet per 3
    p.dryLevel = (1.0f - mix) * 0.5f;
    p.freezeMode = 0.0f;
    reverb.setParameters (p);
    reverb.processStereo (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples());
}

void BuiltinFx::doDelay (juce::AudioBuffer<float>& b)
{
    if (ring[0].empty()) return;
    const int n = (int) ring[0].size();
    const float target = juce::jlimit (1.0f, (float) (n - 2), get (0) * 0.001f * (float) rate);
    const float fb = get (1) * 0.01f, mix = get (2) * 0.01f;
    const bool ping = getBool (3);
    const float damp = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 6000.0f / (float) rate);   // passa-basso nel feedback
    if (delayNow < 0.0f) delayNow = target;
    float* L = b.getWritePointer (0);
    float* R = b.getWritePointer (1);
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        delayNow += (target - delayNow) * 0.0005f;   // il tempo scivola: niente scatti quando si gira la manopola
        const float wl = readFrac (ring[0], ringPos, delayNow), wr = readFrac (ring[1], ringPos, delayNow);
        fbLp[0] += damp * (wl - fbLp[0]);
        fbLp[1] += damp * (wr - fbLp[1]);
        const float inL = L[i], inR = R[i];
        if (ping) { ring[0][(size_t) ringPos] = 0.5f * (inL + inR) + fb * fbLp[1]; ring[1][(size_t) ringPos] = fb * fbLp[0]; }
        else      { ring[0][(size_t) ringPos] = inL + fb * fbLp[0];               ring[1][(size_t) ringPos] = inR + fb * fbLp[1]; }
        L[i] = inL + mix * wl;
        R[i] = inR + mix * wr;
        if (++ringPos == n) ringPos = 0;
    }
}

// shortcut: nessun sovracampionamento, con drive alto compare un po' di aliasing; da aggiungere se serve.
void BuiltinFx::doSaturator (juce::AudioBuffer<float>& b)
{
    const float g = dbToGain (get (0)), norm = 1.0f / std::tanh (g), mix = get (2) * 0.01f, outG = dbToGain (get (3));
    const float tone = juce::jmin (get (1), (float) (rate * 0.45));
    if (tone != lastLp)
    {
        lastLp = tone;
        for (int c = 0; c < 2; ++c) lp[(size_t) c][0].setCoefficients (juce::IIRCoefficients::makeLowPass (rate, tone, 0.7071));
    }
    const bool useTone = get (1) < 19500.0f;
    for (int c = 0; c < 2; ++c)
    {
        float* d = b.getWritePointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            float wet = std::tanh (g * d[i]) * norm;
            if (useTone) wet = lp[(size_t) c][0].processSingleSampleRaw (wet);
            d[i] = ((1.0f - mix) * d[i] + mix * wet) * outG;
        }
    }
}

void BuiltinFx::doChorus (juce::AudioBuffer<float>& b)
{
    if (ring[0].empty()) return;
    const int n = (int) ring[0].size();
    const float hz = get (0), depth = get (1) * 0.001f * (float) rate, mix = get (2) * 0.01f;
    const float base = 0.014f * (float) rate;   // ritardo centrale 14 ms
    const double inc = (double) hz / rate;
    float* L = b.getWritePointer (0);
    float* R = b.getWritePointer (1);
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float lfoL = std::sin ((float) (juce::MathConstants<double>::twoPi * lfoPhase));
        const float lfoR = std::sin ((float) (juce::MathConstants<double>::twoPi * lfoPhase) + juce::MathConstants<float>::halfPi);   // canali sfasati di 90 gradi
        const float wl = readFrac (ring[0], ringPos, base + depth * lfoL), wr = readFrac (ring[1], ringPos, base + depth * lfoR);
        ring[0][(size_t) ringPos] = L[i];
        ring[1][(size_t) ringPos] = R[i];
        L[i] = L[i] * (1.0f - 0.5f * mix) + wl * 0.5f * mix;
        R[i] = R[i] * (1.0f - 0.5f * mix) + wr * 0.5f * mix;
        if (++ringPos == n) ringPos = 0;
        lfoPhase += inc;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
    }
}
} // namespace fc
