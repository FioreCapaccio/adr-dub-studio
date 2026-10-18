#include "License.h"
#include "Settings.h"
#include "EulaData.h"

extern "C"
{
#include "third_party/monocypher/monocypher-ed25519.h"
}

namespace fc::license
{
namespace
{
// Meta' pubblica (SPKI) della coppia Ed25519 di ADR Studio Suite: la stessa chiave firma anche queste licenze.
constexpr const char* kPublicKeySpkiB64 = "MCowBQYDK2VwAyEA9BIm8/AbK4JZNyrf4kCAjrRpISjdvgLqjhv492nbA3M=";
constexpr int64_t kRollbackToleranceMs = 5 * 60 * 1000;
constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

bool decodeBase64 (const juce::String& text, juce::MemoryBlock& out)
{
    juce::MemoryOutputStream mo (out, false);
    return juce::Base64::convertFromBase64 (mo, text.trim());
}

// come JSON.stringify di JavaScript: solo " \ e i caratteri di controllo vengono scritti con la barra, il resto resta UTF-8
juce::String jsString (const juce::String& s)
{
    juce::String r = "\"";
    for (auto p = s.getCharPointer(); ! p.isEmpty();)
    {
        const juce::juce_wchar c = p.getAndAdvance();
        switch (c)
        {
            case '"':  r << "\\\""; break;
            case '\\': r << "\\\\"; break;
            case '\b': r << "\\b"; break;
            case '\f': r << "\\f"; break;
            case '\n': r << "\\n"; break;
            case '\r': r << "\\r"; break;
            case '\t': r << "\\t"; break;
            default:
                if (c < 0x20) r << "\\u" << juce::String::toHexString ((int) c).paddedLeft ('0', 4);
                else r << juce::String::charToString (c);
        }
    }
    return r << "\"";
}

juce::File dataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    d = d.getChildFile ("Application Support");
#endif
    d = d.getChildFile ("ArotronStudio");
    d.createDirectory();
    return d;
}

juce::File anchorFile()
{
    auto h = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
#if JUCE_MAC
    h = h.getChildFile ("Library/Caches/ArotronStudio");
#else
    h = h.getChildFile ("AppData/Local/ArotronStudio");
#endif
    h.createDirectory();
    return h.getChildFile (".adrdub");
}

juce::File licenseFile() { return dataDir().getChildFile ("license.lic"); }

// ─── orologio della prova: data di installazione + ultimo istante visto, in due file con un sigillo ───
struct ClockRec { int64_t install = 0, last = 0; bool ok = false; };

juce::String sealOf (int64_t install, int64_t last)
{
    const auto s = "adr-dub-trial|" + machineId() + "|" + juce::String (install) + "|" + juce::String (last);
    return juce::SHA256 (s.toUTF8(), (size_t) s.getNumBytesAsUTF8()).toHexString();
}

ClockRec readRec (const juce::File& f)
{
    ClockRec r;
    if (! f.existsAsFile()) return r;
    const auto lines = juce::StringArray::fromLines (f.loadFileAsString());
    if (lines.size() < 3) return r;
    r.install = lines[0].getLargeIntValue();
    r.last = lines[1].getLargeIntValue();
    r.ok = r.install > 0 && r.last >= r.install && lines[2].trim() == sealOf (r.install, r.last);
    return r;
}

void writeRec (const juce::File& f, int64_t install, int64_t last)
{
    f.replaceWithText (juce::String (install) + "\n" + juce::String (last) + "\n" + sealOf (install, last) + "\n");
}

/** Aggiorna i due file e restituisce {data di installazione, "adesso" a prova di orologio riportato indietro}. */
std::pair<int64_t, int64_t> advanceClock (int64_t clockNow)
{
    const auto a = readRec (dataDir().getChildFile ("install.dat")), b = readRec (anchorFile());
    int64_t install = clockNow, last = clockNow;
    if (a.ok || b.ok)
    {
        install = a.ok && b.ok ? juce::jmin (a.install, b.install) : (a.ok ? a.install : b.install);   // la piu' vecchia: accorcia la prova di chi la forgia
        last = juce::jmax (a.ok ? a.last : 0, b.ok ? b.last : 0);
    }
    const auto eff = ratchetNow (clockNow, last);
    last = juce::jmax (last, eff);
    writeRec (dataDir().getChildFile ("install.dat"), install, last);
    writeRec (anchorFile(), install, last);
    return { install, eff };
}

Verdict fail (const char* code, const juce::String& reason)
{
    Verdict v;
    v.code = code;
    v.reason = reason;
    return v;
}
} // namespace

