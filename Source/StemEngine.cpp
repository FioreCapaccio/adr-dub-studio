#include "StemEngine.h"
#include "third_party/onnxruntime/onnxruntime_c_api.h"
#include <cmath>
#include <complex>
#include <thread>

namespace fc::stems
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// ─── FFT complessa radix-2 (2048 punti), con tavole calcolate una volta ───
struct FftTables
{
    std::vector<std::complex<float>> tw;   // e^{-2 pi i k / N}, k < N/2
    std::vector<int> rev;
    std::vector<float> window;             // Hann periodica
    float winNorm = 1.0f;                  // sqrt(somma di window^2): torchaudio normalized=True

    FftTables()
    {
        tw.resize (kFft / 2);
        for (int k = 0; k < kFft / 2; ++k) tw[(size_t) k] = std::polar (1.0f, (float) (-2.0 * kPi * k / kFft));
        rev.resize (kFft);
        int bits = 0;
        while ((1 << bits) < kFft) ++bits;
        for (int i = 0; i < kFft; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        window.resize (kFft);
        double s2 = 0.0;
        for (int n = 0; n < kFft; ++n) { window[(size_t) n] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * n / kFft)); s2 += (double) window[(size_t) n] * window[(size_t) n]; }
        winNorm = (float) std::sqrt (s2);
    }
};

const FftTables& tables() { static const FftTables t; return t; }

void fft (std::complex<float>* a, bool inverse)   // inversa senza scala
{
    const auto& t = tables();
    for (int i = 0; i < kFft; ++i) { const int j = t.rev[(size_t) i]; if (j > i) std::swap (a[i], a[j]); }
    for (int len = 2; len <= kFft; len <<= 1)
    {
        const int half = len / 2, step = kFft / len;
        for (int i = 0; i < kFft; i += len)
            for (int k = 0; k < half; ++k)
            {
                auto w = t.tw[(size_t) (k * step)];
                if (inverse) w = std::conj (w);
                const auto u = a[i + k], v = a[i + k + half] * w;
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
}

// Ricampionatore sinc con finestra di Kaiser (32 passaggi per lo zero per lato), centrato: il campione 0 resta al suo posto.
double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
    return sum;
}

std::vector<float> resampleSinc (const float* in, int64_t nIn, double srcRate, double dstRate, int64_t nOut)
{
    constexpr int halfWidth = 32, tableRes = 512;
    constexpr double beta = 9.0;
    const double fc = juce::jmin (1.0, dstRate / srcRate);                 // abbassando la frequenza si taglia sotto la nuova Nyquist
    static const std::vector<float> table = []
    {
        std::vector<float> t ((size_t) halfWidth * tableRes + 2);
        const double i0b = besselI0 (beta);
        for (size_t i = 0; i < t.size(); ++i)
        {
            const double u = (double) i / tableRes;                          // in unita' di passaggi per lo zero
            const double sinc = u < 1.0e-9 ? 1.0 : std::sin (kPi * u) / (kPi * u);
            const double r = u / halfWidth;
            t[i] = (float) (r >= 1.0 ? 0.0 : sinc * besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b);
        }
        return t;
    }();
    const double step = srcRate / dstRate;
    const int taps = (int) std::ceil (halfWidth / fc);
    std::vector<float> out ((size_t) nOut, 0.0f);
    for (int64_t k = 0; k < nOut; ++k)
    {
        const double pos = (double) k * step;
        const int64_t c = (int64_t) std::floor (pos);
        double acc = 0.0;
        for (int j = -taps + 1; j <= taps; ++j)
        {
            const int64_t i = c + j;
            if (i < 0 || i >= nIn) continue;
            const double u = std::abs (((double) i - pos) * fc) * tableRes;
            if (u >= (double) halfWidth * tableRes) continue;
            const size_t ui = (size_t) u;
            const double fr = u - (double) ui;
            acc += (double) in[i] * ((double) table[ui] * (1.0 - fr) + (double) table[ui + 1] * fr);
        }
        out[(size_t) k] = (float) (acc * fc);
    }
    return out;
}

inline int reflectIndex (int64_t i, int64_t n)   // riflessione senza ripetere il bordo (come pad 'reflect'), valida anche oltre un'oscillazione
{
    if (n <= 1) return 0;
    const int64_t period = 2 * (n - 1);
    int64_t m = ((i % period) + period) % period;
    if (m >= n) m = period - m;
    return (int) m;
}
} // namespace

