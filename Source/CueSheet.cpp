#include "CueSheet.h"

namespace fc::cuesheet
{
namespace
{
juce::String esc (const juce::String& s)
{
    return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;").replace ("\n", "<br>");
}

int takesOf (const Session& s, int cueId)
{
    int n = 0;
    for (auto& t : s.tracks)
        for (auto& c : t.clips)
            if (c.cueId == cueId) n = juce::jmax (n, c.takeNo);
    return n;
}

juce::String durationText (double sec)
{
    return juce::String (sec, 1) + " s";
}
} // namespace

juce::String buildHtml (const Project& p, const juce::String& title)
{
    const auto& s = p.getSession();
    const double rate = p.getSampleRate();
    auto tc = [&] (int64_t pos) { return p.formatTc ((double) pos / rate); };

    int counts[4] = { 0, 0, 0, 0 };
    for (auto& c : s.cues) ++counts[juce::jlimit (0, 3, c.status)];

    juce::String h;
    h << "<!DOCTYPE html><html lang=\"it\"><head><meta charset=\"utf-8\"><title>" << esc (title) << "</title><style>"
         "body{font-family:-apple-system,Helvetica,Arial,sans-serif;color:#111;margin:24px}"
         "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:26px 0 6px;border-bottom:2px solid #333;padding-bottom:3px}"
         ".meta{color:#555;font-size:12px;margin-bottom:14px}"
         "table{border-collapse:collapse;width:100%;font-size:12px}th,td{border:1px solid #bbb;padding:5px 7px;vertical-align:top;text-align:left}"
         "th{background:#eee}td.n,td.tc{white-space:nowrap;font-variant-numeric:tabular-nums}"
         ".line{font-size:15px;line-height:1.35}.st0{color:#555}.st1{color:#0a6}.st2{color:#06c;font-weight:600}.st3{color:#c30;font-weight:600}"
         ".char{page-break-before:always}.char:first-of-type{page-break-before:auto}"
         "@media print{body{margin:10mm}a{color:inherit}}"
         "</style></head><body>";
    h << "<h1>" << esc (title) << "</h1><div class=\"meta\">" << esc (p.getFile() == juce::File() ? juce::String ("Sessione senza nome") : p.getFile().getFileName())
      << " &middot; timecode " << esc (p.tcFmt().name) << " &middot; " << (int) s.cues.size() << " cue: "
      << counts[0] << " da registrare, " << counts[1] << " registrati, " << counts[2] << " approvati, " << counts[3] << " da rifare"
      << " &middot; " << esc (juce::Time::getCurrentTime().formatted ("%d/%m/%Y %H:%M")) << "</div>";

    h << "<h2>Riepilogo</h2><table><tr><th>N</th><th>Entrata</th><th>Uscita</th><th>Durata</th><th>Personaggio</th><th>Scena</th><th>Bobina</th><th>Stato</th><th>Take</th></tr>";
    for (auto& c : s.cues)
        h << "<tr><td class=\"n\">" << c.number << "</td><td class=\"tc\">" << esc (tc (c.inPos)) << "</td><td class=\"tc\">" << esc (tc (c.outPos)) << "</td><td>"
          << durationText ((double) (c.outPos - c.inPos) / rate) << "</td><td>" << esc (c.character) << "</td><td>" << esc (c.scene) << "</td><td>" << esc (c.reel)
          << "</td><td class=\"st" << juce::jlimit (0, 3, c.status) << "\">" << esc (adrStatusName (c.status)) << "</td><td>" << takesOf (s, c.id) << "</td></tr>";
    h << "</table>";

    // un copione per personaggio, nell'ordine in cui compaiono
    std::vector<juce::String> names;
    for (auto& c : s.cues)
    {
        const auto n = c.character.trim().isEmpty() ? juce::String ("(senza personaggio)") : c.character.trim();
        bool known = false;
        for (auto& k : names) known = known || k.equalsIgnoreCase (n);
        if (! known) names.push_back (n);
    }
    for (auto& name : names)
    {
        h << "<div class=\"char\"><h2>" << esc (name) << "</h2><table><tr><th>N</th><th>Entrata</th><th>Uscita</th><th>Battuta</th><th>Note</th><th>Stato</th></tr>";
        for (auto& c : s.cues)
        {
            const auto n = c.character.trim().isEmpty() ? juce::String ("(senza personaggio)") : c.character.trim();
            if (! n.equalsIgnoreCase (name)) continue;
            h << "<tr><td class=\"n\">" << c.number << "</td><td class=\"tc\">" << esc (tc (c.inPos)) << "</td><td class=\"tc\">" << esc (tc (c.outPos))
              << "</td><td class=\"line\">" << esc (c.dialogue) << "</td><td>" << esc (c.notes) << "</td><td class=\"st" << juce::jlimit (0, 3, c.status) << "\">"
              << esc (adrStatusName (c.status)) << "</td></tr>";
        }
        h << "</table></div>";
    }
    h << "</body></html>";
    return h;
}
} // namespace fc::cuesheet
