// Headless Audio Unit host for black-box measurement of reference plugins.
//
//   auhost list
//   auhost params <type> <subtype> <manu> [--state in.aupreset] [--set ...] [--setn ...]
//   auhost render <type> <subtype> <manu> <in.wav> <out.wav>
//          [--set <id>=<value>]... [--setn "<name>=<value>"]... [--state in.aupreset]
//          [--block 512] [--channels 2] [--tail <seconds>] [--warmup <seconds>]
//   auhost state  <type> <subtype> <manu> <out.aupreset> [--set ...] [--state in.aupreset]
//   auhost edit   <type> <subtype> <manu> <out.aupreset> [--state in.aupreset] [--title T]
//
// Parameter values are in the plugin's own units, as `params` prints them. `render`
// runs at the input file's sample rate, offline, and writes 32-bit float WAV. `edit`
// opens the plugin's own editor in a window and keeps writing its full state to
// <out.aupreset> (every 2 s and on close), which `render --state` then replays: that
// is the way in for plugins whose effect selection is not an AU parameter.
// Every command but `edit` is killed after AUHOST_TIMEOUT seconds (default 300).
//
// Build: tools/re/build.sh
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
[[noreturn]] void die (const char* what, OSStatus status = 0)
{
    if (status != 0)
        std::fprintf (stderr, "auhost: %s (OSStatus %d)\n", what, (int) status);
    else
        std::fprintf (stderr, "auhost: %s\n", what);
    _exit (1);
}

void check (OSStatus status, const char* what)
{
    if (status != noErr)
        die (what, status);
}

OSType fourcc (const char* text)
{
    char padded[4] = { ' ', ' ', ' ', ' ' };
    std::memcpy (padded, text, std::min<size_t> (4, std::strlen (text)));
    return ((OSType) (unsigned char) padded[0] << 24) | ((OSType) (unsigned char) padded[1] << 16)
         | ((OSType) (unsigned char) padded[2] << 8) | (OSType) (unsigned char) padded[3];
}

std::string fourccText (OSType code)
{
    char text[5] = { (char) (code >> 24), (char) (code >> 16), (char) (code >> 8), (char) code, 0 };
    return text;
}

std::string utf8 (CFStringRef string)
{
    if (string == nullptr)
        return {};
    char buffer[1024];
    return CFStringGetCString (string, buffer, sizeof (buffer), kCFStringEncodingUTF8) ? buffer : "";
}

using Channels = std::vector<std::vector<float>>;

AudioStreamBasicDescription floatFormat (double sampleRate, UInt32 channels, bool interleaved)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                        | (interleaved ? 0 : kAudioFormatFlagIsNonInterleaved);
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = interleaved ? 4 * channels : 4;
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}

struct BufferList
{
    explicit BufferList (UInt32 channels, UInt32 frames) : storage (channels, std::vector<float> (frames))
    {
        list = (AudioBufferList*) std::calloc (1, offsetof (AudioBufferList, mBuffers) + channels * sizeof (AudioBuffer));
        list->mNumberBuffers = channels;
        point (frames);
    }

    void point (UInt32 frames)
    {
        for (UInt32 ch = 0; ch < list->mNumberBuffers; ++ch)
        {
            list->mBuffers[ch].mNumberChannels = 1;
            list->mBuffers[ch].mDataByteSize = frames * 4;
            list->mBuffers[ch].mData = storage[ch].data();
        }
    }

    Channels storage;
    AudioBufferList* list = nullptr;
};

