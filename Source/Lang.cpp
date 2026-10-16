#include "Lang.h"
#include "Settings.h"
#include "LangData.h"

namespace fc::lang
{
const std::vector<Language>& languages()
{
    static const std::vector<Language> list = { { "it", "Italiano" }, { "en", "English" } };
    return list;
}

juce::String savedCode() { return settings().getValue ("language", "it"); }

void saveCode (const juce::String& code)
{
    settings().setValue ("language", code);
    settings().saveIfNeeded();
}

juce::String catalogText (const juce::String& code)
{
    if (code == "en") return juce::String::fromUTF8 (LangData::en_txt, LangData::en_txtSize);
    return {};
}

juce::String translateTo (const juce::String& code, const juce::String& italian)
{
    const auto text = catalogText (code);
    if (text.isEmpty()) return italian;
    juce::LocalisedStrings table (text, false);
    return table.translate (italian);
}

void apply (const juce::String& code)
{
    const auto text = catalogText (code);
    if (text.isEmpty()) { juce::LocalisedStrings::setCurrentMappings (nullptr); return; }   // italiano: i testi nel codice
    juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (text, false));
}
} // namespace fc::lang
