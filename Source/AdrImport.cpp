#include "AdrImport.h"
#include "Timecode.h"

namespace fc::adr
{
namespace
{
bool looksLikeTimecode (const juce::String& s)
{
    int64_t f = 0;
    for (int i = 0; i < numTcFormats; ++i)
        if (timecodeToFrames (s, tcFormats()[i], f)) return true;
    return false;
}

/** Fotogrammi scritti in un timecode "hh:mm:ss:ff" (-1 se non e' un timecode a quattro campi). */
int frameFieldOf (const juce::String& s, bool* dropMark = nullptr)
{
    const auto t = s.trim();
    if (dropMark != nullptr && t.contains (";")) *dropMark = true;
    auto parts = juce::StringArray::fromTokens (t.replaceCharacter (';', ':').replaceCharacter ('.', ':').replaceCharacter (',', ':'), ":", "");
    parts.removeEmptyStrings();
    if (parts.size() != 4) return -1;
    for (auto& p : parts) if (! p.containsOnly ("0123456789")) return -1;
    return parts[3].getIntValue();
}

juce::String norm (const juce::String& s)
{
    return s.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789#");
}

juce::StringArray splitLines (const juce::String& text)
{
    auto t = text.replace ("\r\n", "\n").replaceCharacter ('\r', '\n');
    return juce::StringArray::fromLines (t);
}

juce::StringArray splitFields (const juce::String& line, juce::juce_wchar delim)
{
    juce::StringArray out;
    juce::String cur;
    bool quoted = false;
    for (auto c : line)
    {
        if (c == '"') { quoted = ! quoted; continue; }
        if (c == delim && ! quoted) { out.add (cur.trim()); cur.clear(); continue; }
        cur += juce::String::charToString (c);
    }
    out.add (cur.trim());
    return out;
}

/** Cerca "CHIAVE: valore" (le chiavi sono quelle dei commenti dei cue). Valori separati da '|' o da ';' se ci sono piu' chiavi. */
juce::String keyName (const juce::String& k)
{
    const auto n = norm (k);
    if (n == "character" || n == "char" || n == "personaggio" || n == "actor" || n == "attore" || n == "ruolo" || n == "role") return "character";
    if (n == "scene" || n == "scena" || n == "sc") return "scene";
    if (n == "reel" || n == "rullo" || n == "rullino") return "reel";
    if (n == "dialogue" || n == "dialog" || n == "dialogo" || n == "line" || n == "text" || n == "testo" || n == "battuta") return "dialogue";
    if (n == "note" || n == "notes" || n == "comment" || n == "commento") return "notes";
    if (n == "out" || n == "tcout" || n == "end" || n == "fine" || n == "uscita") return "out";
    if (n == "in" || n == "tcin" || n == "start" || n == "inizio" || n == "ingresso") return "in";
    if (n == "cue" || n == "number" || n == "numero" || n == "n") return "number";
    return {};
}
} // namespace

void parseCommentFields (const juce::String& comment, ImportedCue& cue)
{
    // se non ci sono chiavi il commento intero e' il dialogo
    bool anyKey = false;
    auto parts = juce::StringArray::fromTokens (comment, "|", "");
    for (auto& part : parts)
    {
        const int colon = part.indexOfChar (':');
        if (colon <= 0 || colon > 14) continue;
        const auto key = keyName (part.substring (0, colon));
        if (key.isEmpty()) continue;
        // il timecode contiene ':' : la chiave e' solo la prima parola prima dei due punti
        const auto value = part.substring (colon + 1).trim();
        anyKey = true;
        if (key == "character") cue.character = value;
        else if (key == "scene") cue.scene = value;
        else if (key == "reel") cue.reel = value;
        else if (key == "dialogue") cue.dialogue = value;
        else if (key == "notes") cue.notes = value;
        else if (key == "out") cue.tcOut = value;
        else if (key == "in") cue.tcIn = value;
        else if (key == "number") cue.number = value;
    }
    if (! anyKey && comment.trim().isNotEmpty()) cue.dialogue = comment.trim();
}

//==============================================================================
static ImportResult parseProTools (const juce::StringArray& lines)
{
    ImportResult r;
    r.format = "Pro Tools (testo Session Info)";

    // intestazione della sessione
    for (auto& l : lines)
    {
        auto f = splitFields (l, '\t');
        if (f.size() < 2) continue;
        const auto key = f[0].toUpperCase();
        if (key.startsWith ("SAMPLE RATE")) r.sampleRate = f[1].getDoubleValue();
        else if (key.startsWith ("SESSION START TIMECODE")) r.sessionStartTc = f[1];
        else if (key.startsWith ("TIMECODE FORMAT"))
        {
            const auto v = f[1].toLowerCase();
            if (v.contains ("23.976") || v.contains ("23,976")) r.tcFormatIndex = 0;
            else if (v.startsWith ("24")) r.tcFormatIndex = 1;
            else if (v.startsWith ("25")) r.tcFormatIndex = 2;
            else if (v.contains ("29.97") || v.contains ("29,97"))
                r.tcFormatIndex = v.contains ("drop") && ! v.contains ("non") ? 4 : 3;
            else if (v.startsWith ("30")) r.tcFormatIndex = 5;
            else if (v.startsWith ("50")) r.tcFormatIndex = 6;
            else if (v.startsWith ("60")) r.tcFormatIndex = 7;
        }
    }

    int start = -1;
    for (int i = 0; i < lines.size(); ++i)
        if (norm (lines[i]).contains ("markerslisting")) { start = i; break; }
    if (start < 0) return r;

    // riga delle intestazioni (contiene LOCATION)
    int header = -1;
    for (int i = start + 1; i < juce::jmin (lines.size(), start + 6); ++i)
        if (lines[i].toUpperCase().contains ("LOCATION")) { header = i; break; }
    if (header < 0) return r;

    const auto cols = splitFields (lines[header], '\t');
    int cLoc = -1, cRef = -1, cName = -1, cCom = -1;
    for (int i = 0; i < cols.size(); ++i)
    {
        const auto c = cols[i].toUpperCase();
        if (c.contains ("LOCATION") && cLoc < 0) cLoc = i;
        else if (c.contains ("TIME REFERENCE")) cRef = i;
        else if (c == "NAME") cName = i;
        else if (c.contains ("COMMENT")) cCom = i;
    }
    if (cLoc < 0) return r;

    for (int i = header + 1; i < lines.size(); ++i)
    {
        if (lines[i].trim().isEmpty()) { if (! r.cues.empty()) break; else continue; }
        const auto f = splitFields (lines[i], '\t');
        if (f.size() <= cLoc) continue;
        ImportedCue c;
        c.number = f[0].trim();
        const auto loc = f[cLoc];
        if (looksLikeTimecode (loc)) c.tcIn = loc;
        else if (cRef >= 0 && cRef < f.size()) c.samplesIn = f[cRef].getDoubleValue();
        if (c.tcIn.isEmpty() && c.samplesIn < 0.0) continue;
        if (cName >= 0 && cName < f.size()) c.name = f[cName];
        if (cCom >= 0 && cCom < f.size()) parseCommentFields (f[cCom], c);
        if (c.character.isEmpty()) c.character = c.name;
        r.cues.push_back (c);
    }
    return r;
}

//==============================================================================
static ImportResult parseEdl (const juce::StringArray& lines)
{
    ImportResult r;
    r.format = "EDL (CMX3600)";
    for (auto& l : lines)
        if (l.toUpperCase().startsWith ("FCM:"))
            r.tcFormatIndex = l.toUpperCase().contains ("NON") ? 3 : (l.toUpperCase().contains ("DROP") ? 4 : -1);

    ImportedCue cur;
    bool open = false;
    auto flush = [&]
    {
        if (open && (cur.tcIn.isNotEmpty())) r.cues.push_back (cur);
        cur = {};
        open = false;
    };

    for (auto& raw : lines)
    {
        const auto l = raw.trim();
        if (l.isEmpty()) continue;
        if (l.startsWith ("*"))
        {
            const auto body = l.substring (1).trim();
            const auto up = body.toUpperCase();
            if (up.startsWith ("FROM CLIP NAME:")) { if (open) cur.name = body.fromFirstOccurrenceOf (":", false, false).trim(); }
            else if (up.startsWith ("COMMENT:")) { if (open) parseCommentFields (body.fromFirstOccurrenceOf (":", false, false).trim(), cur); }
            else if (up.startsWith ("LOC:"))
            {
                // locatore: "* LOC: 01:00:12:05 RED Testo del marker"
                flush();
                auto toks = juce::StringArray::fromTokens (body.fromFirstOccurrenceOf (":", false, false).trim(), " \t", "");
                toks.removeEmptyStrings();
                if (toks.size() >= 1 && looksLikeTimecode (toks[0]))
                {
                    ImportedCue c;
                    c.tcIn = toks[0];
                    c.name = toks.size() > 2 ? toks.joinIntoString (" ", 2) : juce::String();
                    parseCommentFields (c.name, c);
                    if (c.character.isEmpty()) c.character = c.name.upToFirstOccurrenceOf ("-", false, false).trim();
                    r.cues.push_back (c);
                }
            }
            continue;
        }
        if (l.toUpperCase().startsWith ("TITLE:") || l.toUpperCase().startsWith ("FCM:")) continue;

        auto toks = juce::StringArray::fromTokens (l, " \t", "");
        toks.removeEmptyStrings();
        // evento: numero, bobina, canale, transizione, [durata], 4 timecode
        if (toks.size() >= 8 && toks[0].containsOnly ("0123456789"))
        {
            flush();
            int tcCount = 0;
            for (int i = toks.size() - 1; i >= 4 && tcCount < 4; --i) if (looksLikeTimecode (toks[i])) ++tcCount; else break;
            if (tcCount == 4)
            {
                cur.number = toks[0];
                cur.name = toks[1];
                cur.tcIn = toks[toks.size() - 2];     // record in
                cur.tcOut = toks[toks.size() - 1];    // record out
                open = true;
            }
        }
    }
    flush();
    return r;
}

//==============================================================================
static ImportResult parseTable (const juce::StringArray& lines)
{
    ImportResult r;
    r.format = "Tabella (CSV / testo)";

    int h = 0;
    while (h < lines.size() && lines[h].trim().isEmpty()) ++h;
    if (h >= lines.size()) return r;

    const auto& hl = lines[h];
    juce::juce_wchar delim = ',';
    int best = hl.retainCharacters (",").length();
    if (hl.retainCharacters ("\t").length() >= best && hl.containsChar ('\t')) { delim = '\t'; best = hl.retainCharacters ("\t").length(); }
    if (hl.retainCharacters (";").length() > best) delim = ';';

    const auto cols = splitFields (hl, delim);
    int cNum = -1, cChar = -1, cScene = -1, cReel = -1, cIn = -1, cOut = -1, cDial = -1, cNotes = -1, cName = -1;
    for (int i = 0; i < cols.size(); ++i)
    {
        const auto n = norm (cols[i]);
        if (n.isEmpty()) continue;
        auto has = [&n] (std::initializer_list<const char*> words) { for (auto w : words) if (n.contains (w)) return true; return false; };
        auto is = [&n] (std::initializer_list<const char*> words) { for (auto w : words) if (n == w) return true; return false; };

        if (cIn < 0 && (is ({ "in", "tcin", "timecodein", "start", "starttc", "inizio", "ingresso", "cuein", "entrata", "intc" }) || has ({ "tcin", "timecodein", "cuein" }))) cIn = i;
        else if (cOut < 0 && (is ({ "out", "tcout", "timecodeout", "end", "endtc", "fine", "uscita", "cueout", "outtc" }) || has ({ "tcout", "timecodeout", "cueout" }))) cOut = i;
        else if (cDial < 0 && has ({ "dialog", "dialogo", "battuta", "line", "text", "testo" })) cDial = i;
        else if (cChar < 0 && has ({ "char", "personag", "actor", "attore", "ruolo", "role" })) cChar = i;
        else if (cScene < 0 && has ({ "scene", "scena" })) cScene = i;
        else if (cReel < 0 && has ({ "reel", "rullo", "rullino" })) cReel = i;
        else if (cNotes < 0 && has ({ "note", "comment", "commento" })) cNotes = i;
        else if (cNum < 0 && (is ({ "#", "n", "no", "num", "number", "numero", "cue", "cuenumber", "cuen", "id" }) || has ({ "cue#", "cuen" }))) cNum = i;
        else if (cName < 0 && is ({ "name", "nome", "title", "titolo" })) cName = i;
    }
    if (cIn < 0) return r;

    for (int i = h + 1; i < lines.size(); ++i)
    {
        if (lines[i].trim().isEmpty()) continue;
        const auto f = splitFields (lines[i], delim);
        if (f.size() <= cIn || ! looksLikeTimecode (f[cIn])) continue;
        auto get = [&f] (int c) { return (c >= 0 && c < f.size()) ? f[c] : juce::String(); };
        ImportedCue c;
        c.tcIn = f[cIn];
        c.tcOut = looksLikeTimecode (get (cOut)) ? get (cOut) : juce::String();
        c.number = get (cNum);
        c.character = get (cChar);
        c.scene = get (cScene);
        c.reel = get (cReel);
        c.dialogue = get (cDial);
        c.notes = get (cNotes);
        c.name = get (cName);
        if (c.character.isEmpty()) c.character = c.name;
        r.cues.push_back (c);
    }
    return r;
}

//==============================================================================
static ImportResult parseFormats (const juce::String& text);

ImportResult parse (const juce::String& text)
{
    auto r = parseFormats (text);
    for (auto& c : r.cues)
        for (const auto* tc : { &c.tcIn, &c.tcOut })
            r.maxFrameField = juce::jmax (r.maxFrameField, frameFieldOf (*tc, &r.dropFrameMarks));
    return r;
}

int suggestedTcFormat (const ImportResult& r, int current)
{
    if (r.tcFormatIndex >= 0) return r.tcFormatIndex;
    current = juce::jlimit (0, numTcFormats - 1, current);
    const auto& cur = tcFormats()[current];
    if (r.maxFrameField < cur.nominal) return current;          // i fotogrammi del file stanno nel formato attuale
    if (r.dropFrameMarks && r.maxFrameField < 30) return 4;      // 29,97 drop frame
    if (r.maxFrameField < 25) return 2;                          // 25 fps
    if (r.maxFrameField < 30) return 5;                          // 30 fps
    if (r.maxFrameField < 50) return 6;                          // 50 fps
    return 7;                                                    // 60 fps
}

static ImportResult parseFormats (const juce::String& text)
{
    const auto lines = splitLines (text);
    if (lines.isEmpty()) return {};

    for (auto& l : lines)
        if (norm (l).contains ("markerslisting")) return parseProTools (lines);

    // EDL: ci sono righe di evento con quattro timecode
    int eventLines = 0;
    for (auto& l : lines)
    {
        auto toks = juce::StringArray::fromTokens (l.trim(), " \t", "");
        toks.removeEmptyStrings();
        if (toks.size() >= 8 && toks[0].containsOnly ("0123456789") && looksLikeTimecode (toks[toks.size() - 1]) && looksLikeTimecode (toks[toks.size() - 4])) ++eventLines;
    }
    if (eventLines > 0 || (lines[0].toUpperCase().startsWith ("TITLE:") && text.contains ("* LOC:"))) return parseEdl (lines);

    return parseTable (lines);
}

std::vector<AdrCue> toCues (const ImportResult& r, const Project& project, double rate, double defaultDur, juce::StringArray& warnings)
{
    std::vector<AdrCue> out;
    int index = 0, badOut = 0;
    juce::String firstBadOut;
    for (auto& ic : r.cues)
    {
        ++index;
        const auto label = "cue " + (ic.number.isNotEmpty() ? ic.number : juce::String (index));
        double tlIn = 0.0, tlOut = 0.0;
        if (ic.tcIn.isNotEmpty())
        {
            if (! project.parseTc (ic.tcIn, tlIn)) { warnings.add (label + ": timecode di ingresso non valido (" + ic.tcIn + ")"); continue; }
        }
        else if (ic.samplesIn >= 0.0)
            tlIn = ic.samplesIn / (r.sampleRate > 0.0 ? r.sampleRate : rate);
        else
        {
            warnings.add (label + ": nessuna posizione");
            continue;
        }
        if (tlIn < 0.0) { warnings.add (label + ": cade prima dell'inizio della sessione (controlla l'inizio timecode)"); continue; }

        bool haveOut = false;
        if (ic.tcOut.isNotEmpty())
        {
            haveOut = project.parseTc (ic.tcOut, tlOut);
            if (! haveOut) { if (badOut++ == 0) firstBadOut = label + " (" + ic.tcOut + ")"; }   // un solo avviso per tutti: i cue sono spesso centinaia
        }
        if (! haveOut || tlOut <= tlIn) tlOut = tlIn + defaultDur;

        AdrCue c;
        c.number = ic.number.retainCharacters ("0123456789").getIntValue();
        c.character = ic.character;
        c.scene = ic.scene;
        c.reel = ic.reel;
        c.dialogue = ic.dialogue;
        c.notes = ic.notes;
        c.inPos = (int64_t) std::llround (tlIn * rate);
        c.outPos = (int64_t) std::llround (tlOut * rate);
        out.push_back (c);
    }
    if (badOut > 0)
        warnings.add (juce::String (badOut) + " cue hanno un timecode di uscita non valido nel formato scelto (primo: " + firstBadOut
                      + "): per quelli e' stata usata la durata predefinita. Controlla il formato timecode: i fotogrammi del file potrebbero superare quelli del formato.");
    return out;
}

} // namespace fc::adr