juce::String machineId()
{
    static const juce::String id = []
    {
        juce::String raw;
#if JUCE_MAC
        juce::ChildProcess p;
        if (p.start (juce::StringArray { "/usr/sbin/ioreg", "-rd1", "-c", "IOPlatformExpertDevice" }))
        {
            const auto out = p.readAllProcessOutput();
            const auto tail = out.fromFirstOccurrenceOf ("\"IOPlatformUUID\" = \"", false, false);
            if (tail != out) raw = tail.upToFirstOccurrenceOf ("\"", false, false);
        }
#elif JUCE_WINDOWS
        raw = juce::WindowsRegistry::getValue ("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography\\MachineGuid", {}, juce::WindowsRegistry::WoW64_64bit);
#else
        raw = juce::File ("/etc/machine-id").loadFileAsString().trim();
#endif
        raw = raw.trim().toUpperCase();
        if (raw.isEmpty()) raw = "UNKNOWN|" + juce::SystemStats::getComputerName();   // ripiego: meglio un ID instabile che nessun ID
        return juce::SHA256 (raw.toUTF8(), (size_t) raw.getNumBytesAsUTF8()).toHexString().substring (0, 32).toUpperCase();
    }();
    return id;
}

juce::String buyUrl()
{
    return "https://adr-suite.com/buy.html?tier=dubstudio&lang=" + (settings().getValue ("language", "it") == "en" ? juce::String ("en") : juce::String ("it"))
           + "&machineId=" + machineId();
}

juce::String canonicalPayload (const juce::var& p)
{
    juce::String r = "{";
    bool first = true;
    auto add = [&] (const char* key, const juce::String& value)
    {
        if (! first) r << ",";
        first = false;
        r << jsString (key) << ":" << value;
    };
    for (auto key : { "licenseId", "issuedTo", "machineId", "tier" })
        if (p.hasProperty (key)) add (key, jsString (p[key].toString()));
    if (p.hasProperty ("features"))
    {
        juce::String arr = "[";
        if (auto* a = p["features"].getArray())
            for (int i = 0; i < a->size(); ++i) arr << (i ? "," : "") << jsString ((*a)[i].toString());
        add ("features", arr << "]");
    }
    if (p.hasProperty ("issuedAt")) add ("issuedAt", jsString (p["issuedAt"].toString()));
    const auto exp = p["expiresAt"];
    add ("expiresAt", exp.isString() && exp.toString().isNotEmpty() ? jsString (exp.toString()) : "null");
    return r << "}";
}

Verdict verifyKey (const juce::String& key, const juce::String& machine, int64_t nowMs)
{
    juce::MemoryBlock raw;
    juce::var stored;
    if (! decodeBase64 (key, raw) || raw.getSize() == 0
        || ! juce::JSON::parse (raw.toString(), stored).wasOk() || ! stored.isObject())
        return fail ("FILE_CORRUPTED", TRANS ("Formato licenza non valido"));

    const auto payload = stored["payload"];
    const auto sig64 = stored["signature"].toString();
    if (! payload.isObject() || sig64.isEmpty()) return fail ("FILE_CORRUPTED", TRANS ("Struttura licenza non valida"));
    if (payload["machineId"].toString().isEmpty()) return fail ("FILE_CORRUPTED", TRANS ("Campo machineId mancante o non valido"));
    if (payload["features"].getArray() == nullptr) return fail ("FILE_CORRUPTED", TRANS ("Campo features mancante o non valido"));
    const auto tier = payload["tier"].toString();
    if (tier != "light" && tier != "full") return fail ("FILE_CORRUPTED", TRANS ("Campo tier non valido"));

    // la firma prima di tutto il resto: una licenza modificata a mano non deve passare solo perche' ha la forma giusta
    juce::MemoryBlock sig, spki;
    decodeBase64 (sig64, sig);
    decodeBase64 (kPublicKeySpkiB64, spki);
    const auto canonical = canonicalPayload (payload);
    if (sig.getSize() != 64 || spki.getSize() < 32
        || crypto_ed25519_check (static_cast<const uint8_t*> (sig.getData()),
                                 static_cast<const uint8_t*> (spki.getData()) + spki.getSize() - 32,
                                 reinterpret_cast<const uint8_t*> (canonical.toRawUTF8()), canonical.getNumBytesAsUTF8()) != 0)
        return fail ("INVALID_SIGNATURE", TRANS ("Firma della licenza non valida"));

    if (payload["machineId"].toString() != machine) return fail ("MACHINE_MISMATCH", TRANS ("Licenza non valida per questo computer"));

    Payload pl;
    pl.licenseId = payload["licenseId"].toString();
    pl.issuedTo = payload["issuedTo"].toString();
    pl.machineId = payload["machineId"].toString();
    pl.tier = tier;
    pl.issuedAt = payload["issuedAt"].toString();
    pl.expiresAt = payload["expiresAt"].isString() ? payload["expiresAt"].toString() : juce::String();
    for (auto& f : *payload["features"].getArray()) pl.features.add (f.toString());

    if (! pl.features.contains (kFeature))
    {
        auto v = fail ("WRONG_PRODUCT", TRANS ("Questa licenza non e' per ADR Dub Studio"));
        v.payload = pl;
        return v;
    }
    if (pl.expiresAt.isNotEmpty())
    {
        const auto t = juce::Time::fromISO8601 (pl.expiresAt);
        if (t.toMilliseconds() == 0) return fail ("FILE_CORRUPTED", TRANS ("Data di scadenza non valida"));
        if (t.toMilliseconds() < nowMs)
        {
            auto v = fail ("EXPIRED", TRANS ("Licenza scaduta"));
            v.payload = pl;
            return v;
        }
    }
    Verdict v;
    v.valid = true;
    v.payload = pl;
    return v;
}

