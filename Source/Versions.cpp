#include "Versions.h"

namespace fc::versions
{
juce::File folderFor (const juce::File& f) { return f.getParentDirectory().getChildFile (f.getFileNameWithoutExtension() + "_versioni"); }

namespace
{
int numberOf (const juce::String& name)
{
    const int p = name.lastIndexOf ("_v");
    if (p < 0) return 0;
    const auto digits = name.substring (p + 2).initialSectionContainingOnly ("0123456789");
    return digits.getIntValue();
}
} // namespace

std::vector<Entry> list (const juce::File& sessionFile)
{
    std::vector<Entry> out;
    if (sessionFile == juce::File()) return out;
    for (const auto& e : juce::RangedDirectoryIterator (folderFor (sessionFile), false, "*.adb;*.arostudio", juce::File::findFiles))
    {
        Entry v;
        v.file = e.getFile();
        v.number = numberOf (v.file.getFileNameWithoutExtension());
        v.time = v.file.getLastModificationTime();
        const auto note = v.file.withFileExtension ("txt");
        if (note.existsAsFile()) v.note = note.loadFileAsString().trim();
        out.push_back (v);
    }
    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.number != b.number ? a.number > b.number : a.time > b.time; });
    return out;
}

juce::File save (const Project& p, const juce::String& note, juce::String& error)
{
    const auto file = p.getFile();
    if (file == juce::File()) { error = "Salva prima la sessione: le versioni stanno in una cartella accanto al file."; return {}; }
    const auto dir = folderFor (file);
    dir.createDirectory();
    int next = 1;
    for (auto& v : list (file)) next = juce::jmax (next, v.number + 1);
    const auto name = file.getFileNameWithoutExtension() + "_v" + juce::String (next).paddedLeft ('0', 3) + "_" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M");
    const auto dest = dir.getChildFile (name + ".adb");
    if (! p.saveCopy (dest, error)) return {};
    if (note.trim().isNotEmpty()) dest.withFileExtension ("txt").replaceWithText (note.trim());
    return dest;
}

bool remove (const Entry& e)
{
    e.file.withFileExtension ("txt").deleteFile();
    return e.file.deleteFile();
}
} // namespace fc::versions
