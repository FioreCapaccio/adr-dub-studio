#include "SampleEditor.h"
#include "AudioTools.h"

namespace fc
{
using namespace theme;

namespace
{
/** FFT radix-2 sul posto (x di dimensione potenza di 2). */
void fft (std::vector<std::complex<float>>& x)
{
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const auto w = std::polar (1.0f, -juce::MathConstants<float>::twoPi / (float) len);
        for (size_t i = 0; i < n; i += len)
{
            std::complex<float> t (1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k, t *= w)
            {
                const auto u = x[i + k], v = x[i + k + len / 2] * t;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
            }
        }
    }
}

juce::Colour heat (float v)   // 0..1 -> nero, blu, ciano, giallo, bianco
{
    v = juce::jlimit (0.0f, 1.0f, v);
    if (v < 0.25f) return juce::Colour::fromFloatRGBA (0.0f, 0.0f, v * 3.2f, 1.0f);
    if (v < 0.5f)  return juce::Colour::fromFloatRGBA (0.0f, (v - 0.25f) * 4.0f, 0.8f, 1.0f);
    if (v < 0.75f) return juce::Colour::fromFloatRGBA ((v - 0.5f) * 4.0f, 1.0f, 0.8f * (1.0f - (v - 0.5f) * 4.0f), 1.0f);
    return juce::Colour::fromFloatRGBA (1.0f, 1.0f, (v - 0.75f) * 4.0f, 1.0f);
}
} // namespace

juce::Image makeSpectrogram (const juce::AudioBuffer<float>& audio, int64_t from, int64_t length, double rate, int width, int height)
{
    constexpr int N = 1024;
    juce::Image img (juce::Image::RGB, juce::jmax (1, width), juce::jmax (1, height), true);
    if (length <= 0 || audio.getNumSamples() == 0) return img;
    static const std::vector<float> hann = [] { std::vector<float> w (N); for (int i = 0; i < N; ++i) w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N); return w; }();
    const float fLo = 60.0f, fHi = (float) juce::jmin (12000.0, rate * 0.5);
    std::vector<std::complex<float>> x (N);
    std::vector<float> col ((size_t) N / 2 + 1);
    const int nch = audio.getNumChannels(), total = audio.getNumSamples();
    for (int px = 0; px < width; ++px)
    {
        const int64_t centre = from + (int64_t) (((double) px + 0.5) * (double) length / (double) width);
        for (int i = 0; i < N; ++i)
        {
            const int64_t s = centre - N / 2 + i;
            float v = 0.0f;
            if (s >= 0 && s < total) { for (int ch = 0; ch < nch; ++ch) v += audio.getSample (ch, (int) s); v /= (float) nch; }
            x[(size_t) i] = v * hann[(size_t) i];
        }
        fft (x);
        for (size_t k = 0; k < col.size(); ++k) col[k] = juce::Decibels::gainToDecibels (std::abs (x[k]) * 4.0f / (float) N, -120.0f);
        for (int py = 0; py < height; ++py)
        {
            const float f0 = fLo * std::pow (fHi / fLo, (float) (height - 1 - py) / (float) height), f1 = fLo * std::pow (fHi / fLo, (float) (height - py) / (float) height);
            const int b0 = (int) std::floor (f0 / (float) rate * N), b1 = juce::jmax (b0, (int) std::floor (f1 / (float) rate * N));
            float m = -120.0f;
            for (int k = b0; k <= b1 && k < (int) col.size(); ++k) m = juce::jmax (m, col[(size_t) k]);
            img.setPixelAt (px, py, heat ((m + 90.0f) / 70.0f));
        }
    }
    return img;
}