int trialDaysLeft (int64_t installMs, int64_t nowMs)
{
    const auto elapsed = (int) juce::jmax<int64_t> (0, (nowMs - installMs) / kDayMs);
    return juce::jmax (0, kTrialDays - elapsed);
}

int64_t ratchetNow (int64_t clockMs, int64_t lastSeenMs)
{
    return lastSeenMs - clockMs > kRollbackToleranceMs ? lastSeenMs : clockMs;
}

Status current()
{
    const auto [install, now] = advanceClock (juce::Time::currentTimeMillis());
    Status s;
    const auto f = licenseFile();
    if (f.existsAsFile())
    {
        const auto v = verifyKey (f.loadFileAsString(), machineId(), now);
        s.issuedTo = v.payload.issuedTo;
        s.expiresAt = v.payload.expiresAt;
        if (v.valid) { s.state = State::Licensed; return s; }
        s.state = v.code == "EXPIRED" ? State::Expired : State::Invalid;
        s.code = v.code;
        s.reason = v.reason;
        return s;
    }
    s.daysLeft = trialDaysLeft (install, now);
    s.state = s.daysLeft > 0 ? State::Trial : State::TrialExpired;
    return s;
}

bool allowed (const Status& s) { return s.state == State::Trial || s.state == State::Licensed; }

bool importKey (const juce::String& text, juce::String& error)
{
    const auto key = text.trim();
    const auto v = verifyKey (key, machineId(), advanceClock (juce::Time::currentTimeMillis()).second);
    if (! v.valid)
    {
        error = v.reason;
        return false;
    }
    if (! licenseFile().replaceWithText (key)) { error = TRANS ("Impossibile salvare la licenza."); return false; }
    return true;
}

juce::String describe (const Status& s)
{
    switch (s.state)
    {
        case State::Trial:        return TRANS ("Versione di prova") + " - " + juce::String (s.daysLeft) + " " + (s.daysLeft == 1 ? TRANS ("giorno rimasto") : TRANS ("giorni rimasti"));
        case State::TrialExpired: return TRANS ("La prova gratuita e' scaduta.");
        case State::Licensed:
        {
            auto t = TRANS ("Licenza valida") + (s.issuedTo.isNotEmpty() ? " - " + s.issuedTo : juce::String());
            t << " - " << (s.expiresAt.isNotEmpty() ? TRANS ("scade il") + " " + juce::Time::fromISO8601 (s.expiresAt).formatted ("%d/%m/%Y") : TRANS ("senza scadenza"));
            return t;
        }
        case State::Expired:      return TRANS ("Licenza scaduta") + (s.expiresAt.isNotEmpty() ? " (" + juce::Time::fromISO8601 (s.expiresAt).formatted ("%d/%m/%Y") + ")" : juce::String());
        case State::Invalid:      return TRANS ("Licenza non valida") + (s.reason.isNotEmpty() ? ": " + s.reason : juce::String());
    }
    return {};
}

juce::String shortLabel (const Status& s)
{
    switch (s.state)
    {
        case State::Trial:        return TRANS ("Prova") + ": " + juce::String (s.daysLeft) + " " + (s.daysLeft == 1 ? TRANS ("giorno") : TRANS ("giorni"));
        case State::Licensed:     return TRANS ("Licenza valida");
        case State::TrialExpired: return TRANS ("Prova scaduta");
        case State::Expired:      return TRANS ("Licenza scaduta");
        case State::Invalid:      return TRANS ("Licenza non valida");
    }
    return {};
}

bool eulaAccepted() { return settings().getValue ("eula.accepted") == "1"; }

void acceptEula()
{
    settings().setValue ("eula.accepted", "1");
    settings().saveIfNeeded();
}

juce::String eulaText (const juce::String& lang)
{
    return lang == "en" ? juce::String::fromUTF8 (EulaData::eula_en_txt, EulaData::eula_en_txtSize)
                        : juce::String::fromUTF8 (EulaData::eula_it_txt, EulaData::eula_it_txtSize);
}
} // namespace fc::license
