// Prova dell'host ARA da riga di comando: ADR Dub Studio --ara plugin.vst3 entrata.wav uscita.wav
// Carica il plug-in, gli presenta il file come clip, lo fa renderizzare e scrive il risultato.
#include "AraEdit.h"

#if ADR_ARA
void runAraProbe (const juce::String& pluginPath, const juce::String& inPath, const juce::String& outPath, std::function<void (int)> done)
{
    using namespace juce;
    AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> rd (fm.createReaderFor (File (inPath)));
    if (rd == nullptr) { std::cout << "non riesco a leggere " << inPath << std::endl; done (1); return; }
    fc::ara::Source src;
    src.rate = rd->sampleRate; src.name = File (inPath).getFileName();
    const int chans = jmin (2, (int) rd->numChannels);
    src.audio.setSize (chans, (int) rd->lengthInSamples);
    rd->read (&src.audio, 0, (int) rd->lengthInSamples, 0, true, chans > 1);
    std::cout << "clip: " << chans << " canali, " << src.rate << " Hz, " << src.audio.getNumSamples() << " campioni" << std::endl;
    auto in = std::make_shared<AudioBuffer<float>> (src.audio);
    const double rate = src.rate;

    fc::ara::Session::create (std::move (src), File (pluginPath), [in, rate, outPath, done] (std::unique_ptr<fc::ara::Session> s, String err)
    {
        if (s == nullptr) { std::cout << "ARA non disponibile: " << err << std::endl; done (2); return; }
        std::cout << "agganciato: " << s->pluginName() << ", editor " << (s->hasEditor() ? "presente" : "assente") << std::endl;
        auto out = s->render();
        double ri = 0, ro = 0;
        for (int c = 0; c < in->getNumChannels(); ++c) ri += in->getRMSLevel (c, 0, in->getNumSamples());
        for (int c = 0; c < 2; ++c) ro += out.getRMSLevel (c, 0, out.getNumSamples());
        std::cout << "RMS ingresso " << ri / in->getNumChannels() << "  uscita " << ro / 2 << std::endl;
        File of (outPath); of.deleteFile();
        if (auto fs = of.createOutputStream())
        {
            WavAudioFormat wav;
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (fs.release(), rate, 2, 24, {}, 0));
            if (w != nullptr) w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
        }
        std::cout << "scritto " << outPath << std::endl;
        s.reset();
        done (0);
    });
}

/** --ara-ui plugin.vst3 entrata.wav uscita.wav: apre la finestra ARA del plug-in; "Applica" scrive l'audio modificato in uscita.wav. */
void runAraUi (const juce::String& pluginPath, const juce::String& inPath, const juce::String& outPath, std::function<void (int)> done)
{
    using namespace juce;
    static AudioDeviceManager dm;
    dm.initialiseWithDefaultDevices (0, 2);
    AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> rd (fm.createReaderFor (File (inPath)));
    if (rd == nullptr) { done (1); return; }
    fc::ara::Source src;
    src.rate = rd->sampleRate; src.name = File (inPath).getFileName();
    const int chans = jmin (2, (int) rd->numChannels);
    src.audio.setSize (chans, (int) rd->lengthInSamples);
    rd->read (&src.audio, 0, (int) rd->lengthInSamples, 0, true, chans > 1);
    const double rate = src.rate;
    fc::ara::openWindow (std::move (src), File (pluginPath), dm, nullptr,
        [rate, outPath, done] (AudioBuffer<float> out)
        {
            File of (outPath); of.deleteFile();
            if (auto fs = of.createOutputStream()) { WavAudioFormat wav; std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (fs.release(), rate, 2, 24, {}, 0)); if (w != nullptr) w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples()); }
            std::cout << "applicato: " << outPath << std::endl;
            done (0);
        },
        [done] (String e) { std::cout << "errore: " << e << std::endl; done (2); });
}
#endif