void stftChunk (const float* x, float* spec)
{
    const auto& t = tables();
    std::vector<std::complex<float>> a ((size_t) kFft);
    const float scale = 1.0f / t.winNorm;
    for (int k = 0; k < kFrames; ++k)
    {
        const int start = k * kHop - kFft / 2;
        for (int n = 0; n < kFft; ++n) a[(size_t) n] = { x[reflectIndex (start + n, kChunk)] * t.window[(size_t) n], 0.0f };
        fft (a.data(), false);
        for (int f = 0; f < kBins; ++f)
        {
            float* p = spec + ((size_t) f * kFrames + (size_t) k) * 2;
            p[0] = a[(size_t) f].real() * scale;
            p[1] = a[(size_t) f].imag() * scale;
        }
    }
}

void istftChunk (const float* spec, float* y)
{
    const auto& t = tables();
    std::vector<std::complex<float>> a ((size_t) kFft);
    std::vector<float> acc ((size_t) kChunk + kFft, 0.0f), wsum ((size_t) kChunk + kFft, 0.0f);
    const float scale = t.winNorm / (float) kFft;   // irfft = (1/N) * FFT inversa; poi si rimette la normalizzazione
    for (int k = 0; k < kFrames; ++k)
    {
        for (int f = 0; f < kBins; ++f)
        {
            const float* p = spec + ((size_t) f * kFrames + (size_t) k) * 2;
            a[(size_t) f] = { p[0], p[1] };
        }
        a[0].imag (0.0f);
        a[(size_t) kFft / 2].imag (0.0f);
        for (int f = 1; f < kFft / 2; ++f) a[(size_t) (kFft - f)] = std::conj (a[(size_t) f]);
        fft (a.data(), true);
        const size_t base = (size_t) k * kHop;
        for (int n = 0; n < kFft; ++n)
        {
            acc[base + (size_t) n] += a[(size_t) n].real() * scale * t.window[(size_t) n];
            wsum[base + (size_t) n] += t.window[(size_t) n] * t.window[(size_t) n];
        }
    }
    for (int i = 0; i < kChunk; ++i)
    {
        const size_t j = (size_t) i + kFft / 2;
        y[i] = wsum[j] > 1.0e-8f ? acc[j] / wsum[j] : 0.0f;
    }
}

