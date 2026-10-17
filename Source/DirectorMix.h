#pragma once

#include "AudioEngine.h"
#include "Model.h"
#include "Theme.h"

namespace fc
{
/** Mix del direttore per le cuffie dell'attore: con "mix personalizzato" acceso l'attore sente le tracce coi livelli scelti qui (indipendenti dai fader del master),
    per esempio la guida piu' alta e senza effetti. Spento, sente il master. Vale solo se le cuffie dell'attore sono attive. */
class DirectorMixPanel : public juce::Component, private juce::ChangeListener
{
public:
    DirectorMixPanel (Project& p, AudioEngine& e) : project (p), engine (e)
    {
        custom.setButtonText (TRANS ("Mix personalizzato per le cuffie dell'attore (invece del master)"));
        custom.setToggleState (project.directorMix, juce::dontSendNotification);
        custom.onClick = [this] { project.directorMix = custom.getToggleState(); engine.cue.custom = project.directorMix; project.markDirty(); };
        addAndMakeVisible (custom);
        hint.setText (TRANS ("Livelli di ogni traccia nelle cuffie, dopo il fader. -80 = la traccia non si sente. Servono le cuffie dell'attore attive (Sala > Cuffie dell'attore)."), juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, theme::dim);
        hint.setFont (theme::font (11.0f));
        addAndMakeVisible (hint);
        view.setViewedComponent (&rowsHolder, false);
        view.setScrollBarsShown (true, false);
        addAndMakeVisible (view);
        project.addChangeListener (this);
        engine.cue.custom = project.directorMix;
        rebuild();
        setSize (520, 420);
    }
    ~DirectorMixPanel() override { project.removeChangeListener (this); }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        custom.setBounds (r.removeFromTop (26));
        hint.setBounds (r.removeFromTop (34));
        r.removeFromTop (6);
        view.setBounds (r);
        layoutRows();
    }

private:
    struct Row : public juce::Component
    {
        juce::Label name;
        juce::Slider level { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        Row()
        {
            name.setColour (juce::Label::textColourId, theme::text);
            addAndMakeVisible (name);
            level.setRange (-80.0, 6.0, 0.5);
            level.setSkewFactorFromMidPoint (-18.0);
            level.setTextValueSuffix (" dB");
            level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            addAndMakeVisible (level);
        }
        void resized() override { auto r = getLocalBounds(); name.setBounds (r.removeFromLeft (150)); level.setBounds (r); }
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (project.getSession().tracks.size() != rows.size()) rebuild();
    }

    void rebuild()
    {
        rows.clear();
        const auto& tracks = project.getSession().tracks;
        for (auto& t : tracks)
        {
            auto row = std::make_unique<Row>();
            row->name.setText (t.name, juce::dontSendNotification);
            row->level.setValue (t.live->cueDb.load(), juce::dontSendNotification);
            row->level.onValueChange = [this, id = t.id, s = &row->level]
            {
                if (auto* tr = project.findTrack (id)) { tr->live->cueDb = (float) s->getValue(); project.markDirty(); }
            };
            rowsHolder.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        layoutRows();
    }

    void layoutRows()
    {
        const int w = juce::jmax (200, view.getWidth() - view.getScrollBarThickness());
        rowsHolder.setSize (w, (int) rows.size() * 30 + 4);
        for (size_t i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, (int) i * 30 + 2, w, 28);
    }

    Project& project;
    AudioEngine& engine;
    juce::ToggleButton custom;
    juce::Label hint;
    juce::Viewport view;
    juce::Component rowsHolder;
    std::vector<std::unique_ptr<Row>> rows;
};
} // namespace fc