Channels readAudio (const char* path, double& sampleRate)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path, (CFIndex) std::strlen (path), false);
    ExtAudioFileRef file = nullptr;
    check (ExtAudioFileOpenURL (url, &file), "cannot open input file");
    CFRelease (url);

    AudioStreamBasicDescription fileFormat {};
    UInt32 size = sizeof (fileFormat);
    check (ExtAudioFileGetProperty (file, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat), "cannot read input format");
    sampleRate = fileFormat.mSampleRate;

    const UInt32 channels = fileFormat.mChannelsPerFrame;
    auto client = floatFormat (sampleRate, channels, false);
    check (ExtAudioFileSetProperty (file, kExtAudioFileProperty_ClientDataFormat, sizeof (client), &client), "cannot set client format");

    Channels out (channels);
    const UInt32 chunk = 8192;
    BufferList buffers (channels, chunk);

    for (;;)
    {
        buffers.point (chunk);
        UInt32 frames = chunk;
        check (ExtAudioFileRead (file, &frames, buffers.list), "read failed");
        if (frames == 0)
            break;
        for (UInt32 ch = 0; ch < channels; ++ch)
            out[ch].insert (out[ch].end(), buffers.storage[ch].begin(), buffers.storage[ch].begin() + frames);
    }

    ExtAudioFileDispose (file);
    return out;
}

void writeAudio (const char* path, const Channels& audio, double sampleRate)
{
    const UInt32 channels = (UInt32) audio.size();
    const UInt32 frames = (UInt32) audio[0].size();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path, (CFIndex) std::strlen (path), false);
    auto fileFormat = floatFormat (sampleRate, channels, true);
    ExtAudioFileRef file = nullptr;
    check (ExtAudioFileCreateWithURL (url, kAudioFileWAVEType, &fileFormat, nullptr, kAudioFileFlags_EraseFile, &file), "cannot create output file");
    CFRelease (url);

    auto client = floatFormat (sampleRate, channels, false);
    check (ExtAudioFileSetProperty (file, kExtAudioFileProperty_ClientDataFormat, sizeof (client), &client), "cannot set client format");

    BufferList buffers (channels, frames);
    buffers.storage = audio;
    buffers.point (frames);
    check (ExtAudioFileWrite (file, frames, buffers.list), "write failed");
    ExtAudioFileDispose (file);
}

struct Options
{
    std::vector<std::pair<AudioUnitParameterID, float>> setById;
    std::vector<std::pair<std::string, float>> setByName;
    std::string stateIn, title = "auhost";
    UInt32 block = 512, channels = 2;
    double tail = 0.0, warmup = 0.0;
};

Options parseOptions (int argc, char** argv, int first)
{
    Options options;
    for (int i = first; i < argc; ++i)
    {
        const std::string flag = argv[i];
        auto value = [&]() -> std::string
        {
            if (i + 1 >= argc)
                die (("missing value after " + flag).c_str());
            return argv[++i];
        };
        auto split = [&] (const std::string& pair) -> std::pair<std::string, float>
        {
            const auto eq = pair.rfind ('=');
            if (eq == std::string::npos)
                die (("expected key=value, got " + pair).c_str());
            return { pair.substr (0, eq), std::strtof (pair.c_str() + eq + 1, nullptr) };
        };

        if (flag == "--set")          { auto kv = split (value()); options.setById.push_back ({ (AudioUnitParameterID) std::strtoul (kv.first.c_str(), nullptr, 0), kv.second }); }
        else if (flag == "--setn")    options.setByName.push_back (split (value()));
        else if (flag == "--state")   options.stateIn = value();
        else if (flag == "--title")   options.title = value();
        else if (flag == "--block")   options.block = (UInt32) std::atoi (value().c_str());
        else if (flag == "--channels") options.channels = (UInt32) std::atoi (value().c_str());
        else if (flag == "--tail")    options.tail = std::atof (value().c_str());
        else if (flag == "--warmup")  options.warmup = std::atof (value().c_str());
        else die (("unknown option " + flag).c_str());
    }
    return options;
}