// ─── Separazione a blocchi ───
bool separate (const float* const* channels, int numChannels, int64_t numSamples, double srcRate, const Options& opt,
               const std::array<juce::File, kStems>& outFiles, const CoreFn& core,
               const std::function<bool (float)>& progress, juce::String& error)
{
    if (numChannels < 1 || numChannels > 2) { error = TRANS ("Servono uno o due canali."); return false; }
    if (numSamples < 2 || srcRate <= 0.0) { error = TRANS ("Audio troppo corto."); return false; }

    // 1) a 48 kHz
    std::vector<std::vector<float>> in ((size_t) numChannels);
    int64_t n = numSamples;
    if (std::abs (srcRate - (double) kRate) < 0.5)
        for (int c = 0; c < numChannels; ++c) in[(size_t) c].assign (channels[c], channels[c] + numSamples);
    else
    {
        n = (int64_t) std::llround ((double) numSamples * kRate / srcRate);
        for (int c = 0; c < numChannels; ++c) in[(size_t) c] = resampleSinc (channels[c], numSamples, srcRate, (double) kRate, n);
    }

    // 2) file di uscita
    juce::WavAudioFormat wav;
    std::array<std::unique_ptr<juce::AudioFormatWriter>, kStems> writers;
    for (int s = 0; s < kStems; ++s)
    {
        outFiles[(size_t) s].getParentDirectory().createDirectory();
        outFiles[(size_t) s].deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (outFiles[(size_t) s].createOutputStream());
        if (os == nullptr) { error = TRANS ("Impossibile scrivere ") + outFiles[(size_t) s].getFullPathName(); return false; }
        writers[(size_t) s].reset (wav.createWriterFor (os.get(), (double) kRate, (unsigned) numChannels, 24, {}, 0));
        if (writers[(size_t) s] == nullptr) { error = TRANS ("Impossibile scrivere ") + outFiles[(size_t) s].getFullPathName(); return false; }
        os.release();
    }

    // 3) blocchi con sovrapposizione: finestra con rampe che sommano 1
    const int hop = (opt.highQuality ? 4 : 6) * kRate;
    const int ovl = kChunk - hop;
    std::vector<float> win ((size_t) kChunk, 1.0f);
    for (int i = 0; i < ovl; ++i)
    {
        const float up = (float) (0.5 - 0.5 * std::cos (kPi * (i + 0.5) / ovl));
        win[(size_t) i] = up;
        win[(size_t) (kChunk - ovl + i)] = 1.0f - up;
    }
    const int64_t nChunks = (n + kChunk - 2 * (int64_t) hop + hop - 1) / hop + 1;

    const size_t specSize = (size_t) kBins * kFrames * 2;
    const int W = juce::jlimit (1, 8, opt.workers);

    // Lavoro di un blocco (si puo' fare in parallelo): ritorna i tre stem del blocco, per canale, gia' per le finestre di sovrapposizione.
    struct Scratch
    {
        std::vector<float> chData, mid, spec, est, chSpec, tmpSpec, tmpOut, g;
        std::vector<std::vector<std::vector<float>>> out;   // [stem][canale][kChunk]
        bool silent = true;
        juce::String error;
        bool ok = true;
    };
    std::vector<Scratch> scratch ((size_t) W);
    for (auto& sc : scratch)
    {
        sc.chData.resize ((size_t) numChannels * kChunk);
        sc.mid.resize ((size_t) kChunk);
        sc.spec.resize (specSize);
        sc.est.resize ((size_t) kStems * specSize);
        sc.chSpec.resize (specSize);
        sc.tmpSpec.resize (specSize);
        sc.tmpOut.resize ((size_t) kChunk);
        sc.g.resize ((size_t) kStems * kBins * kFrames * 2);
        sc.out.assign ((size_t) kStems, std::vector<std::vector<float>> ((size_t) numChannels, std::vector<float> ((size_t) kChunk, 0.0f)));
    }

    auto processChunk = [&] (int64_t ci, Scratch& sc)
    {
        const int64_t s0 = ci * hop - ovl;   // inizio (virtuale) del blocco
        sc.silent = true;
        sc.ok = true;
        for (int c = 0; c < numChannels; ++c)
            for (int j = 0; j < kChunk; ++j)
            {
                const float v = in[(size_t) c][(size_t) reflectIndex (s0 + j, n)];
                sc.chData[(size_t) c * kChunk + (size_t) j] = v;
                if (std::abs (v) > 1.0e-6f) sc.silent = false;
            }
        if (sc.silent) return;   // silenzio: stem a zero, il modello non serve
        for (int j = 0; j < kChunk; ++j)
        {
            float m = 0.0f;
            for (int c = 0; c < numChannels; ++c) m += sc.chData[(size_t) c * kChunk + (size_t) j];
            sc.mid[(size_t) j] = m / (float) numChannels;
        }
        stftChunk (sc.mid.data(), sc.spec.data());
        if (! core (sc.spec.data(), sc.est.data(), sc.error)) { sc.ok = false; return; }
        // maschere complesse che sommano 1: stima_k / somma delle stime (dove le stime si annullano a vicenda si ripiega su |stima_k|^2 / somma)
        for (size_t i = 0; i < (size_t) kBins * kFrames; ++i)
        {
            float tr = 0.0f, ti = 0.0f, pw = 1.0e-20f, p[kStems];
            for (int st = 0; st < kStems; ++st)
            {
                const float* e = sc.est.data() + (size_t) st * specSize + i * 2;
                tr += e[0]; ti += e[1];
                p[st] = e[0] * e[0] + e[1] * e[1];
                pw += p[st];
            }
            const float t2 = tr * tr + ti * ti;
            for (int st = 0; st < kStems; ++st)
            {
                const float* e = sc.est.data() + (size_t) st * specSize + i * 2;
                float* gp = sc.g.data() + ((size_t) st * kBins * kFrames + i) * 2;
                if (t2 > 0.01f * pw) { gp[0] = (e[0] * tr + e[1] * ti) / t2; gp[1] = (e[1] * tr - e[0] * ti) / t2; }   // e / somma
                else { gp[0] = p[st] / pw; gp[1] = 0.0f; }
            }
        }
        for (int c = 0; c < numChannels; ++c)
        {
            stftChunk (sc.chData.data() + (size_t) c * kChunk, sc.chSpec.data());
            for (int st = 0; st < kStems; ++st)
            {
                const float* gs = sc.g.data() + (size_t) st * kBins * kFrames * 2;
                for (size_t i = 0; i < (size_t) kBins * kFrames; ++i)
                {
                    const float xr = sc.chSpec[i * 2], xi = sc.chSpec[i * 2 + 1], gr = gs[i * 2], gi = gs[i * 2 + 1];
                    sc.tmpSpec[i * 2] = xr * gr - xi * gi;
                    sc.tmpSpec[i * 2 + 1] = xr * gi + xi * gr;
                }
                istftChunk (sc.tmpSpec.data(), sc.out[(size_t) st][(size_t) c].data());
            }
        }
    };

    std::vector<std::vector<std::vector<float>>> acc ((size_t) kStems, std::vector<std::vector<float>> ((size_t) numChannels, std::vector<float> ((size_t) kChunk, 0.0f)));
    std::vector<float> wacc ((size_t) kChunk, 0.0f);
    int64_t flushed = 0;   // campioni finali gia' scritti

    auto writeFinal = [&] (int64_t upTo, int64_t chunkStart)   // scrive i campioni [flushed, upTo) che stanno in acc (relativo a chunkStart)
    {
        upTo = juce::jmin (upTo, n);
        const int64_t cnt = upTo - flushed;
        if (cnt <= 0) return;
        std::vector<std::vector<float>> out ((size_t) numChannels, std::vector<float> ((size_t) cnt));
        for (int st = 0; st < kStems; ++st)
        {
            std::vector<const float*> ptrs;
            for (int c = 0; c < numChannels; ++c)
            {
                for (int64_t i = 0; i < cnt; ++i)
                {
                    const size_t j = (size_t) (flushed + i - chunkStart);
                    out[(size_t) c][(size_t) i] = wacc[j] > 1.0e-6f ? acc[(size_t) st][(size_t) c][j] / wacc[j] : 0.0f;
                }
                ptrs.push_back (out[(size_t) c].data());
            }
            writers[(size_t) st]->writeFromFloatArrays (ptrs.data(), numChannels, (int) cnt);
        }
        flushed = upTo;
    };

    for (int64_t base = 0; base < nChunks && flushed < n; base += W)
    {
        if (progress && ! progress ((float) base / (float) nChunks)) { error = TRANS ("Annullato."); return false; }
        const int cnt = (int) juce::jmin<int64_t> (W, nChunks - base);
        if (cnt == 1) processChunk (base, scratch[0]);
        else
        {
            std::vector<std::thread> pool;
            for (int k = 0; k < cnt; ++k) pool.emplace_back ([&, k] { processChunk (base + k, scratch[(size_t) k]); });
            for (auto& t : pool) t.join();
        }
        for (int k = 0; k < cnt; ++k)   // in ordine: somma con la finestra e scrive cio' che e' definitivo
        {
            auto& sc = scratch[(size_t) k];
            if (! sc.ok) { error = sc.error; return false; }
            const int64_t ci = base + k;
            const int64_t s0 = ci * hop - ovl;
            if (! sc.silent)
                for (int st = 0; st < kStems; ++st)
                    for (int c = 0; c < numChannels; ++c)
                    {
                        auto& a = acc[(size_t) st][(size_t) c];
                        const auto& o = sc.out[(size_t) st][(size_t) c];
                        for (int j = 0; j < kChunk; ++j) a[(size_t) j] += o[(size_t) j] * win[(size_t) j];
                    }
            for (int j = 0; j < kChunk; ++j) wacc[(size_t) j] += win[(size_t) j];

            writeFinal (s0 + hop, s0);   // i campioni prima del prossimo blocco sono definitivi

            for (auto& st : acc) for (auto& ch : st)   // fa scorrere i buffer di un passo
            {
                std::memmove (ch.data(), ch.data() + hop, (size_t) ovl * sizeof (float));
                std::fill (ch.begin() + ovl, ch.end(), 0.0f);
            }
            std::memmove (wacc.data(), wacc.data() + hop, (size_t) ovl * sizeof (float));
            std::fill (wacc.begin() + ovl, wacc.end(), 0.0f);
        }
    }
    if (progress) progress (1.0f);
    for (auto& w : writers) w.reset();
    return true;
}

