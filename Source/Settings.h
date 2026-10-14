#pragma once

#include <JuceHeader.h>

namespace fc
{
/** Preferenze dell'app (~/Library/Application Support/ArotronStudio/ArotronStudio.settings). */
inline juce::PropertiesFile& settings()
{
    static std::unique_ptr<juce::PropertiesFile> file = []
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "ArotronStudio";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        return std::make_unique<juce::PropertiesFile> (o);
    }();
    return *file;
}
} // namespace fc