AudioUnit openUnit (const char* type, const char* subtype, const char* manu)
{
    AudioComponentDescription description { fourcc (type), fourcc (subtype), fourcc (manu), 0, 0 };
    AudioComponent component = AudioComponentFindNext (nullptr, &description);
    if (component == nullptr)
        die ("no such audio unit");
    AudioUnit unit = nullptr;
    check (AudioComponentInstanceNew (component, &unit), "cannot instantiate audio unit");
    return unit;
}

std::vector<AudioUnitParameterID> parameterIds (AudioUnit unit)
{
    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo (unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, &writable) != noErr)
        return {};
    std::vector<AudioUnitParameterID> ids (size / sizeof (AudioUnitParameterID));
    if (! ids.empty())
        AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
    return ids;
}

bool parameterInfo (AudioUnit unit, AudioUnitParameterID id, AudioUnitParameterInfo& info)
{
    UInt32 size = sizeof (info);
    return AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size) == noErr;
}

std::string parameterName (const AudioUnitParameterInfo& info)
{
    if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) != 0 && info.cfNameString != nullptr)
        return utf8 (info.cfNameString);
    return info.name;
}

void loadState (AudioUnit unit, const std::string& path)
{
    NSData* data = [NSData dataWithContentsOfFile: [NSString stringWithUTF8String: path.c_str()]];
    if (data == nil)
        die ("cannot read state file");
    CFPropertyListRef plist = CFPropertyListCreateWithData (nullptr, (__bridge CFDataRef) data, kCFPropertyListImmutable, nullptr, nullptr);
    if (plist == nullptr)
        die ("state file is not a property list");
    check (AudioUnitSetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof (plist)), "plugin rejected the state");
    CFRelease (plist);
}

bool saveState (AudioUnit unit, const std::string& path)
{
    CFPropertyListRef plist = nullptr;
    UInt32 size = sizeof (plist);
    if (AudioUnitGetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) != noErr || plist == nullptr)
        return false;
    CFDataRef data = CFPropertyListCreateData (nullptr, plist, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
    CFRelease (plist);
    if (data == nullptr)
        return false;
    const bool ok = [(__bridge NSData*) data writeToFile: [NSString stringWithUTF8String: path.c_str()] atomically: YES];
    CFRelease (data);
    return ok;
}

void applyParameters (AudioUnit unit, const Options& options)
{
    for (const auto& [id, value] : options.setById)
        check (AudioUnitSetParameter (unit, id, kAudioUnitScope_Global, 0, value, 0), "cannot set parameter by id");

    if (options.setByName.empty())
        return;

    const auto ids = parameterIds (unit);
    for (const auto& [wanted, value] : options.setByName)
    {
        bool found = false;
        for (auto id : ids)
        {
            AudioUnitParameterInfo info {};
            if (parameterInfo (unit, id, info) && strcasecmp (parameterName (info).c_str(), wanted.c_str()) == 0)
            {
                check (AudioUnitSetParameter (unit, id, kAudioUnitScope_Global, 0, value, 0), "cannot set parameter by name");
                found = true;
                break;
            }
        }
        if (! found)
            die (("no parameter named " + wanted).c_str());
    }
}

struct Source
{
    const Channels* audio = nullptr;
    int64_t position = 0;
    Channels scratch;
};

OSStatus pullInput (void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
{
    auto* source = (Source*) context;
    for (UInt32 ch = 0; ch < io->mNumberBuffers; ++ch)
    {
        if (io->mBuffers[ch].mData == nullptr)
        {
            auto& scratch = source->scratch[std::min<size_t> (ch, source->scratch.size() - 1)];
            scratch.resize (frames);
            io->mBuffers[ch].mData = scratch.data();
            io->mBuffers[ch].mDataByteSize = frames * 4;
        }
        auto* destination = (float*) io->mBuffers[ch].mData;
        const auto& channel = (*source->audio)[std::min<size_t> (ch, source->audio->size() - 1)];
        for (UInt32 i = 0; i < frames; ++i)
        {
            const int64_t index = source->position + i;
            destination[i] = index >= 0 && index < (int64_t) channel.size() ? channel[(size_t) index] : 0.0f;
        }
    }
    return noErr;
}

void configure (AudioUnit unit, double sampleRate, const Options& options, Source& source)
{
    auto format = floatFormat (sampleRate, options.channels, false);
    check (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof (format)), "input format rejected");
    check (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof (format)), "output format rejected");
    UInt32 maxFrames = options.block;
    check (AudioUnitSetProperty (unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof (maxFrames)), "block size rejected");

    source.scratch.assign (options.channels, std::vector<float> (options.block));
    AURenderCallbackStruct callback { pullInput, &source };
    check (AudioUnitSetProperty (unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof (callback)), "cannot install input callback");
    check (AudioUnitInitialize (unit), "initialize failed");
}