bool mixFiles (const juce::File& a, const juce::File& b, const juce::File& out, juce::String& error)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> ra (wav.createReaderFor (a.createInputStream().release(), true)), rb (wav.createReaderFor (b.createInputStream().release(), true));
    if (ra == nullptr || rb == nullptr || ra->numChannels != rb->numChannels) { error = TRANS ("File degli stem non validi."); return false; }
    out.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (out.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w;
    if (os != nullptr) w.reset (wav.createWriterFor (os.get(), ra->sampleRate, ra->numChannels, 24, {}, 0));
    if (w == nullptr) { error = TRANS ("Impossibile scrivere ") + out.getFullPathName(); return false; }
    os.release();
    const int64_t total = juce::jmin (ra->lengthInSamples, rb->lengthInSamples);
    juce::AudioBuffer<float> x ((int) ra->numChannels, 1 << 16), y ((int) ra->numChannels, 1 << 16);
    for (int64_t pos = 0; pos < total; pos += x.getNumSamples())
    {
        const int n = (int) juce::jmin<int64_t> (x.getNumSamples(), total - pos);
        ra->read (&x, 0, n, pos, true, true);
        rb->read (&y, 0, n, pos, true, true);
        for (int c = 0; c < x.getNumChannels(); ++c) juce::FloatVectorOperations::add (x.getWritePointer (c), y.getReadPointer (c), n);
        w->writeFromAudioSampleBuffer (x, 0, n);
    }
    return true;
}

