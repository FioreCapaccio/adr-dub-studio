#define DONT_SET_USING_JUCE_NAMESPACE 1   // evita conflitti con Point/Component dei framework Apple
#include "VideoSupport.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

namespace fc::video
{
namespace
{
NSURL* urlFor (const juce::File& f) { return [NSURL fileURLWithPath: [NSString stringWithUTF8String: f.getFullPathName().toRawUTF8()]]; }

juce::Image imageFromCG (CGImageRef cg)
{
    const int w = (int) CGImageGetWidth (cg), h = (int) CGImageGetHeight (cg);
    juce::Image img (juce::Image::ARGB, w, h, true);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate (bd.data, (size_t) w, (size_t) h, 8, (size_t) bd.lineStride, cs,
                                              kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextDrawImage (ctx, CGRectMake (0, 0, w, h), cg);
    CGContextRelease (ctx);
    CGColorSpaceRelease (cs);
    return img;
}
} // namespace

bool isVideoFile (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".mov" || ext == ".mp4" || ext == ".m4v" || ext == ".qt";
}

Info probe (const juce::File& f)
{
    Info info;
    @autoreleasepool
    {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL: urlFor (f) options: nil];
        NSArray<AVAssetTrack*>* vt = [asset tracksWithMediaType: AVMediaTypeVideo];
        if (vt.count == 0) { info.error = "Nessuna traccia video in " + f.getFileName(); return info; }

        AVAssetTrack* t = vt.firstObject;
        const double dur = CMTimeGetSeconds (asset.duration);
        if (! (dur > 0.0)) { info.error = "Durata del video non valida."; return info; }

        info.duration = dur;
        const float fps = t.nominalFrameRate;
        info.fps = fps > 1.0f ? (double) fps : 25.0;
        const CGSize sz = CGSizeApplyAffineTransform (t.naturalSize, t.preferredTransform);
        info.width = (int) std::abs (sz.width);
        info.height = (int) std::abs (sz.height);
        info.hasAudio = [asset tracksWithMediaType: AVMediaTypeAudio].count > 0;
        info.ok = true;
    }
    return info;
}

std::vector<juce::Image> thumbnails (const juce::File& f, double interval, int count, int height, const std::function<void (float)>& progress)
{
    std::vector<juce::Image> out;
    @autoreleasepool
    {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL: urlFor (f) options: nil];
        AVAssetImageGenerator* gen = [[AVAssetImageGenerator alloc] initWithAsset: asset];
        gen.appliesPreferredTrackTransform = YES;
        gen.maximumSize = CGSizeMake ((CGFloat) height * 4, (CGFloat) height);
        const CMTime tol = CMTimeMakeWithSeconds (interval * 0.5, 600);
        gen.requestedTimeToleranceBefore = tol;
        gen.requestedTimeToleranceAfter = tol;

        const double dur = CMTimeGetSeconds (asset.duration);
        for (int i = 0; i < count; ++i)
        {
            const double t = juce::jmin (dur - 0.05, ((double) i + 0.5) * interval);
            NSError* err = nil;
            CGImageRef cg = [gen copyCGImageAtTime: CMTimeMakeWithSeconds (juce::jmax (0.0, t), 600) actualTime: nullptr error: &err];
            if (cg != nullptr)
            {
                out.push_back (imageFromCG (cg));
                CGImageRelease (cg);
            }
            else
            {
                out.push_back (out.empty() ? juce::Image() : out.back());
            }
            if (progress) progress ((float) (i + 1) / (float) count);
        }
    }
    return out;
}