//==============================================================================
SampleEditor::SampleEditor (Project& p, AudioEngine& e, int64_t id, std::function<void (juce::String)> err)
    : project (p), engine (e), clipId (id), showError (std::move (err))
{
    for (auto* b : { &viewBtn, &allBtn, &listenBtn, &fitBtn, &gainBtn, &normBtn, &fadeInBtn, &fadeOutBtn, &silBtn, &revBtn, &invBtn, &dcBtn, &hitBtn, &sliceBtn })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    viewBtn.setClickingTogglesState (true);
    viewBtn.setTooltip (TRANS ("Alterna fra forma d'onda e spettrogramma (frequenze verso l'alto, colore = livello): si vedono sibilanti, rumori e colpi di vento"));
    allBtn.setTooltip (TRANS ("Seleziona tutto il clip"));
    listenBtn.setTooltip (TRANS ("Ascolta la selezione (o tutto il clip) in ciclo; ferma con Spazio"));
    fitBtn.setTooltip (TRANS ("Mostra tutto il clip"));
    gainBtn.setTooltip (TRANS ("Cambia il livello della selezione dei dB scritti qui accanto"));
    normBtn.setTooltip (TRANS ("Porta il picco della selezione a -1 dBFS"));
    fadeInBtn.setTooltip (TRANS ("Dissolvenza in crescendo sulla selezione"));
    fadeOutBtn.setTooltip (TRANS ("Dissolvenza in calando sulla selezione"));
    silBtn.setTooltip (TRANS ("Azzera la selezione (taglia un colpo di tosse, un click)"));
    revBtn.setTooltip (TRANS ("Inverte la selezione nel tempo"));
    invBtn.setTooltip (TRANS ("Inverte la polarita' della selezione"));
    dcBtn.setTooltip (TRANS ("Toglie l'offset di continua dalla selezione"));
    hitBtn.setClickingTogglesState (true);
    hitBtn.setTooltip (TRANS ("Mostra i punti di attacco (dove il suono parte all'improvviso): la barra accanto regola la sensibilita'"));
    sliceBtn.setTooltip (TRANS ("Divide il clip nei punti di attacco mostrati: ogni sillaba o colpo diventa un clip (un solo passo di undo)"));
    hitSens.setRange (0.0, 1.0, 0.01);
    hitSens.setValue (0.5, juce::dontSendNotification);
    hitSens.onValueChange = [this] { findHits(); repaint(); };
    addAndMakeVisible (hitSens);
    hitBtn.onClick = [this] { findHits(); repaint(); };
    sliceBtn.onClick = [this]
    {
        const auto* c = project.findClip (clipId);
        if (c == nullptr || hits.empty()) { if (showError) showError (TRANS ("Prima mostra i punti di attacco: nessuno trovato.")); return; }
        std::vector<int64_t> pos;
        for (auto h : hits) pos.push_back (c->start + h);
        project.sliceClip (clipId, pos);
    };
    gainEd.setText ("-6", false);
    gainEd.setJustification (juce::Justification::centred);
    gainEd.setInputRestrictions (6, "-+0123456789.,");
    addAndMakeVisible (gainEd);
    hint.setText (TRANS ("Trascina per selezionare, rotellina per scorrere, Cmd/Ctrl + rotellina per lo zoom. Le elaborazioni agiscono solo sulla selezione e si annullano con Modifica > Annulla."), juce::dontSendNotification);
    hint.setColour (juce::Label::textColourId, dim);
    hint.setFont (font (11.0f));
    addAndMakeVisible (hint);
    addAndMakeVisible (bar);
    bar.addListener (this);

    viewBtn.onClick = [this] { spectrogram = viewBtn.getToggleState(); spectro = {}; repaint(); };
    allBtn.onClick = [this] { if (auto* c = project.findClip (clipId)) setSelection (0, c->length); };
    fitBtn.onClick = [this] { if (auto* c = project.findClip (clipId)) { spp = juce::jmax (0.5, (double) c->length / (double) juce::jmax (100, plot().getWidth())); viewStart = 0.0; updateScroll(); repaint(); } };
    listenBtn.onClick = [this]
    {
        const auto* c = project.findClip (clipId);
        if (c == nullptr) return;
        const int64_t a = selA >= 0 ? juce::jmin (selA, selB) : 0, b = selA >= 0 ? juce::jmax (selA, selB) : c->length;
        engine.setLoop (true, c->start + a, c->start + b);
        engine.setPosition (c->start + a);
        engine.play();
    };
    gainBtn.onClick = [this] { applyRange (Project::RangeOp::Gain, gainEd.getText().replaceCharacter (',', '.').getDoubleValue()); };
    normBtn.onClick = [this] { applyRange (Project::RangeOp::Normalize, -1.0); };
    fadeInBtn.onClick = [this] { applyRange (Project::RangeOp::FadeIn); };
    fadeOutBtn.onClick = [this] { applyRange (Project::RangeOp::FadeOut); };
    silBtn.onClick = [this] { applyRange (Project::RangeOp::Silence); };
    revBtn.onClick = [this] { applyRange (Project::RangeOp::Reverse); };
    invBtn.onClick = [this] { applyRange (Project::RangeOp::Invert); };
    dcBtn.onClick = [this] { applyRange (Project::RangeOp::RemoveDc); };

    project.addChangeListener (this);
    startTimerHz (20);
    setSize (980, 480);
}

SampleEditor::~SampleEditor() { project.removeChangeListener (this); }

