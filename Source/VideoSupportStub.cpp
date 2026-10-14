#include "VideoSupport.h"

// Versione senza video (Windows, Linux): il resto dell'app funziona, ma i video non si possono importare ne' esportare.
namespace fc::video
{
bool isVideoFile (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".mov" || ext == ".mp4" || ext == ".m4v";
}

Info probe (const juce::File&)
{
    Info i;
    i.error = "Il video non e' ancora supportato su questa piattaforma.";
    return i;
}

std::vector<juce::Image> thumbnails (const juce::File&, double, int, int) { return {}; }

bool muxAudio (const juce::File&, const juce::File&, const juce::File&, double, const std::function<bool (double)>&, juce::String& error, double, double)
{
    error = "L'esportazione del video non e' ancora supportata su questa piattaforma.";
    return false;
}
} // namespace fc::video
