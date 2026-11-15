#include "TrackIcons.h"
#include "TrackPresets.h"

namespace fc
{
juce::String trackIconName (int icon)
{
    static const char* n[] = { "", "Voice-over", "Musica", "Effetti", "Cuffie" };
    return icon >= 1 && icon <= kNumTrackIcons ? TRANS (n[icon]) : juce::String();
}

int effectiveTrackIcon (int icon, int type)
{
    if (icon != 0) return icon > 0 ? icon : 0;
    return type == TrackDialogue ? 1 : type == TrackME ? 2 : type == TrackEffects ? 3 : type == TrackGuide ? 4 : 0;
}

bool hasTrackBadge (const juce::Image& image, int icon, int type) { return image.isValid() || effectiveTrackIcon (icon, type) > 0; }

void drawTrackIcon (juce::Graphics& g, int icon, juce::Rectangle<float> r, juce::Colour c)
{
    using juce::Point;
    const float pi = juce::MathConstants<float>::pi;
    juce::Path fill, line;
    auto seg = [&line] (float x1, float y1, float x2, float y2) { line.startNewSubPath (x1, y1); line.lineTo (x2, y2); };
    switch (icon)
    {
        case 1:   // voice-over: microfono
            fill.addRoundedRectangle (0.36f, 0.06f, 0.28f, 0.5f, 0.14f);
            line.addArc (0.22f, 0.28f, 0.56f, 0.46f, 0.5f * pi, 1.5f * pi, true);
            seg (0.5f, 0.74f, 0.5f, 0.92f); seg (0.34f, 0.92f, 0.66f, 0.92f);
            break;
        case 2:   // musica: nota
            fill.addEllipse (0.18f, 0.6f, 0.32f, 0.26f);
            fill.addRectangle (0.44f, 0.14f, 0.07f, 0.58f);
            line.startNewSubPath (0.5f, 0.15f); line.quadraticTo (0.62f, 0.4f, 0.82f, 0.46f);
            break;
        case 3: fill.addStar ({ 0.5f, 0.5f }, 8, 0.17f, 0.46f); break;   // effetti
        case 4:   // guida: cuffie
            line.addArc (0.16f, 0.12f, 0.68f, 0.68f, -0.5f * pi, 0.5f * pi, true);
            fill.addRoundedRectangle (0.1f, 0.44f, 0.18f, 0.34f, 0.06f); fill.addRoundedRectangle (0.72f, 0.44f, 0.18f, 0.34f, 0.06f);
            break;
        default: return;
    }
    const auto t = juce::AffineTransform::scale (r.getWidth(), r.getHeight()).translated (r.getX(), r.getY());
    fill.applyTransform (t);
    line.applyTransform (t);
    g.setColour (c);
    g.fillPath (fill);
    g.strokePath (line, juce::PathStrokeType (juce::jmax (1.2f, 0.09f * r.getWidth()), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawTrackBadge (juce::Graphics& g, const juce::Image& image, int icon, int type, juce::Rectangle<float> r, juce::Colour c)
{
    if (image.isValid())
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, r.getWidth() * 0.2f);
        g.reduceClipRegion (clip);
        g.drawImage (image, r, juce::RectanglePlacement::fillDestination);
        g.setColour (c.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (0.5f), r.getWidth() * 0.2f, 1.0f);
        return;
    }
    drawTrackIcon (g, effectiveTrackIcon (icon, type), r.reduced (r.getWidth() * 0.08f), c);
}

juce::Image makeTrackImage (const juce::File& f)
{
    auto img = juce::ImageFileFormat::loadFrom (f);
    if (! img.isValid()) return {};
    const int side = juce::jmin (img.getWidth(), img.getHeight());
    return img.getClippedImage ({ (img.getWidth() - side) / 2, (img.getHeight() - side) / 2, side, side })
              .rescaled (96, 96, juce::Graphics::highResamplingQuality);
}

juce::String trackImageToBase64 (const juce::Image& img)
{
    juce::MemoryOutputStream mo;
    juce::PNGImageFormat png;
    return png.writeImageToStream (img, mo) ? juce::Base64::toBase64 (mo.getData(), mo.getDataSize()) : juce::String();
}

juce::Image trackImageFromBase64 (const juce::String& text)
{
    juce::MemoryOutputStream mo;
    if (text.isEmpty() || ! juce::Base64::convertFromBase64 (mo, text)) return {};
    return juce::ImageFileFormat::loadFrom (mo.getData(), mo.getDataSize());
}
} // namespace fc
