#pragma once

#include "Model.h"
#include "Lang.h"

namespace fc
{
/** Ruoli di traccia per il doppiaggio, ciascuno con una catena di partenza: EQ a 4 bande, compressore, limiter e livello.
    Sono valori di partenza di uso comune nel mix per il cinema e la TV (pulizia dei bassi, spazio per la voce, dinamica controllata):
    vanno sempre ritoccati a orecchio sul materiale. */
enum TrackType { TrackNone = 0, TrackDialogue, TrackME, TrackEffects, TrackGuide, kNumTrackTypes };   // gli indici sono salvati nelle sessioni: non cambiare l'ordine

inline juce::String trackTypeName (int t) { static const char* n[] = { TRX ("Nessuno"), TRX ("Dialoghi"), TRX ("M&E"), TRX ("Effetti"), TRX ("Guida") }; return juce::translate (n[juce::jlimit (0, kNumTrackTypes - 1, t)]); }
inline juce::String trackTypeTag (int t) { static const char* n[] = { "", TRX ("DIALOGHI"), TRX ("M&E"), TRX ("EFFETTI"), TRX ("GUIDA") }; return juce::translate (n[juce::jlimit (0, kNumTrackTypes - 1, t)]); }
inline juce::Colour trackTypeColour (int t) { static const juce::uint32 c[] = { 0xff848b97, 0xff4aa8ff, 0xffb58cff, 0xffffc94d, 0xff6fbf73 }; return juce::Colour (c[juce::jlimit (0, kNumTrackTypes - 1, t)]); }

inline juce::String trackPresetDescription (int t)
{
    switch (t)
    {
        case TrackDialogue: return TRANS ("Passa-alto 80 Hz, -2,5 dB a 250 Hz (corpo e rimbombo), +2,5 dB a 3,5 kHz (intelligibilita'), +1,5 dB di aria sopra i 10 kHz. "
                                   "Compressore 3:1, soglia -20 dB, attacco 10 ms, rilascio 120 ms. Limiter a -3 dB.");
        case TrackME:       return TRANS ("Musica ed effetti (M&E) senza dialoghi. Passa-alto 30 Hz, -1,5 dB a 300 Hz, -2 dB a 2,8 kHz (fa spazio ai dialoghi), +1 dB di aria. "
                                   "Compressore delicato 2:1, soglia -18 dB, attacco 30 ms, rilascio 200 ms. Limiter a -1 dB. Livello -6 dB.");
        case TrackEffects:  return TRANS ("Passa-alto 40 Hz, -1,5 dB a 350 Hz, +1,5 dB a 5 kHz (presenza), +1 dB di aria. "
                                   "Compressore 4:1, soglia -14 dB, attacco 5 ms, rilascio 80 ms. Limiter a -2 dB. Livello -3 dB.");
        case TrackGuide:    return TRANS ("Dialogo originale di riferimento: nessuna elaborazione, livello -6 dB, bus Guida (escluso dalle consegne).");
        default:            return TRANS ("Nessuna elaborazione: EQ, compressore e limiter spenti, livello a 0 dB.");
    }
}

/** Imposta la catena della traccia secondo il tipo. Sovrascrive EQ, compressore, limiter e fader; non tocca pan, send, inserti VST3. */
inline void applyTrackPreset (TrackLive& l, int type)
{
    type = juce::jlimit (0, kNumTrackTypes - 1, type);
    struct B { int type; float f, g, q; };
    struct P { B b[4]; bool comp; float thr, ratio, atk, rel, knee, makeup; float ceiling, volume; };
    static const P presets[] = {
        // nessuno
        { { { ParamEq::LowShelf, 100, 0, 0.7f }, { ParamEq::Bell, 500, 0, 1.0f }, { ParamEq::Bell, 2500, 0, 1.0f }, { ParamEq::HighShelf, 8000, 0, 0.7f } }, false, -18, 3, 10, 120, 6, 0, -1.0f, 0.0f },
        // dialogo
        { { { ParamEq::HighPass, 80, 0, 0.707f }, { ParamEq::Bell, 250, -2.5f, 1.2f }, { ParamEq::Bell, 3500, 2.5f, 1.0f }, { ParamEq::HighShelf, 10000, 1.5f, 0.7f } }, true, -20, 3, 10, 120, 6, 2.0f, -3.0f, 0.0f },
        // musica
        { { { ParamEq::HighPass, 30, 0, 0.707f }, { ParamEq::Bell, 300, -1.5f, 0.8f }, { ParamEq::Bell, 2800, -2.0f, 0.9f }, { ParamEq::HighShelf, 9000, 1.0f, 0.7f } }, true, -18, 2, 30, 200, 8, 1.0f, -1.0f, -6.0f },
        // effetti
        { { { ParamEq::HighPass, 40, 0, 0.707f }, { ParamEq::Bell, 350, -1.5f, 1.0f }, { ParamEq::Bell, 5000, 1.5f, 0.8f }, { ParamEq::HighShelf, 10000, 1.0f, 0.7f } }, true, -14, 4, 5, 80, 4, 1.0f, -2.0f, -3.0f },
        // guida: nessuna elaborazione, piu' bassa del resto
        { { { ParamEq::LowShelf, 100, 0, 0.7f }, { ParamEq::Bell, 500, 0, 1.0f }, { ParamEq::Bell, 2500, 0, 1.0f }, { ParamEq::HighShelf, 8000, 0, 0.7f } }, false, -18, 3, 10, 120, 6, 0, -1.0f, -6.0f },
    };
    const auto& p = presets[type];
    for (int i = 0; i < ParamEq::kBands; ++i) l.eq.setBand (i, p.b[i].type, p.b[i].f, p.b[i].g, p.b[i].q);
    l.eq.enabled = type != TrackNone && type != TrackGuide;
    l.comp.enabled = p.comp;
    l.comp.thresholdDb = p.thr; l.comp.ratio = p.ratio; l.comp.attackMs = p.atk; l.comp.releaseMs = p.rel; l.comp.kneeDb = p.knee; l.comp.makeupDb = p.makeup;
    l.limiterOn = type != TrackNone && type != TrackGuide;
    l.ceilingDb = p.ceiling;
    l.volumeDb = p.volume;
    l.trackType = type;
}

} // namespace fc

namespace fc
{
/** Bus di gruppo predefiniti per ruolo: Dialoghi, M&E, Effetti e Guida hanno ciascuno il proprio bus (1..4), cosi' gli stem escono separati. */
inline juce::String groupBusName (int k) { static const char* n[] = { TRX ("DIALOGHI"), TRX ("M&E"), TRX ("EFFETTI"), TRX ("GUIDA") }; return juce::translate (n[juce::jlimit (0, 3, k)]); }
inline int roleOutBus (int type)
{
    switch (type) { case TrackDialogue: return 1; case TrackME: return 2; case TrackEffects: return 3; case TrackGuide: return 4; default: return 0; }
}
} // namespace fc
