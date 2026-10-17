#include "Manual.h"
#include "ManualData.h"

namespace fc::manual
{
bool open (const juce::String& lang, juce::String& error)
{
    const char* data = nullptr;
    int size = 0;
    if (lang == "en") { data = ManualData::manual_en_zip; size = ManualData::manual_en_zipSize; }
    else               { data = ManualData::manual_it_zip; size = ManualData::manual_it_zipSize; }

    const auto version = juce::JUCEApplicationBase::getInstance() != nullptr ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion() : juce::String ("x");
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ADRDubStudioManual-" + version + "-" + (lang == "en" ? "en" : "it"));
    const auto index = dir.getChildFile ("index.html");
    if (! index.existsAsFile())
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        juce::ZipFile zip (in);
        dir.deleteRecursively();
        dir.createDirectory();
        if (zip.uncompressTo (dir).failed() || ! index.existsAsFile()) { error = "Manuale non disponibile."; return false; }
    }
    return juce::URL (index).launchInDefaultBrowser();
}
} // namespace fc::manual