bool SampleEditor::applyRange (Project::RangeOp op, double param)
{
    if (selA < 0 || std::abs (selB - selA) < 2) { if (showError) showError (TRANS ("Seleziona prima una porzione di audio.")); return false; }
    const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ADR Dub Studio").getChildFile ("Elaborati");
    project.processRange (clipId, juce::jmin (selA, selB), juce::jmax (selA, selB), op, param, folder, [this, safe = juce::Component::SafePointer<SampleEditor> (this)] (juce::String err)
    {
        if (safe != nullptr && err.isNotEmpty() && showError) showError (err);
    });
    return true;
}

juce::Rectangle<int> SampleEditor::plot() const { return getLocalBounds().withTrimmedTop (94).withTrimmedBottom (46).reduced (8, 0); }

void SampleEditor::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    auto row1 = area.removeFromTop (28);
    area.removeFromTop (4);
    auto row2 = area.removeFromTop (28);
    int x = row1.getX();
    auto put = [&] (juce::Rectangle<int> row, juce::Component& c, int w) { c.setBounds (x, row.getY(), w, row.getHeight()); x += w + 4; };
    put (row1, viewBtn, 118); put (row1, allBtn, 52); put (row1, fitBtn, 58); put (row1, listenBtn, 70); x += 16;
    put (row1, hitBtn, 78); put (row1, hitSens, 100); put (row1, sliceBtn, 62);
    x = row2.getX();
    put (row2, gainBtn, 80); put (row2, gainEd, 46); put (row2, normBtn, 92); put (row2, fadeInBtn, 62); put (row2, fadeOutBtn, 70);
    put (row2, silBtn, 70); put (row2, revBtn, 68); put (row2, invBtn, 66); put (row2, dcBtn, 72);
    spectro = {};
    if (! fitted && plot().getWidth() > 100)   // alla prima apertura il clip sta tutto nella finestra
    {
        fitted = true;
        if (auto* c = project.findClip (clipId)) spp = juce::jmax (0.5, (double) c->length / (double) plot().getWidth());
    }
    hint.setBounds (8, 70, getWidth() - 16, 18);
    auto bb = getLocalBounds().reduced (8, 0);
    bar.setBounds (bb.removeFromBottom (14).translated (0, -26));
    updateScroll();
}

void SampleEditor::findHits()
{
    hits.clear();
    const auto* c = project.findClip (clipId);
    if (c == nullptr || c->asset == nullptr || ! hitBtn.getToggleState()) return;
    hits = tools::findHitpoints (c->asset->audio, c->offset, c->length, project.getSampleRate(), hitSens.getValue(), 0.08);
}

void SampleEditor::updateScroll()
{
    const auto* c = project.findClip (clipId);
    if (c == nullptr) return;
    const double visible = (double) plot().getWidth() * spp;
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, (double) c->length - visible), viewStart);
    bar.setRangeLimits (0.0, juce::jmax ((double) c->length, visible));
    bar.setCurrentRange (viewStart, visible, juce::dontSendNotification);
}

void SampleEditor::zoomAt (float x, double factor)
{
    const auto* c = project.findClip (clipId);
    if (c == nullptr) return;
    const double at = viewStart + (x - (float) plot().getX()) * spp;
    spp = juce::jlimit (0.02, juce::jmax (0.5, (double) c->length / (double) juce::jmax (100, plot().getWidth())) * 1.5, spp * factor);
    viewStart = at - (x - (float) plot().getX()) * spp;
    updateScroll();
    repaint();
}

void SampleEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) { zoomAt ((float) e.x, w.deltaY > 0 ? 0.8 : 1.25); return; }
    viewStart -= (std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : -w.deltaY) * 200.0 * spp;
    updateScroll();
    repaint();
}

void SampleEditor::mouseMagnify (const juce::MouseEvent& e, float scale) { zoomAt ((float) e.x, 1.0 / juce::jmax (0.2f, scale)); }

void SampleEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! plot().contains (e.getPosition())) return;
    const auto* c = project.findClip (clipId);
    if (c == nullptr) return;
    anchor = juce::jlimit<int64_t> (0, c->length, xToPos ((float) e.x));
    selA = selB = -1;
    engine.setPosition (c->start + anchor);
    repaint();
}

void SampleEditor::mouseDrag (const juce::MouseEvent& e)
{
    const auto* c = project.findClip (clipId);
    if (c == nullptr || e.getDistanceFromDragStart() < 3) return;
    selA = anchor;
    selB = juce::jlimit<int64_t> (0, c->length, xToPos ((float) e.x));
    repaint();
}

void SampleEditor::mouseUp (const juce::MouseEvent&) {}

void SampleEditor::mouseDoubleClick (const juce::MouseEvent&)
{
    if (auto* c = project.findClip (clipId)) setSelection (0, c->length);
}

void SampleEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);
    const auto* c = project.findClip (clipId);
    const auto area = plot();
    if (c == nullptr || c->asset == nullptr) { g.setColour (dim); g.drawText (TRANS ("Il clip non c'e' piu'."), area, juce::Justification::centred); return; }
    const double rate = project.getSampleRate();
    g.setColour (well);
    g.fillRect (area);

    // righello
    const double secPerPx = spp / rate;
    double step = 1.0;
    for (double s : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0 }) { step = s; if (s / secPerPx > 70.0) break; }
    g.setFont (font (10.0f));
    for (double t = std::ceil (viewStart / rate / step) * step; t * rate < viewStart + area.getWidth() * spp; t += step)
    {
        const float x = posToX ((int64_t) std::llround (t * rate));
        g.setColour (grid);
        g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
        g.setColour (dim);
        g.drawText (juce::String (t, step < 0.01 ? 3 : step < 1.0 ? 2 : 1) + " s", (int) x + 3, area.getY() + 1, 60, 12, juce::Justification::left);
    }

    const int64_t last = c->length;
    if (spectrogram)
    {
        if (! spectro.isValid() || spectroSpp != spp || spectroStart != viewStart || spectro.getHeight() != area.getHeight())
        {
            const int64_t from = c->offset + (int64_t) viewStart;
            const int64_t len = juce::jmax<int64_t> (1, juce::jmin<int64_t> (last - (int64_t) viewStart, (int64_t) ((double) area.getWidth() * spp)));
            const int w = (int) juce::jmax<int64_t> (1, (int64_t) ((double) len / spp));
            spectro = makeSpectrogram (c->asset->audio, from, len, rate, w, area.getHeight());
            spectroSpp = spp; spectroStart = viewStart;
        }
        g.drawImageAt (spectro, (int) juce::jmax ((float) area.getX(), posToX (0)), area.getY());
    }
    else
    {
        const int nch = c->asset->audio.getNumChannels();
        const int rowH = area.getHeight() / juce::jmax (1, nch);
        for (int ch = 0; ch < nch; ++ch)
        {
            const auto row = juce::Rectangle<int> (area.getX(), area.getY() + ch * rowH, area.getWidth(), rowH);
            const float mid = (float) row.getCentreY(), half = (float) row.getHeight() * 0.46f;
            g.setColour (grid);
            g.drawHorizontalLine ((int) mid, (float) row.getX(), (float) row.getRight());
            const float* d = c->asset->audio.getReadPointer (ch) + c->offset;
            g.setColour (green.withAlpha (0.9f));
            for (int px = 0; px < row.getWidth(); ++px)
            {
                const int64_t s0 = (int64_t) (viewStart + px * spp), s1 = juce::jmin<int64_t> (last, (int64_t) (viewStart + (px + 1) * spp) + 1);
                if (s0 >= last) break;
                float lo = 0.0f, hi = 0.0f;
                for (int64_t s = s0; s < s1; s += juce::jmax<int64_t> (1, (s1 - s0) / 64)) { lo = juce::jmin (lo, d[s]); hi = juce::jmax (hi, d[s]); }
                g.drawVerticalLine (row.getX() + px, mid - hi * half, mid - lo * half + 1.0f);
            }
        }
    }

    if (selA >= 0 && selA != selB)
    {
        const float x0 = posToX (juce::jmin (selA, selB)), x1 = posToX (juce::jmax (selA, selB));
        g.setColour (blue.withAlpha (0.28f));
        g.fillRect (juce::Rectangle<float> (x0, (float) area.getY(), x1 - x0, (float) area.getHeight()).getIntersection (area.toFloat()));
        g.setColour (hilite);
        g.drawVerticalLine ((int) x0, (float) area.getY(), (float) area.getBottom());
        g.drawVerticalLine ((int) x1, (float) area.getY(), (float) area.getBottom());
        g.setFont (font (11.0f, true));
        g.drawText (juce::String ((double) std::abs (selB - selA) / rate, 3) + " s", (int) x0 + 4, area.getBottom() - 16, 120, 14, juce::Justification::left);
    }
    if (hitBtn.getToggleState())
    {
        g.setColour (yellow.withAlpha (0.9f));
        for (auto h : hits)
        {
            const float x = posToX (h);
            if (x < (float) area.getX() || x > (float) area.getRight()) continue;
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
            g.fillRect (x - 3.0f, (float) area.getY(), 7.0f, 4.0f);
        }
    }
    // testina
    const float px = posToX (engine.getPosition() - c->start);
    if (px >= (float) area.getX() && px <= (float) area.getRight())
    {
        g.setColour (playhead);
        g.drawVerticalLine ((int) px, (float) area.getY(), (float) area.getBottom());
    }
    g.setColour (border);
    g.drawRect (area);
}
} // namespace fc