namespace
{
/** Composizione con il video (spostato dell'offset) e l'audio del mix; la composizione parte da rangeStartSec se c'e' un intervallo. nil + error se fallisce. */
AVMutableComposition* makeComposition (const juce::File& video, const juce::File& audio, double videoOffsetSec, double rangeStartSec, double rangeEndSec, juce::String& error)
{
    AVURLAsset* v = [AVURLAsset URLAssetWithURL: urlFor (video) options: nil];
    AVURLAsset* a = [AVURLAsset URLAssetWithURL: urlFor (audio) options: nil];
    AVAssetTrack* vt = [[v tracksWithMediaType: AVMediaTypeVideo] firstObject];
    AVAssetTrack* at = [[a tracksWithMediaType: AVMediaTypeAudio] firstObject];
    if (vt == nil) { error = "Il file video non contiene video."; return nil; }
    if (at == nil) { error = "Il file audio del mix non e' leggibile."; return nil; }

    AVMutableComposition* comp = [AVMutableComposition composition];
    NSError* err = nil;

    // offset: > 0 il video parte dopo l'audio, < 0 si tagliano i primi fotogrammi del video
    // con un intervallo la composizione parte da rangeStartSec: il tempo nel video e' (tempo timeline - offset)
    const bool ranged = rangeEndSec > rangeStartSec;
    const double rs = ranged ? rangeStartSec : 0.0;
    CMTime vSrcStart = kCMTimeZero, vAt = kCMTimeZero, vLen = v.duration;
    const double srcStartSec = juce::jmax (0.0, rs - videoOffsetSec), atSec = juce::jmax (0.0, videoOffsetSec - rs);
    if (atSec > 0.0) vAt = CMTimeMakeWithSeconds (atSec, 600);
    if (srcStartSec > 0.0) vSrcStart = CMTimeMakeWithSeconds (srcStartSec, 600);
    double lenSec = CMTimeGetSeconds (v.duration) - srcStartSec;
    if (ranged) lenSec = juce::jmin (lenSec, (rangeEndSec - rangeStartSec) - atSec);
    if (lenSec <= 0.0) { error = ranged ? "L'intervallo scelto non contiene video." : "L'offset del video supera la sua durata."; return nil; }
    if (ranged || srcStartSec > 0.0) vLen = CMTimeMakeWithSeconds (lenSec, 600);
    const CMTime endOfVideo = CMTimeAdd (vAt, vLen);

    AVMutableCompositionTrack* cv = [comp addMutableTrackWithMediaType: AVMediaTypeVideo preferredTrackID: kCMPersistentTrackID_Invalid];
    if (! [cv insertTimeRange: CMTimeRangeMake (vSrcStart, vLen) ofTrack: vt atTime: vAt error: &err])
    {
        error = juce::String::fromUTF8 (err.localizedDescription.UTF8String);
        return nil;
    }
    cv.preferredTransform = vt.preferredTransform;

    AVMutableCompositionTrack* ca = [comp addMutableTrackWithMediaType: AVMediaTypeAudio preferredTrackID: kCMPersistentTrackID_Invalid];
    const CMTime aDur = CMTimeMinimum (a.duration, endOfVideo);
    if (! [ca insertTimeRange: CMTimeRangeMake (kCMTimeZero, aDur) ofTrack: at atTime: kCMTimeZero error: &err])
    {
        error = juce::String::fromUTF8 (err.localizedDescription.UTF8String);
        return nil;
    }
    return comp;
}
} // namespace

