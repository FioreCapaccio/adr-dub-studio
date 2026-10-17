#include "LogicalEditor.h"

namespace fc::logic
{
juce::StringArray fieldNames() { return { TRANS ("Nome del file"), TRANS ("Nome della traccia"), TRANS ("Personaggio del cue"), TRANS ("Durata (s)"), TRANS ("Guadagno (dB)"), TRANS ("Silenziato (1 si, 0 no)"), TRANS ("Stelle del take"), TRANS ("Posizione (s)") }; }
juce::StringArray opNames() { return { TRANS ("contiene"), TRANS ("e' uguale a"), TRANS ("e' maggiore di"), TRANS ("e' minore di") }; }
juce::StringArray actionNames()
{
    return { TRANS ("Seleziona"), TRANS ("Silenzia"), TRANS ("Riattiva"), TRANS ("Elimina"), TRANS ("Imposta il guadagno (dB)"), TRANS ("Aggiungi al guadagno (dB)"),
             TRANS ("Imposta le dissolvenze (ms)"), TRANS ("Valuta con N stelle"), TRANS ("Scarta il take") };
}

static bool test (const Cond& c, const Project& p, const Track& t, const Clip& clip)
{
    const double rate = p.getSampleRate();
    auto text = [&c] (const juce::String& s)
    {
        if (c.op == Op::Contains) return s.containsIgnoreCase (c.value);
        if (c.op == Op::Equals) return s.equalsIgnoreCase (c.value);
        return false;   // maggiore / minore non hanno senso sul testo
    };
    auto num = [&c] (double v)
    {
        const double x = c.value.replaceCharacter (',', '.').getDoubleValue();
        switch (c.op)
        {
            case Op::Greater: return v > x;
            case Op::Less:    return v < x;
            default:          return std::abs (v - x) < 0.0005 + 1.0e-9;
        }
    };
    switch (c.field)
    {
        case Field::Name:      return clip.asset != nullptr && text (clip.asset->name);
        case Field::Track:     return text (t.name);
        case Field::Character:
        {
            const auto* cue = clip.cueId != 0 ? p.findCue (clip.cueId) : nullptr;
            return cue != nullptr && text (cue->character);
        }
        case Field::Length:    return num ((double) clip.length / rate);
        case Field::Gain:      return num ((double) clip.gainDb);
        case Field::Muted:     return num (clip.muted ? 1.0 : 0.0);
        case Field::Rating:    return num ((double) clip.rating);
        case Field::Start:     return num ((double) clip.start / rate);
    }
    return false;
}

std::vector<int64_t> matching (const Project& p, const std::vector<Cond>& conds)
{
    std::vector<int64_t> out;
    for (auto& t : p.getSession().tracks)
        for (auto& c : t.clips)
        {
            bool ok = true;
            for (auto& cond : conds) ok = ok && test (cond, p, t, c);
            if (ok) out.push_back (c.id);
        }
    return out;
}

int apply (Project& p, const std::vector<int64_t>& ids, Action action, double param)
{
    if (ids.empty()) return 0;
    if (action == Action::Select)
    {
        p.selection = std::set<int64_t> (ids.begin(), ids.end());
        p.sendChangeMessage();
        return (int) ids.size();
    }
    std::vector<int64_t> todo;
    for (auto id : ids) { int ti = -1; if (p.findClip (id, &ti) != nullptr && ! p.getSession().tracks[(size_t) ti].editLocked()) todo.push_back (id); }
    if (todo.empty()) return 0;
    p.beginEdit();
    const auto fade = (int64_t) std::llround (param * 0.001 * p.getSampleRate());
    int n = 0;
    for (auto id : todo)
    {
        auto* c = p.findClip (id);
        if (c == nullptr) continue;
        switch (action)
        {
            case Action::Mute:      c->muted = true; break;
            case Action::Unmute:    c->muted = false; break;
            case Action::SetGain:   c->gainDb = juce::jlimit (-60.0f, 24.0f, (float) param); break;
            case Action::AddGain:   c->gainDb = juce::jlimit (-60.0f, 24.0f, c->gainDb + (float) param); break;
            case Action::SetFadeMs: c->fadeIn = juce::jlimit<int64_t> (0, c->length / 2, fade); c->fadeOut = juce::jlimit<int64_t> (0, c->length / 2, fade); break;
            case Action::Rate:      c->rating = juce::jlimit (0, 5, (int) std::lround (param)); break;
            case Action::Reject:    c->rejected = true; break;
            default: break;
        }
        ++n;
    }
    if (action == Action::Delete)
    {
        for (auto& t : p.edit().tracks)
            t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [&todo] (const Clip& c) { return std::find (todo.begin(), todo.end(), c.id) != todo.end(); }), t.clips.end());
        p.selection.clear();
        n = (int) todo.size();
    }
    p.changed();
    return n;
}
} // namespace fc::logic