void pumpRunLoop()
{
    while (CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {}
}

void renderBlock (AudioUnit unit, BufferList& out, UInt32 frames, Float64 sampleTime)
{
    out.point (frames);
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp stamp {};
    stamp.mSampleTime = sampleTime;
    stamp.mFlags = kAudioTimeStampSampleTimeValid;
    check (AudioUnitRender (unit, &flags, &stamp, 0, frames, out.list), "render failed");
}

/** Initialises the unit, applies state and parameters, and runs silence through it.
    Several vendors (Native Instruments among them) queue parameter changes and only
    apply them inside render, so a value read back straight after a set is stale. */
void applyAndSettle (AudioUnit unit, const Options& options)
{
    static Channels silence (2, std::vector<float> (1));
    static Source source;
    source.audio = &silence;
    source.position = 1;
    configure (unit, 48000.0, options, source);
    if (! options.stateIn.empty())
        loadState (unit, options.stateIn);
    applyParameters (unit, options);

    BufferList out (options.channels, options.block);
    Float64 sampleTime = 0;
    for (int block = 0; block < 24; ++block)
    {
        renderBlock (unit, out, options.block, sampleTime);
        sampleTime += options.block;
        pumpRunLoop();
    }
}

bool changesAnything (const Options& options)
{
    return ! options.stateIn.empty() || ! options.setById.empty() || ! options.setByName.empty();
}

int listUnits()
{
    for (OSType type : { kAudioUnitType_Effect, kAudioUnitType_MusicEffect })
    {
        AudioComponentDescription wanted { type, 0, 0, 0, 0 };
        AudioComponent component = nullptr;
        while ((component = AudioComponentFindNext (component, &wanted)) != nullptr)
        {
            AudioComponentDescription found {};
            AudioComponentGetDescription (component, &found);
            CFStringRef name = nullptr;
            AudioComponentCopyName (component, &name);
            std::printf ("%s\t%s\t%s\t%s\n", fourccText (found.componentType).c_str(), fourccText (found.componentSubType).c_str(),
                         fourccText (found.componentManufacturer).c_str(), utf8 (name).c_str());
            if (name != nullptr)
                CFRelease (name);
        }
    }
    return 0;
}

int printParameters (AudioUnit unit)
{
    std::printf ("id\tname\tmin\tmax\tdefault\tcurrent\tunit\tdisplay\tchoices\n");
    for (auto id : parameterIds (unit))
    {
        AudioUnitParameterInfo info {};
        if (! parameterInfo (unit, id, info))
            continue;
        AudioUnitParameterValue current = 0;
        AudioUnitGetParameter (unit, id, kAudioUnitScope_Global, 0, &current);

        std::string display;
        AudioUnitParameterStringFromValue fromValue { id, &current, nullptr };
        UInt32 size = sizeof (fromValue);
        if (AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0, &fromValue, &size) == noErr && fromValue.outString != nullptr)
        {
            display = utf8 (fromValue.outString);
            CFRelease (fromValue.outString);
        }

        std::string choices;
        CFArrayRef names = nullptr;
        size = sizeof (names);
        if (AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &names, &size) == noErr && names != nullptr)
        {
            for (CFIndex i = 0; i < CFArrayGetCount (names); ++i)
                choices += (i > 0 ? "|" : "") + utf8 ((CFStringRef) CFArrayGetValueAtIndex (names, i));
            CFRelease (names);
        }

        std::printf ("%u\t%s\t%g\t%g\t%g\t%g\t%u\t%s\t%s\n", (unsigned) id, parameterName (info).c_str(), info.minValue, info.maxValue,
                     info.defaultValue, current, (unsigned) info.unit, display.c_str(), choices.c_str());
    }

    Float64 latency = 0, tail = 0;
    UInt32 size = sizeof (latency);
    AudioUnitGetProperty (unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &size);
    size = sizeof (tail);
    AudioUnitGetProperty (unit, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &tail, &size);
    std::fprintf (stderr, "latency_s\t%g\ntail_s\t%g\n", latency, tail);
    return 0;
}

