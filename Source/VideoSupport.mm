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

std::vector<juce::Image> thumbnails (const juce::File& f, double interval, int count, int height)
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
        }
    }
    return out;
}

bool muxAudio (const juce::File& video, const juce::File& audio, const juce::File& out, double videoOffsetSec,
               const std::function<bool (double)>& progress, juce::String& error, double rangeStartSec, double rangeEndSec)
{
    @autoreleasepool
    {
        AVURLAsset* v = [AVURLAsset URLAssetWithURL: urlFor (video) options: nil];
        AVURLAsset* a = [AVURLAsset URLAssetWithURL: urlFor (audio) options: nil];
        AVAssetTrack* vt = [[v tracksWithMediaType: AVMediaTypeVideo] firstObject];
        AVAssetTrack* at = [[a tracksWithMediaType: AVMediaTypeAudio] firstObject];
        if (vt == nil) { error = "Il file video non contiene video."; return false; }
        if (at == nil) { error = "Il file audio del mix non e' leggibile."; return false; }

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
        if (lenSec <= 0.0) { error = ranged ? "L'intervallo scelto non contiene video." : "L'offset del video supera la sua durata."; return false; }
        if (ranged || srcStartSec > 0.0) vLen = CMTimeMakeWithSeconds (lenSec, 600);
        const CMTime endOfVideo = CMTimeAdd (vAt, vLen);

        AVMutableCompositionTrack* cv = [comp addMutableTrackWithMediaType: AVMediaTypeVideo preferredTrackID: kCMPersistentTrackID_Invalid];
        if (! [cv insertTimeRange: CMTimeRangeMake (vSrcStart, vLen) ofTrack: vt atTime: vAt error: &err])
        {
            error = juce::String::fromUTF8 (err.localizedDescription.UTF8String);
            return false;
        }
        cv.preferredTransform = vt.preferredTransform;

        AVMutableCompositionTrack* ca = [comp addMutableTrackWithMediaType: AVMediaTypeAudio preferredTrackID: kCMPersistentTrackID_Invalid];
        const CMTime aDur = CMTimeMinimum (a.duration, endOfVideo);
        if (! [ca insertTimeRange: CMTimeRangeMake (kCMTimeZero, aDur) ofTrack: at atTime: kCMTimeZero error: &err])
        {
            error = juce::String::fromUTF8 (err.localizedDescription.UTF8String);
            return false;
        }

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

} // namespace fc::video