// ─── Scaricamento di modello e libreria ───
namespace
{
const char* kBaseUrl = "https://pub-5b43b8b19f644fc68ae407c9ab0b853f.r2.dev/downloads/adr-stems/1/";

struct PackFile { const char* remote; const char* local; const char* sha256; int64_t size; };

#if JUCE_MAC
 #if defined (__arm64__) || defined (__aarch64__)
  constexpr PackFile kRuntime[] = { { "libonnxruntime-1.23.2-osx-arm64.dylib", "libonnxruntime.dylib", "d306d2bc768540766c7ed8a1e0ff05d2870c77a934ebeee4a7bafa1b732ef299", 35138784 } };
 #else
  constexpr PackFile kRuntime[] = { { "libonnxruntime-1.23.2-osx-x86_64.dylib", "libonnxruntime.dylib", "8c9c78de65ea3786f987c0d980e9c1b13a3a5fbc6b3e2965ba05b450e6e4c054", 39742608 } };
 #endif
#elif JUCE_WINDOWS
constexpr PackFile kRuntime[] = { { "onnxruntime-1.23.2-win-x64.dll", "onnxruntime.dll", "dec964ab1ee36cc9b0ae247d13b376627992fc57dec0454354017ab8fd84f1ea", 14186016 },
                                  { "onnxruntime_providers_shared-1.23.2-win-x64.dll", "onnxruntime_providers_shared.dll", "a2b3a50956aa75a9879c8472bc7df4f7a8072bcd2db19a1b7d988e7688f293ef", 22088 } };
#else
constexpr PackFile kRuntime[] = { { "", "", "", 0 } };
#endif
constexpr PackFile kModel = { "bandit-v2-multi-fp16.onnx", "bandit-v2-multi-fp16.onnx", "8044cc5e02ea38cd659c71c1948a945f474cdff7e45758e86f515a541e8d8d5d", 75172017 };
constexpr PackFile kNotices[] = { { "MODEL-LICENSE.txt", "MODEL-LICENSE.txt", "ef12f42ca351bde3ce8a442cd6fc2cbce30ad2b5f30f2acdbd279c60cff17b9c", 991 },
                                  { "onnxruntime-LICENSE-and-NOTICES.txt", "onnxruntime-LICENSE-and-NOTICES.txt", "c90dc142f577b67fe50c85395316c8498aec2cb5c90c0eead2453b2ca8c617ad", 327946 } };

std::vector<PackFile> allFiles()
{
    std::vector<PackFile> v { kModel };
    for (auto& r : kRuntime) if (r.remote[0] != 0) v.push_back (r);
    for (auto& n : kNotices) v.push_back (n);
    return v;
}

juce::String sha256Of (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return {};
    return juce::SHA256 (in).toHexString();
}

bool fileOk (const PackFile& p)
{
    const auto f = packDir().getChildFile (p.local);
    return f.existsAsFile() && f.getSize() == p.size;   // l'impronta si controlla al momento dello scaricamento
}
} // namespace