bool muxAudio (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec,
               const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec, double rangeEndSec)
{
    @autoreleasepool
    {
        AVMutableComposition* comp = makeComposition (video, audio, videoOffsetSec, rangeStartSec, rangeEndSec, error);
        if (comp == nil) return false;

        AVAssetExportSession* s = [[AVAssetExportSession alloc] initWithAsset: comp presetName: AVAssetExportPresetPassthrough];
        if (s == nil) { error = "Impossibile creare la sessione di esportazione."; return false; }
        out.deleteFile();
        s.outputURL = urlFor (out);
        s.outputFileType = AVFileTypeQuickTimeMovie;
        s.shouldOptimizeForNetworkUse = NO;

        dispatch_semaphore_t sem = dispatch_semaphore_create (0);
        [s exportAsynchronouslyWithCompletionHandler: ^{ dispatch_semaphore_signal (sem); }];

        bool cancelled = false;
        while (dispatch_semaphore_wait (sem, dispatch_time (DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0)
        {
            if (progress && ! progress ((double) s.progress))
            {
                [s cancelExport];
                cancelled = true;
            }
        }

        if (cancelled) { out.deleteFile(); error = "Esportazione annullata."; return false; }
        if (s.status != AVAssetExportSessionStatusCompleted)
        {
            error = s.error != nil ? juce::String::fromUTF8 (s.error.localizedDescription.UTF8String) : juce::String ("Esportazione video non riuscita.");
            out.deleteFile();
            return false;
        }
    }
    return true;
}

//==============================================================================
// Burn-in: ogni fotogramma viene decodificato, scritto sopra con CoreText e ricodificato in H.264; l'audio e' il mix (AAC).
namespace
{
NSString* ns (const juce::String& s) { return [NSString stringWithUTF8String: s.toRawUTF8()]; }

CTLineRef makeLine (NSString* text, const char* fontName, CGFloat size, CGColorRef colour)
{
    CTFontRef font = CTFontCreateWithName ((__bridge CFStringRef) [NSString stringWithUTF8String: fontName], size, nullptr);
    NSDictionary* attrs = @{ (__bridge id) kCTFontAttributeName: (__bridge id) font, (__bridge id) kCTForegroundColorAttributeName: (__bridge id) colour };
    NSAttributedString* as = [[NSAttributedString alloc] initWithString: text attributes: attrs];
    CTLineRef line = CTLineCreateWithAttributedString ((__bridge CFAttributedStringRef) as);
    CFRelease (font);
    return line;
}

/** Scrive un testo su una scatola scura semitrasparente. anchor: 0 sinistra, 1 centro, 2 destra rispetto a x. Ritorna l'altezza usata. */
CGFloat drawBoxedText (CGContextRef ctx, NSString* text, const char* fontName, CGFloat size, CGFloat x, CGFloat yTop, int anchor, CGFloat width, CGFloat height)
{
    CGColorRef white = CGColorCreateGenericRGB (1, 1, 1, 1);
    CTLineRef line = makeLine (text, fontName, size, white);
    CGFloat asc = 0, desc = 0, lead = 0;
    const CGFloat tw = (CGFloat) CTLineGetTypographicBounds (line, &asc, &desc, &lead);
    const CGFloat pad = size * 0.45, bw = tw + pad * 2, bh = asc + desc + pad;
    CGFloat bx = anchor == 0 ? x : (anchor == 1 ? x - bw / 2 : x - bw);
    const CGFloat by = height - yTop - bh;   // origine in basso a sinistra
    CGContextSetRGBFillColor (ctx, 0, 0, 0, 0.62);
    CGPathRef path = CGPathCreateWithRoundedRect (CGRectMake (bx, by, bw, bh), size * 0.28, size * 0.28, nullptr);
    CGContextAddPath (ctx, path);
    CGContextFillPath (ctx);
    CGPathRelease (path);
    CGContextSetTextPosition (ctx, bx + pad, by + pad * 0.5 + desc);
    CTLineDraw (line, ctx);
    CFRelease (line);
    CGColorRelease (white);
    (void) width;
    return bh;
}

/** Spezza il testo in righe che stanno nella larghezza data. */
std::vector<juce::String> wrapText (const juce::String& text, const char* fontName, CGFloat size, CGFloat maxWidth)
{
    std::vector<juce::String> lines;
    auto words = juce::StringArray::fromTokens (text.replace ("\n", " "), " ", "");
    words.removeEmptyStrings();
    CGColorRef c = CGColorCreateGenericRGB (1, 1, 1, 1);
    juce::String cur;
    for (auto& w : words)
    {
        const juce::String trial = cur.isEmpty() ? w : cur + " " + w;
        CTLineRef l = makeLine (ns (trial), fontName, size, c);
        const bool fits = CTLineGetTypographicBounds (l, nullptr, nullptr, nullptr) <= maxWidth;
        CFRelease (l);
        if (fits || cur.isEmpty()) cur = trial; else { lines.push_back (cur); cur = w; }
    }
    if (cur.isNotEmpty()) lines.push_back (cur);
    CGColorRelease (c);
    return lines;
}

void drawBurnIn (CVPixelBufferRef pb, double timelineSec, const BurnIn& b)
{
    CVPixelBufferLockBaseAddress (pb, 0);
    const size_t w = CVPixelBufferGetWidth (pb), h = CVPixelBufferGetHeight (pb);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate (CVPixelBufferGetBaseAddress (pb), w, h, 8, CVPixelBufferGetBytesPerRow (pb), cs,
                                              kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease (cs);
    if (ctx != nullptr)
    {
        const CGFloat fs = (CGFloat) h * 0.034, margin = (CGFloat) h * 0.03;
        if (b.timecode && b.timecodeAt)
            drawBoxedText (ctx, ns (b.timecodeAt (timelineSec)), "Menlo-Bold", fs * 1.1, margin, margin, 0, (CGFloat) w, (CGFloat) h);

        const BurnInLine* active = nullptr;
        if (b.cues)
            for (auto& l : b.lines) if (timelineSec >= l.from && timelineSec < l.to) { active = &l; break; }
        if (active != nullptr)
        {
            juce::String who = active->character.isEmpty() ? juce::String() : active->character;
            if (active->number > 0) who << (who.isEmpty() ? "" : "  ") << "#" << active->number;
            if (who.isNotEmpty()) drawBoxedText (ctx, ns (who), "HelveticaNeue-Bold", fs, (CGFloat) w - margin, margin, 2, (CGFloat) w, (CGFloat) h);

            const auto lines = wrapText (active->text, "HelveticaNeue-Bold", fs * 1.25, (CGFloat) w * 0.86);
            CGFloat y = (CGFloat) h - margin - (CGFloat) lines.size() * fs * 1.95;
            for (auto& ln : lines)
            {
                y += drawBoxedText (ctx, ns (ln), "HelveticaNeue-Bold", fs * 1.25, (CGFloat) w / 2, y, 1, (CGFloat) w, (CGFloat) h) + fs * 0.12;
            }
        }
        CGContextRelease (ctx);
    }
    CVPixelBufferUnlockBaseAddress (pb, 0);
}
} // namespace

bool detectCuts (const juce::File& video, double sensitivity, const std::function<bool (double)>& progress, std::vector<double>& cuts, juce::String& error)
{
    cuts.clear();
    @autoreleasepool
    {
        AVURLAsset* a = [AVURLAsset URLAssetWithURL: urlFor (video) options: nil];
        AVAssetTrack* vt = [[a tracksWithMediaType: AVMediaTypeVideo] firstObject];
        if (vt == nil) { error = "Il file non contiene video."; return false; }
        NSError* err = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset: a error: &err];
        if (reader == nil) { error = "Impossibile leggere il video: " + juce::String::fromUTF8 (err.localizedDescription.UTF8String); return false; }
        constexpr int W = 64, H = 36;
        AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: vt
                                                                                   outputSettings: @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                                                                                      (id) kCVPixelBufferWidthKey: @(W), (id) kCVPixelBufferHeightKey: @(H) }];
        [reader addOutput: out];
        if (! [reader startReading]) { error = "Impossibile leggere il video."; return false; }

        const double thr = 0.55 - 0.40 * juce::jlimit (0.0, 1.0, sensitivity);
        const double total = juce::jmax (0.001, CMTimeGetSeconds (a.duration));
        std::vector<float> prevLum ((size_t) (W * H), 0.0f), lum ((size_t) (W * H), 0.0f);
        double prevHist[32] = {}, hist[32] = {};
        bool havePrev = false;
        std::vector<double> recent;   // punteggi recenti senza i tagli: la "normalita'" del movimento
        double lastCut = -10.0;

        while (CMSampleBufferRef sb = [out copyNextSampleBuffer])
        {
            const double t = CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sb));
            if (CVImageBufferRef pb = CMSampleBufferGetImageBuffer (sb))
            {
                CVPixelBufferLockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);
                const uint8_t* base = (const uint8_t*) CVPixelBufferGetBaseAddress (pb);
                const size_t stride = CVPixelBufferGetBytesPerRow (pb);
                const int w = (int) juce::jmin<size_t> (W, CVPixelBufferGetWidth (pb)), h = (int) juce::jmin<size_t> (H, CVPixelBufferGetHeight (pb));
                std::fill (std::begin (hist), std::end (hist), 0.0);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const uint8_t* px = base + (size_t) y * stride + (size_t) x * 4;   // BGRA
                        const float l = 0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2];
                        lum[(size_t) (y * W + x)] = l;
                        hist[juce::jmin (31, (int) l / 8)] += 1.0;
                    }
                CVPixelBufferUnlockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);
                const double n = (double) (w * h);
                for (auto& v : hist) v /= n;

                if (havePrev)
                {
                    double hd = 0.0, pd = 0.0;
                    for (int i = 0; i < 32; ++i) hd += std::abs (hist[i] - prevHist[i]);
                    hd *= 0.5;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) pd += std::abs (lum[(size_t) (y * W + x)] - prevLum[(size_t) (y * W + x)]);
                    pd /= n * 255.0;
                    const double score = 0.6 * hd + 0.4 * juce::jmin (1.0, pd * 4.0);
                    double avg = 0.0;
                    for (auto r : recent) avg += r;
                    avg = recent.empty() ? 0.0 : avg / (double) recent.size();
                    if (score > thr && score > 2.5 * avg + 0.05 && t - lastCut > 0.5)
                    {
                        cuts.push_back (t);
                        lastCut = t;
                    }
                    else
                    {
                        recent.push_back (score);
                        if (recent.size() > 8) recent.erase (recent.begin());
                    }
                }
                prevLum = lum;
                std::copy (std::begin (hist), std::end (hist), std::begin (prevHist));
                havePrev = true;
            }
            CFRelease (sb);
            if (progress && ! progress (juce::jlimit (0.0, 1.0, t / total))) { [reader cancelReading]; error = "Analisi annullata."; return false; }
        }
        if (reader.status == AVAssetReaderStatusFailed)
        {
            error = reader.error != nil ? juce::String::fromUTF8 (reader.error.localizedDescription.UTF8String) : juce::String ("Lettura del video non riuscita.");
            return false;
        }
    }
    return true;
}