int render (AudioUnit unit, const char* inPath, const char* outPath, const Options& options)
{
    double sampleRate = 0;
    Channels input = readAudio (inPath, sampleRate);
    if (input.empty() || input[0].empty())
        die ("input file is empty");

    Source source;
    source.audio = &input;
    configure (unit, sampleRate, options, source);
    if (! options.stateIn.empty())
        loadState (unit, options.stateIn);
    applyParameters (unit, options);
    pumpRunLoop();

    const int64_t warmup = (int64_t) (options.warmup * sampleRate);
    const int64_t total = (int64_t) input[0].size() + (int64_t) (options.tail * sampleRate);
    Channels output (options.channels);
    for (auto& channel : output)
        channel.reserve ((size_t) total);

    BufferList out (options.channels, options.block);
    Float64 sampleTime = 0;

    // Warm-up feeds silence so smoothed parameters and loaders settle before the take.
    for (int64_t position = -warmup; position < total; position += options.block)
    {
        const UInt32 frames = (UInt32) std::min<int64_t> (options.block, total - position);
        source.position = position;
        renderBlock (unit, out, frames, sampleTime);
        sampleTime += frames;
        for (UInt32 i = 0; i < frames; ++i)
            if (position + i >= 0)
                for (UInt32 ch = 0; ch < options.channels; ++ch)
                    output[ch].push_back (out.storage[ch][i]);
        if (((position / options.block) & 63) == 0)
            pumpRunLoop();
    }

    writeAudio (outPath, output, sampleRate);

    Float64 latency = 0;
    UInt32 size = sizeof (latency);
    AudioUnitGetProperty (unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &size);
    std::fprintf (stderr, "rendered %lld frames at %g Hz, reported latency %g samples\n", (long long) total, sampleRate, latency * sampleRate);
    return 0;
}
} // namespace

@interface EditSession : NSObject <NSWindowDelegate>
@property (nonatomic) AudioUnit unit;
@property (nonatomic) std::string statePath;
@end

@implementation EditSession
{
    std::unique_ptr<BufferList> buffers;
    Float64 sampleTime;
}

- (void) tick: (NSTimer*) timer
{
    if (! buffers)
        buffers = std::make_unique<BufferList> (2, 512);
    renderBlock (self.unit, *buffers, 512, sampleTime);
    sampleTime += 512;
}

- (void) save: (NSTimer*) timer
{
    saveState (self.unit, self.statePath);
}

- (void) windowWillClose: (NSNotification*) notification
{
    const bool ok = saveState (self.unit, self.statePath);
    std::fprintf (stderr, "auhost: state %s to %s\n", ok ? "saved" : "NOT saved", self.statePath.c_str());
    _exit (ok ? 0 : 1);
}
@end

