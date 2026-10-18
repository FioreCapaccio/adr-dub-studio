#pragma once

#include "Leveler.h"

namespace fc::level
{
/** Lavoro in un thread con barra di avanzamento e Annulla. */
class Job : public juce::ThreadWithProgressWindow
{
public:
    Job (Request r, std::function<void (Outcome)> done);
    void run() override;
    void threadComplete (bool) override { if (done) done (outcome); }

private:
    Request req;
    std::function<void (Outcome)> done;
    Outcome outcome;
};

/** Testo del risultato: per ogni traccia il livello prima e dopo. */
juce::String summary (const std::vector<TrackCurve>&, int clamped);
} // namespace fc::level