bool burnInSupported() { return true; }

bool muxAudioBurnIn (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec, const BurnIn& burn,
                     const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec, double rangeEndSec)
{
    @autoreleasepool
    {
        AVMutableComposition* comp = makeComposition (video, audio, videoOffsetSec, rangeStartSec, rangeEndSec, error);
        if (comp == nil) return false;
        const double timeOffset = rangeEndSec > rangeStartSec ? rangeStartSec : 0.0;   // tempo della timeline all'istante 0 della composizione

        AVAssetTrack* cvt = [[comp tracksWithMediaType: AVMediaTypeVideo] firstObject];
        AVAssetTrack* cat = [[comp tracksWithMediaType: AVMediaTypeAudio] firstObject];
        NSError* err = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset: comp error: &err];
        if (reader == nil) { error = "Impossibile leggere il video: " + juce::String::fromUTF8 (err.localizedDescription.UTF8String); return false; }

        AVAssetReaderTrackOutput* vOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: cvt
                                                                                    outputSettings: @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) }];
        AVAssetReaderTrackOutput* aOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: cat
                                                                                    outputSettings: @{ AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO,
                                                                                                       AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO, AVNumberOfChannelsKey: @2 }];
        [reader addOutput: vOut];
        [reader addOutput: aOut];

        out.deleteFile();
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL: urlFor (out) fileType: AVFileTypeQuickTimeMovie error: &err];
        if (writer == nil) { error = "Impossibile creare il file: " + juce::String::fromUTF8 (err.localizedDescription.UTF8String); return false; }

        const int w = (int) cvt.naturalSize.width, h = (int) cvt.naturalSize.height;
        const double bitsPerSecond = juce::jlimit (4.0e6, 40.0e6, (double) w * h * 4.0);
        AVAssetWriterInput* vIn = [AVAssetWriterInput assetWriterInputWithMediaType: AVMediaTypeVideo
                                                                     outputSettings: @{ AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @(w), AVVideoHeightKey: @(h),
                                                                                        AVVideoCompressionPropertiesKey: @{ AVVideoAverageBitRateKey: @((int) bitsPerSecond),
                                                                                                                            AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel } }];
        vIn.transform = cvt.preferredTransform;
        vIn.expectsMediaDataInRealTime = NO;
        AVAssetWriterInputPixelBufferAdaptor* adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput: vIn
                                                                                                                         sourcePixelBufferAttributes: @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                                                                                                                                         (id) kCVPixelBufferWidthKey: @(w), (id) kCVPixelBufferHeightKey: @(h) }];
        AVAssetWriterInput* aIn = [AVAssetWriterInput assetWriterInputWithMediaType: AVMediaTypeAudio
                                                                     outputSettings: @{ AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVNumberOfChannelsKey: @2, AVSampleRateKey: @48000, AVEncoderBitRateKey: @192000 }];
        aIn.expectsMediaDataInRealTime = NO;
        [writer addInput: vIn];
        [writer addInput: aIn];

        if (! [reader startReading]) { error = "Impossibile leggere il video: " + juce::String::fromUTF8 (reader.error.localizedDescription.UTF8String); return false; }
        if (! [writer startWriting]) { error = "Impossibile scrivere il video: " + juce::String::fromUTF8 (writer.error.localizedDescription.UTF8String); return false; }
        [writer startSessionAtSourceTime: kCMTimeZero];

        const double totalSec = juce::jmax (0.001, CMTimeGetSeconds (comp.duration));
        bool vDone = false, aDone = false, cancelled = false, failed = false;
        while (! (vDone && aDone) && ! failed)
        {
            bool worked = false;
            if (! vDone && vIn.isReadyForMoreMediaData)
            {
                worked = true;
                CMSampleBufferRef sb = [vOut copyNextSampleBuffer];
                if (sb == nullptr) { [vIn markAsFinished]; vDone = true; }
                else
                {
                    const CMTime pts = CMSampleBufferGetPresentationTimeStamp (sb);
                    if (CVImageBufferRef pb = CMSampleBufferGetImageBuffer (sb))
                    {
                        drawBurnIn (pb, CMTimeGetSeconds (pts) + timeOffset, burn);
                        if (! [adaptor appendPixelBuffer: pb withPresentationTime: pts]) failed = true;
                    }
                    CFRelease (sb);
                    if (progress && ! progress (juce::jlimit (0.0, 1.0, CMTimeGetSeconds (pts) / totalSec))) { cancelled = true; break; }
                }
            }
            if (! aDone && aIn.isReadyForMoreMediaData)
            {
                worked = true;
                CMSampleBufferRef sb = [aOut copyNextSampleBuffer];
                if (sb == nullptr) { [aIn markAsFinished]; aDone = true; }
                else { if (! [aIn appendSampleBuffer: sb]) failed = true; CFRelease (sb); }
            }
            if (! worked) usleep (2000);
        }

        if (cancelled)
        {
            [reader cancelReading];
            [writer cancelWriting];
            out.deleteFile();
            error = "Esportazione annullata.";
            return false;
        }
        if (failed || writer.status == AVAssetWriterStatusFailed)
        {
            error = writer.error != nil ? juce::String::fromUTF8 (writer.error.localizedDescription.UTF8String) : juce::String ("Esportazione video non riuscita.");
            [reader cancelReading];
            [writer cancelWriting];
            out.deleteFile();
            return false;
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create (0);
        [writer finishWritingWithCompletionHandler: ^{ dispatch_semaphore_signal (sem); }];
        dispatch_semaphore_wait (sem, DISPATCH_TIME_FOREVER);
        if (writer.status != AVAssetWriterStatusCompleted)
        {
            error = writer.error != nil ? juce::String::fromUTF8 (writer.error.localizedDescription.UTF8String) : juce::String ("Esportazione video non riuscita.");
            out.deleteFile();
            return false;
        }
    }
    return true;
}

