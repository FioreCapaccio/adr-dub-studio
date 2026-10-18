#include "Manual.h"
#include "ManualData.h"
#include "Theme.h"

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

#if JUCE_WEB_BROWSER && JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE
namespace
{
juce::String mimeFor (const juce::String& name)
{
    const auto e = name.fromLastOccurrenceOf (".", false, false).toLowerCase();
    if (e == "html" || e == "htm") return "text/html";
    if (e == "css") return "text/css";
    if (e == "js") return "text/javascript";
    if (e == "png") return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "svg") return "image/svg+xml";
    if (e == "json") return "application/json";
    return "application/octet-stream";
}

class ManualWindow : public juce::DocumentWindow
{
public:
    explicit ManualWindow (const juce::String& title) : DocumentWindow (title, fc::theme::bg, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setResizeLimits (640, 420, 4000, 3000);
    }
    void closeButtonPressed() override { setVisible (false); }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createWindow (const juce::String& lang)
{
    const bool en = lang == "en";
    const char* data = en ? ManualData::manual_en_zip : ManualData::manual_it_zip;
    const int size = en ? ManualData::manual_en_zipSize : ManualData::manual_it_zipSize;

    // tutto il manuale (circa 3 MB) in memoria: il fornitore delle risorse puo' essere chiamato da piu' thread, qui si legge soltanto
    auto files = std::make_shared<std::map<juce::String, std::vector<std::byte>>>();
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        juce::ZipFile zip (in);
        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto* e = zip.getEntry (i);
            if (e == nullptr || e->filename.endsWithChar ('/')) continue;
            std::unique_ptr<juce::InputStream> s (zip.createStreamForEntry (i));
            if (s == nullptr) continue;
            juce::MemoryBlock mb;
            s->readIntoMemoryBlock (mb);
            auto* p = static_cast<const std::byte*> (mb.getData());
            (*files)["/" + e->filename] = std::vector<std::byte> (p, p + mb.getSize());
        }
    }
    if (files->find ("/index.html") == files->end()) return nullptr;

    auto options = juce::WebBrowserComponent::Options()
        .withResourceProvider ([files] (const juce::String& path) -> std::optional<juce::WebBrowserComponent::Resource>
        {
            auto key = path.upToFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf ("#", false, false);
            if (key.isEmpty() || key == "/") key = "/index.html";
            const auto it = files->find (juce::URL::removeEscapeChars (key));
            if (it == files->end()) return std::nullopt;
            return juce::WebBrowserComponent::Resource { it->second, mimeFor (it->first) };
        });
#if JUCE_WINDOWS
    options = options.withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                     .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2()
                                                  .withUserDataFolder (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ArotronStudio/webview")));
#endif
    if (! juce::WebBrowserComponent::areOptionsSupported (options)) return nullptr;

    auto* browser = new juce::WebBrowserComponent (options);
    browser->setSize (1100, 780);
    auto w = std::make_unique<ManualWindow> (en ? "ADR Dub Studio - User manual" : juce::String (juce::CharPointer_UTF8 ("ADR Dub Studio - Manuale utente")));
    w->setContentOwned (browser, true);
    browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    w->centreWithSize (1100, 780);
    w->setVisible (true);
    return w;
}
#else
std::unique_ptr<juce::DocumentWindow> createWindow (const juce::String&) { return nullptr; }
#endif
} // namespace fc::manual