juce::File packDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    d = d.getChildFile ("Application Support");
#endif
    return d.getChildFile ("ArotronStudio").getChildFile ("engine");
}

bool packReady()
{
    for (auto& p : allFiles()) if (! fileOk (p)) return false;
    return true;
}

int packDownloadMB()
{
    int64_t total = 0;
    for (auto& p : allFiles()) if (! fileOk (p)) total += p.size;
    return (int) ((total + 1000000) / 1000000);
}

bool downloadPack (const std::function<bool (float, const juce::String&)>& progress, juce::String& error)
{
    const auto files = allFiles();
    int64_t total = 0, done = 0;
    for (auto& p : files) if (! fileOk (p)) total += p.size;
    packDir().createDirectory();

    for (auto& p : files)
    {
        if (fileOk (p)) continue;
        const juce::URL url (juce::String (kBaseUrl) + p.remote);
        auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs (20000).withNumRedirectsToFollow (5));
        if (in == nullptr) { error = TRANS ("Non riesco a collegarmi per scaricare il motore di separazione. Controlla la connessione a internet."); return false; }

        const auto tmp = packDir().getChildFile (juce::String (p.local) + ".part");
        tmp.deleteFile();
        {
            juce::FileOutputStream out (tmp);
            if (! out.openedOk()) { error = TRANS ("Impossibile scrivere ") + tmp.getFullPathName(); return false; }
            std::vector<char> buf (256 * 1024);
            while (! in->isExhausted())
            {
                const int r = in->read (buf.data(), (int) buf.size());
                if (r <= 0) break;
                if (! out.write (buf.data(), (size_t) r)) { error = TRANS ("Spazio su disco insufficiente."); tmp.deleteFile(); return false; }
                done += r;
                if (progress && ! progress ((float) done / (float) juce::jmax<int64_t> (1, total), juce::String (p.local)))
                {
                    out.flush();
                    tmp.deleteFile();
                    error = TRANS ("Annullato.");
                    return false;
                }
            }
        }
        if (tmp.getSize() != p.size || sha256Of (tmp) != juce::String (p.sha256))
        {
            tmp.deleteFile();
            error = TRANS ("Il file scaricato non e' valido (") + juce::String (p.local) + TRANS ("). Riprova.");
            return false;
        }
        const auto dest = packDir().getChildFile (p.local);
        dest.deleteFile();
        if (! tmp.moveFileTo (dest)) { error = TRANS ("Impossibile salvare ") + dest.getFullPathName(); return false; }
    }
    return true;
}

