#pragma once

#include <JuceHeader.h>

namespace fc
{
/** Formato di timecode (come nei menu di Pro Tools). */
struct TcFormat
{
    const char* name;
    double fps;        // frequenza reale dei fotogrammi
    int nominal;       // fotogrammi per secondo "contati" (24, 25, 30...)
    bool drop;         // drop-frame (solo 29,97)
};

inline const TcFormat* tcFormats()
{
    static const TcFormat f[] = {
        { "23,976 fps",             24000.0 / 1001.0, 24, false },
        { "24 fps",                 24.0,             24, false },
        { "25 fps",                 25.0,             25, false },
        { "29,97 fps (non drop)",   30000.0 / 1001.0, 30, false },
        { "29,97 fps (drop frame)", 30000.0 / 1001.0, 30, true  },
        { "30 fps",                 30.0,             30, false },
        { "50 fps",                 50.0,             50, false },
        { "60 fps",                 60.0,             60, false }
    };
    return f;
}
constexpr int numTcFormats = 8;

/** Indice del formato piu' vicino a una frequenza di fotogrammi (per i video appena importati). */
inline int tcFormatForFps (double fps)
{
    int best = 2;
    double bestDiff = 1.0e9;
    for (int i = 0; i < numTcFormats; ++i)
    {
        if (tcFormats()[i].drop) continue;   // il drop-frame si sceglie a mano
        const double d = std::abs (tcFormats()[i].fps - fps);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

/** Secondi -> numero di fotogramma (arrotondato per difetto, con una tolleranza). */
inline int64_t secondsToFrames (double seconds, const TcFormat& f)
{
    return (int64_t) std::floor (juce::jmax (0.0, seconds) * f.fps + 1.0e-4);
}

inline double framesToSeconds (int64_t frames, const TcFormat& f) { return (double) frames / f.fps; }

/** Numero di fotogramma -> "hh:mm:ss:ff" (con ';' prima dei fotogrammi nel drop-frame). */
inline juce::String framesToTimecode (int64_t frames, const TcFormat& f)
{
    if (frames < 0) frames = 0;
    if (f.drop)
    {
        // algoritmo standard del drop-frame a 29,97: si saltano 2 numeri al minuto, tranne ogni decimo minuto
        const int64_t d = frames / 17982, m = frames % 17982;
        frames += 18 * d + (m < 2 ? 0 : 2 * ((m - 2) / 1798));
    }
    const int nom = f.nominal;
    const int ff = (int) (frames % nom);
    const int64_t total = frames / nom;
    return juce::String::formatted (f.drop ? "%02d:%02d:%02d;%02d" : "%02d:%02d:%02d:%02d",
                                    (int) (total / 3600), (int) ((total / 60) % 60), (int) (total % 60), ff);
}

inline juce::String secondsToTimecode (double seconds, const TcFormat& f)
{
    return framesToTimecode (secondsToFrames (seconds, f), f);
}

/** "hh:mm:ss:ff" (o con ';' o '.') -> numero di fotogramma. Ritorna false se il testo non e' un timecode. */
inline bool timecodeToFrames (const juce::String& text, const TcFormat& f, int64_t& frames)
{
    const auto t = text.trim().replaceCharacter (';', ':').replaceCharacter ('.', ':').replaceCharacter (',', ':');
    auto parts = juce::StringArray::fromTokens (t, ":", "");
    parts.removeEmptyStrings();
    if (parts.size() != 4) return false;
    for (auto& p : parts) if (! p.containsOnly ("0123456789")) return false;
    const int64_t h = parts[0].getLargeIntValue(), m = parts[1].getLargeIntValue(), s = parts[2].getLargeIntValue(), ff = parts[3].getLargeIntValue();
    if (m > 59 || s > 59 || ff >= f.nominal) return false;
    int64_t n = ((h * 3600 + m * 60 + s) * f.nominal) + ff;
    if (f.drop)
    {
        const int64_t totalMinutes = h * 60 + m;
        n -= 2 * (totalMinutes - totalMinutes / 10);
    }
    frames = n;
    return true;
}

inline bool timecodeToSeconds (const juce::String& text, const TcFormat& f, double& seconds)
{
    int64_t fr = 0;
    if (! timecodeToFrames (text, f, fr)) return false;
    seconds = framesToSeconds (fr, f);
    return true;
}

} // namespace fc