bool decodeAudioNative (const juce::File& f, double targetRate, juce::AudioBuffer<float>& out, const std::function<void (float)>& progress, juce::String& error)
{
    @autoreleasepool
    {
        AVURLAsset* a = [AVURLAsset URLAssetWithURL: urlFor (f) options: nil];
        NSArray<AVAssetTrack*>* tracks = [a tracksWithMediaType: AVMediaTypeAudio];
        if (tracks.count == 0) { error = "Il file non contiene audio."; return false; }

        int channels = 2;
        for (id desc in tracks.firstObject.formatDescriptions)
            if (const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription ((CMAudioFormatDescriptionRef) desc))
                channels = (int) juce::jlimit<UInt32> (1, 2, asbd->mChannelsPerFrame);

        NSError* err = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset: a error: &err];
        if (reader == nil) { error = "Impossibile aprire l'audio: " + juce::String::fromUTF8 (err.localizedDescription.UTF8String); return false; }
        NSDictionary* settings = @{ AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @32, AVLinearPCMIsFloatKey: @YES,
                                    AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO,
                                    AVNumberOfChannelsKey: @(channels), AVSampleRateKey: @(targetRate) };
        AVAssetReaderTrackOutput* o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: tracks.firstObject outputSettings: settings];
        [reader addOutput: o];
        if (! [reader startReading])
        {
            error = "Impossibile leggere l'audio: " + (reader.error != nil ? juce::String::fromUTF8 (reader.error.localizedDescription.UTF8String) : juce::String ("errore sconosciuto"));
            return false;
        }

        const double totalSec = juce::jmax (0.001, CMTimeGetSeconds (a.duration));
        std::vector<float> chan[2];
        const size_t reserve = (size_t) (totalSec * targetRate * 1.02) + 4096;
        for (int c = 0; c < channels; ++c) chan[c].reserve (reserve);

        while (CMSampleBufferRef sb = [o copyNextSampleBuffer])
        {
            CMBlockBufferRef bb = CMSampleBufferGetDataBuffer (sb);
            size_t len = 0;
            char* data = nullptr;
            if (bb != nullptr && CMBlockBufferGetDataPointer (bb, 0, nullptr, &len, &data) == kCMBlockBufferNoErr)
            {
                const float* fl = reinterpret_cast<const float*> (data);
                const size_t frames = len / (sizeof (float) * (size_t) channels);
                for (size_t i = 0; i < frames; ++i)
                    for (int c = 0; c < channels; ++c) chan[c].push_back (fl[i * (size_t) channels + (size_t) c]);
            }
            CFRelease (sb);
            if (progress) progress ((float) juce::jlimit (0.0, 1.0, (double) chan[0].size() / (targetRate * totalSec)));
        }
        if (reader.status == AVAssetReaderStatusFailed)
        {
            error = "Lettura dell'audio non riuscita: " + juce::String::fromUTF8 (reader.error.localizedDescription.UTF8String);
            return false;
        }
        if (chan[0].empty()) { error = "L'audio del file e' vuoto."; return false; }

        out.setSize (channels, (int) chan[0].size());
        for (int c = 0; c < channels; ++c) out.copyFrom (c, 0, chan[c].data(), (int) chan[c].size());
    }
    return true;
}

void setNativePlayerRate (juce::Component& videoComponent, double rate)
{
    for (int i = 0; i < videoComponent.getNumChildComponents(); ++i)
        if (auto* nv = dynamic_cast<juce::NSViewComponent*> (videoComponent.getChildComponent (i)))
        {
            id view = (__bridge id) nv->getView();
            if ([view respondsToSelector: @selector (player)])
            {
                AVPlayer* p = [view player];
                if (p != nil && p.rate != 0.0f) p.rate = (float) rate;
            }
            return;
        }
}

bool extractAudio (const juce::File&, const juce::File&, juce::String& error) { error = "Non usato su macOS."; return false; }
bool usesExternalDecoder() { return false; }
juce::String ffmpegStatus() { return {}; }
} // namespace fc::video