static int edit (AudioUnit unit, const char* statePath, const Options& options)
{
    static Channels silence (2, std::vector<float> (1));
    static Source source;
    source.audio = &silence;
    source.position = 1;
    Options stereo = options;
    stereo.channels = 2;
    configure (unit, 48000.0, stereo, source);
    if (! options.stateIn.empty())
        loadState (unit, options.stateIn);
    applyParameters (unit, options);

    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo (unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) != noErr || size == 0)
        die ("plugin has no Cocoa editor");
    auto* info = (AudioUnitCocoaViewInfo*) std::malloc (size);
    check (AudioUnitGetProperty (unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &size), "cannot read editor info");

    NSBundle* bundle = [NSBundle bundleWithURL: (__bridge NSURL*) info->mCocoaAUViewBundleLocation];
    Class factoryClass = [bundle classNamed: (__bridge NSString*) info->mCocoaAUViewClass[0]];
    if (factoryClass == Nil)
        die ("cannot load the editor class");
    id<AUCocoaUIBase> factory = [[factoryClass alloc] init];

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy: NSApplicationActivationPolicyRegular];
    NSView* view = [factory uiViewForAudioUnit: unit withSize: NSMakeSize (900, 600)];
    if (view == nil)
        die ("plugin returned no editor view");

    NSWindow* window = [[NSWindow alloc] initWithContentRect: NSMakeRect (80, 80, view.frame.size.width, view.frame.size.height)
                                                   styleMask: NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing: NSBackingStoreBuffered
                                                       defer: NO];
    window.title = [NSString stringWithUTF8String: options.title.c_str()];
    window.contentView = view;
    window.releasedWhenClosed = NO;

    static EditSession* session;
    session = [[EditSession alloc] init];
    session.unit = unit;
    session.statePath = statePath;
    window.delegate = session;
    [NSTimer scheduledTimerWithTimeInterval: 0.01 target: session selector: @selector (tick:) userInfo: nil repeats: YES];
    [NSTimer scheduledTimerWithTimeInterval: 2.0 target: session selector: @selector (save:) userInfo: nil repeats: YES];

    [window makeKeyAndOrderFront: nil];
    [NSApp activateIgnoringOtherApps: YES];
    [NSApp run];
    return 0;
}

int main (int argc, char** argv)
{
    @autoreleasepool
    {
        if (argc >= 2 && std::strcmp (argv[1], "list") == 0)
            return listUnits();

        if (argc < 5)
            die ("usage: auhost list | params|render|state|edit <type> <subtype> <manu> ... (see the header of auhost.mm)");

        const std::string command = argv[1];

        // Some plugins hang or raise a modal alert when hosted headless. Nothing here
        // legitimately runs for minutes, so every command but `edit` dies by SIGALRM
        // after AUHOST_TIMEOUT seconds (default 300, 0 disables).
        if (command != "edit")
        {
            const char* limit = std::getenv ("AUHOST_TIMEOUT");
            alarm ((unsigned) (limit != nullptr ? std::atoi (limit) : 300));
        }

        AudioUnit unit = openUnit (argv[2], argv[3], argv[4]);
        int status = 1;

        if (command == "params")
        {
            const Options options = parseOptions (argc, argv, 5);
            if (changesAnything (options))
                applyAndSettle (unit, options);
            status = printParameters (unit);
        }
        else if (command == "render" && argc >= 7)
        {
            status = render (unit, argv[5], argv[6], parseOptions (argc, argv, 7));
        }
        else if (command == "state" && argc >= 6)
        {
            const Options options = parseOptions (argc, argv, 6);
            if (changesAnything (options))
                applyAndSettle (unit, options);
            status = saveState (unit, argv[5]) ? 0 : 1;
        }
        else if (command == "edit" && argc >= 6)
        {
            status = edit (unit, argv[5], parseOptions (argc, argv, 6));
        }
        else
        {
            die ("unknown command or missing arguments");
        }

        std::fflush (stdout);
        std::fflush (stderr);
        // Plugin teardown is where third-party AUs most often crash; the results are
        // already on disk, so leave without running it.
        _exit (status);
    }
}