// ─── ONNX Runtime (caricato a richiesta) ───
struct OnnxCore::Impl
{
    juce::DynamicLibrary lib;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* opts = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* mem = nullptr;

    ~Impl()
    {
        if (api == nullptr) return;
        if (session) api->ReleaseSession (session);
        if (opts) api->ReleaseSessionOptions (opts);
        if (mem) api->ReleaseMemoryInfo (mem);
        if (env) api->ReleaseEnv (env);
    }

    juce::String msg (OrtStatus* st)
    {
        juce::String m = api->GetErrorMessage (st);
        api->ReleaseStatus (st);
        return m;
    }
};

OnnxCore::OnnxCore() : impl (std::make_unique<Impl>()) {}
OnnxCore::~OnnxCore() = default;

bool OnnxCore::load (juce::String& error)
{
    if (! packReady()) { error = TRANS ("Il motore di separazione non e' ancora stato scaricato."); return false; }
    auto& d = *impl;
    const auto libFile = packDir().getChildFile (
#if JUCE_MAC
        "libonnxruntime.dylib"
#else
        "onnxruntime.dll"
#endif
    );
    if (! d.lib.open (libFile.getFullPathName())) { error = TRANS ("Non riesco a caricare ONNX Runtime."); return false; }
    using GetBase = const OrtApiBase* (*) (void);
    auto getBase = (GetBase) d.lib.getFunction ("OrtGetApiBase");
    if (getBase == nullptr) { error = TRANS ("ONNX Runtime non valido."); return false; }
    d.api = getBase()->GetApi (ORT_API_VERSION);
    if (d.api == nullptr) { error = TRANS ("Versione di ONNX Runtime non compatibile."); return false; }

    if (auto* st = d.api->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "adr-dub-studio", &d.env)) { error = d.msg (st); return false; }
    if (auto* st = d.api->CreateSessionOptions (&d.opts)) { error = d.msg (st); return false; }
    d.api->SetIntraOpNumThreads (d.opts, 1);   // il parallelismo si fa fra blocchi (le reti ricorrenti scalano male dentro un blocco)
    d.api->SetSessionGraphOptimizationLevel (d.opts, ORT_ENABLE_ALL);
    const auto model = packDir().getChildFile (kModel.local);
#if JUCE_WINDOWS
    auto* st = d.api->CreateSession (d.env, model.getFullPathName().toWideCharPointer(), d.opts, &d.session);
#else
    auto* st = d.api->CreateSession (d.env, model.getFullPathName().toRawUTF8(), d.opts, &d.session);
#endif
    if (st) { error = d.msg (st); return false; }
    if (auto* st2 = d.api->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &d.mem)) { error = d.msg (st2); return false; }
    return true;
}

int OnnxCore::suggestedWorkers() { return juce::jlimit (1, 8, juce::SystemStats::getNumPhysicalCpus()); }

CoreFn OnnxCore::fn()
{
    return [this] (const float* spec, float* est, juce::String& error) -> bool
    {
        auto& d = *impl;
        const int64_t inShape[4] = { 1, kBins, kFrames, 2 };
        OrtValue* in = nullptr;
        if (auto* st = d.api->CreateTensorWithDataAsOrtValue (d.mem, const_cast<float*> (spec), (size_t) kBins * kFrames * 2 * sizeof (float), inShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in))
        { error = d.msg (st); return false; }
        const char* inNames[] = { "spec" };
        const char* outNames[] = { "est" };
        OrtValue* out = nullptr;
        auto* st = d.api->Run (d.session, nullptr, inNames, &in, 1, outNames, 1, &out);
        d.api->ReleaseValue (in);
        if (st) { error = d.msg (st); return false; }
        float* p = nullptr;
        if (auto* st2 = d.api->GetTensorMutableData (out, (void**) &p)) { error = d.msg (st2); d.api->ReleaseValue (out); return false; }
        std::memcpy (est, p, (size_t) kStems * kBins * kFrames * 2 * sizeof (float));
        d.api->ReleaseValue (out);
        return true;
    };
}
} // namespace fc::stems
