#include "LevelerUi.h"

namespace fc::level
{
Job::Job (Request r, std::function<void (Outcome)> d)
    : juce::ThreadWithProgressWindow (TRANS ("Livello del volume: analisi..."), true, true), req (std::move (r)), done (std::move (d)) {}

void Job::run()
{
    outcome = process (req, [this] (double p) { setProgress (p); return ! threadShouldExit(); }, [this] (const juce::String& m) { setStatusMessage (m); });
}

juce::String summary (const std::vector<TrackCurve>& curves, int clamped)
{
    juce::String s;
    for (auto& c : curves)
        s << c.trackName << ": " << TRANS ("livello mediano") << " " << juce::String (c.plan.speechMedian, 1) << " LUFS, "
          << TRANS ("variazione da") << " +/-" << juce::String (c.plan.spreadBefore, 1) << " " << TRANS ("a") << " +/-" << juce::String (c.plan.spreadAfter, 1) << " dB, "
          << (int) c.points.size() << " " << TRANS ("punti") << "\n";
    if (clamped > 0) s << "\n" << juce::String (clamped) << " " << TRANS ("punti sono stati limitati dal massimo della corsia (+6 dB): abbassa il fader della traccia e rifai.") << "\n";
    s << "\n" << TRANS ("Le tracce sono in modalita' lettura: la curva si vede e si modifica nella corsia VOL. Per togliere tutto: Modifica > Annulla.");
    return s;
}
} // namespace fc::level
