#include "StemSeparatorUi.h"

namespace fc::stems
{
Job::Job (Request r, std::function<void (Outcome)> d)
    : juce::ThreadWithProgressWindow (TRANS ("Separazione in dialoghi, musica ed effetti..."), true, true), req (std::move (r)), done (std::move (d)) {}

Outcome process (const Request& reqIn, const std::function<bool (double)>& setProgress, const std::function<void (const juce::String&)>& setStatusMessage, const std::function<bool()>& shouldExit)
{
    Request req = reqIn;
    Outcome outcome;
    juce::String err;
    int64_t totalSamples = 0;
    for (auto& c : req.clips) totalSamples += c.length;
    if (totalSamples <= 0) { outcome.message = TRANS ("Nessun audio da separare."); return outcome; }

    // 1) il motore (una tantum)
    double base = 0.0;
    if (! packReady())
    {
        setStatusMessage (TRANS ("Scarico il motore di separazione (solo la prima volta)..."));
        base = 0.2;
        if (! downloadPack ([&] (float p, const juce::String&) { return setProgress ((double) p * 0.2) && ! shouldExit(); }, err))
        {
            outcome.cancelled = shouldExit();
            outcome.message = err;
            return outcome;
        }
    }
    setStatusMessage (TRANS ("Carico il modello..."));
    OnnxCore core;
    if (! core.load (err)) { outcome.message = err; return outcome; }

    // 2) ogni clip
    req.folder.createDirectory();
    int64_t doneSamples = 0;
    int index = 0;
    juce::StringArray written;
    for (auto& c : req.clips)
    {
        ++index;
        setStatusMessage (TRANS ("Clip ") + juce::String (index) + "/" + juce::String ((int) req.clips.size()) + ": " + c.name);
        const int nch = juce::jmin (2, c.asset->audio.getNumChannels());
        const float* ch[2] = { c.asset->audio.getReadPointer (0) + c.offset, c.asset->audio.getReadPointer (nch - 1) + c.offset };

        const auto stem = juce::File::createLegalFileName (c.name.isNotEmpty() ? c.name : c.asset->name);
        juce::String tag;
        for (int n = 1;; ++n)
        {
            tag = n == 1 ? juce::String() : "_" + juce::String (n);
            if (! req.folder.getChildFile (stem + tag + "_dialoghi.wav").exists() && ! req.folder.getChildFile (stem + tag + "_musica.wav").exists()
                && ! req.folder.getChildFile (stem + tag + "_effetti.wav").exists() && ! req.folder.getChildFile (stem + tag + "_ME.wav").exists()) break;
        }
        const std::array<juce::File, kStems> files { req.folder.getChildFile (stem + tag + "_dialoghi.wav"), req.folder.getChildFile (stem + tag + "_musica.wav"),
                                                     req.folder.getChildFile (stem + tag + "_effetti.wav") };
        Options opt;
        opt.highQuality = req.highQuality;
        opt.workers = OnnxCore::suggestedWorkers();
        const double clipShare = (double) c.length / (double) totalSamples, clipBase = base + (1.0 - base) * (double) doneSamples / (double) totalSamples;
        const bool ok = separate (ch, nch, c.length, req.rate, opt, files, core.fn(),
                                  [&] (float p) { return setProgress (clipBase + (1.0 - base) * clipShare * p) && ! shouldExit(); }, err);
        if (! ok)
        {
            for (auto& f : files) f.deleteFile();
            outcome.cancelled = shouldExit();
            outcome.message = err;
            for (auto& f : written) juce::File (f).deleteFile();
            return outcome;
        }
        doneSamples += c.length;

        Project::StemClipSet set;
        set.clipId = c.clipId;
        std::array<juce::File, 3> finals = files;
        if (req.mergeME)
        {
            const auto me = req.folder.getChildFile (stem + tag + "_ME.wav");
            if (! mixFiles (files[1], files[2], me, err)) { outcome.message = err; return outcome; }
            files[1].deleteFile();
            files[2].deleteFile();
            finals = { files[0], me, juce::File() };
        }
        for (int s = 0; s < 3; ++s)
        {
            if (finals[(size_t) s] == juce::File()) continue;
            juce::String e2;
            set.assets[(size_t) s] = loadAsset (*req.formats, finals[(size_t) s], req.rate, e2);
            if (set.assets[(size_t) s] == nullptr) { outcome.message = e2; return outcome; }
            written.add (finals[(size_t) s].getFullPathName());
        }
        if (req.splitVoices && set.assets[0] != nullptr)
        {
            setStatusMessage (TRANS ("Riconoscimento delle voci..."));
            set.voices = gender::analyse (set.assets[0]->audio, 0, juce::jmin (c.length, set.assets[0]->length()), req.rate);
        }
        outcome.sets.push_back (std::move (set));
    }
    outcome.ok = true;
    outcome.message = TRANS ("File creati in ") + req.folder.getFullPathName();
    return outcome;
}

void Job::run()
{
    outcome = process (req, [this] (double p) { setProgress (p); return true; }, [this] (const juce::String& m) { setStatusMessage (m); }, [this] { return threadShouldExit(); });
}

// ─── Finestra delle opzioni ───
OptionsPanel::OptionsPanel (int numClips, double totalSeconds, bool needsDownload, int downloadMB, std::function<void (Choice)> onStart, std::function<void()> onCancel)
    : start (std::move (onStart)), cancelFn (std::move (onCancel))
{
    title.setText (TRANS ("Separa dialoghi, musica ed effetti"), juce::dontSendNotification);
    title.setFont (theme::font (18.0f, true));
    title.setColour (juce::Label::textColourId, theme::text);
    const int mins = juce::jmax (1, (int) std::ceil (totalSeconds * 0.45 / 60.0));
    info.setText (juce::String (numClips) + " clip, " + juce::String (totalSeconds / 60.0, 1) + " "
                  + TRANS ("minuti di audio. Tempo stimato: circa") + " " + juce::String (mins) + " " + (mins == 1 ? TRANS ("minuto") : TRANS ("minuti"))
                  + " " + TRANS ("(dipende dal computer; con la qualita' alta circa il 50% in piu')."), juce::dontSendNotification);
    info.setFont (theme::font (13.0f));
    info.setColour (juce::Label::textColourId, theme::dim);
    info.setJustificationType (juce::Justification::topLeft);
    note.setText (needsDownload ? TRANS ("La prima volta si scarica il motore di separazione (circa") + " " + juce::String (downloadMB) + " MB, " + TRANS ("una sola volta): serve internet. Poi si lavora offline.")
                                : TRANS ("Il lavoro avviene sul tuo computer, senza internet. Il modello e' Bandit v2 (licenza CC BY-SA 4.0)."), juce::dontSendNotification);
    note.setFont (theme::font (12.0f));
    note.setColour (juce::Label::textColourId, theme::dim);
    note.setJustificationType (juce::Justification::topLeft);
    three.setRadioGroupId (7001);
    two.setRadioGroupId (7001);
    three.setToggleState (true, juce::dontSendNotification);
    mute.setToggleState (true, juce::dontSendNotification);
    go.onClick = [this] { Choice c; c.mergeME = two.getToggleState(); c.highQuality = hq.getToggleState(); c.muteSources = mute.getToggleState(); c.splitVoices = voices.getToggleState(); if (start) start (c); };
    cancel.onClick = [this] { if (cancelFn) cancelFn(); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &title, &info, &note, &three, &two, &hq, &mute, &voices, &go, &cancel }) addAndMakeVisible (c);
    setSize (560, 360);
}

void OptionsPanel::paint (juce::Graphics& g) { g.fillAll (theme::panel); }

void OptionsPanel::resized()
{
    auto r = getLocalBounds().reduced (20);
    title.setBounds (r.removeFromTop (28));
    info.setBounds (r.removeFromTop (44));
    r.removeFromTop (6);
    three.setBounds (r.removeFromTop (26));
    two.setBounds (r.removeFromTop (26));
    hq.setBounds (r.removeFromTop (26));
    mute.setBounds (r.removeFromTop (26));
    voices.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    note.setBounds (r.removeFromTop (44));
    auto row = r.removeFromBottom (32);
    go.setBounds (row.removeFromRight (110));
    row.removeFromRight (8);
    cancel.setBounds (row.removeFromRight (110));
}
} // namespace fc::stems
