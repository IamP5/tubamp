#include "Bundle.h"
#include "Files.h"

#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <unistd.h>

#include <algorithm>

namespace tubamp::update::detail
{
namespace
{
    template <class T>
    struct CFOwned
    {
        T ref = nullptr;
        ~CFOwned() { if (ref != nullptr) CFRelease (ref); }
    };

    std::string textOf (CFTypeRef value)
    {
        if (value == nullptr || CFGetTypeID (value) != CFStringGetTypeID())
            return {};
        const auto string = static_cast<CFStringRef> (value);
        const auto capacity = CFStringGetMaximumSizeForEncoding (CFStringGetLength (string), kCFStringEncodingUTF8) + 1;
        std::string out (std::size_t (capacity), '\0');
        if (! CFStringGetCString (string, out.data(), capacity, kCFStringEncodingUTF8))
            return {};
        out.resize (std::char_traits<char>::length (out.c_str()));
        return out;
    }

    std::string textOf (CFDictionaryRef dict, CFStringRef key) { return textOf (CFDictionaryGetValue (dict, key)); }

    std::optional<std::int64_t> integerOf (CFTypeRef value)
    {
        std::int64_t n = 0;
        if (value == nullptr || CFGetTypeID (value) != CFNumberGetTypeID()
            || CFNumberIsFloatType (static_cast<CFNumberRef> (value))
            || ! CFNumberGetValue (static_cast<CFNumberRef> (value), kCFNumberSInt64Type, &n))
            return std::nullopt;
        return n;
    }

    std::uint32_t bigEndian32 (const std::uint8_t* p)
    {
        return (std::uint32_t (p[0]) << 24) | (std::uint32_t (p[1]) << 16) | (std::uint32_t (p[2]) << 8) | p[3];
    }

    std::uint32_t littleEndian32 (const std::uint8_t* p)
    {
        return (std::uint32_t (p[3]) << 24) | (std::uint32_t (p[2]) << 16) | (std::uint32_t (p[1]) << 8) | p[0];
    }

    fs::path executableOf (const fs::path& bundle) { return bundle / "Contents" / "MacOS" / pinnedExecutable; }
}

std::optional<BundleFacts> readBundleFacts (const fs::path& bundle)
{
    const auto bytes = readFile (bundle / "Contents" / "Info.plist", 1 << 20);
    if (! bytes)
        return std::nullopt;
    CFOwned<CFDataRef> data { CFDataCreate (kCFAllocatorDefault, bytes->data(), CFIndex (bytes->size())) };
    CFOwned<CFPropertyListRef> plist { CFPropertyListCreateWithData (kCFAllocatorDefault, data.ref,
                                                                     kCFPropertyListImmutable, nullptr, nullptr) };
    if (plist.ref == nullptr || CFGetTypeID (plist.ref) != CFDictionaryGetTypeID())
        return std::nullopt;

    const auto dict = static_cast<CFDictionaryRef> (plist.ref);
    BundleFacts facts;
    facts.identifier = textOf (dict, CFSTR ("CFBundleIdentifier"));
    facts.executable = textOf (dict, CFSTR ("CFBundleExecutable"));
    facts.shortVersion = textOf (dict, CFSTR ("CFBundleShortVersionString"));

    const auto components = CFDictionaryGetValue (dict, CFSTR ("AudioComponents"));
    if (components != nullptr && CFGetTypeID (components) == CFArrayGetTypeID())
    {
        const auto array = static_cast<CFArrayRef> (components);
        for (CFIndex i = 0; i < CFArrayGetCount (array); ++i)
        {
            AudioComponentEntry entry;
            const auto item = CFArrayGetValueAtIndex (array, i);
            if (item != nullptr && CFGetTypeID (item) == CFDictionaryGetTypeID())
            {
                const auto d = static_cast<CFDictionaryRef> (item);
                entry = { textOf (d, CFSTR ("type")), textOf (d, CFSTR ("subtype")), textOf (d, CFSTR ("manufacturer")),
                          integerOf (CFDictionaryGetValue (d, CFSTR ("version"))) };
            }
            facts.components.push_back (std::move (entry));
        }
    }
    return facts;
}

std::optional<Version> versionOf (const fs::path& bundle)
{
    const auto facts = readBundleFacts (bundle);
    return facts ? Version::parse (facts->shortVersion) : std::nullopt;
}

std::optional<Version> pinnedVersion (const BundleFacts& facts)
{
    const auto version = Version::parse (facts.shortVersion);
    if (! version || facts.identifier != pinnedBundleId
        || facts.executable != pinnedExecutable || facts.components.size() != 1)
        return std::nullopt;
    const auto& c = facts.components.front();
    if (c.type != pinnedType || c.subtype != pinnedSubtype || c.manufacturer != pinnedManufacturer
        || c.version != std::int64_t (version->audioComponentVersion()))
        return std::nullopt;
    return version;
}

std::vector<int> cpuTypesOf (const fs::path& machO)
{
    Fd fd (::open (machO.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    std::uint8_t head[4096] {};
    const auto got = fd ? ::pread (fd.get(), head, sizeof head, 0) : -1;
    if (got < 8)
        return {};
    const auto size = std::size_t (got);

    std::vector<int> types;
    const auto fatMagic = bigEndian32 (head);
    if (fatMagic == FAT_MAGIC || fatMagic == FAT_MAGIC_64)
    {
        const std::size_t stride = fatMagic == FAT_MAGIC ? sizeof (fat_arch) : sizeof (fat_arch_64);
        const auto count = std::min<std::size_t> (bigEndian32 (head + 4), (size - 8) / stride);
        for (std::size_t i = 0; i < count; ++i)
            types.push_back (int (bigEndian32 (head + 8 + i * stride)));
    }
    else if (const auto magic = littleEndian32 (head); magic == MH_MAGIC_64 || magic == MH_MAGIC)
        types.push_back (int (littleEndian32 (head + 4)));
    return types;
}

int runningCpuType() noexcept
{
#if defined(__arm64__)
    return CPU_TYPE_ARM64;
#elif defined(__x86_64__)
    return CPU_TYPE_X86_64;
#else
#error "tubamp ships arm64 and x86_64 only"
#endif
}

Result<InspectedBundle, Failure> inspect (UnpackedBundle&&, const Staging& staging, const Offer& offer,
                                          const SelfUpdatable& site)
{
    const auto bundle = staging.dir() / bundleRoot;
    const auto facts = readBundleFacts (bundle);
    const auto version = facts ? pinnedVersion (*facts) : std::nullopt;
    if (! version || *version != offer.version())
        return Failure::wrongBundle;

    auto needed = cpuTypesOf (executableOf (site.layout().liveBundle()));
    needed.push_back (runningCpuType());
    const auto present = cpuTypesOf (executableOf (bundle));
    for (const auto type : needed)
        if (std::find (present.begin(), present.end(), type) == present.end())
            return Failure::wrongBundle;

    if (! codeSealIntact (bundle))
        return Failure::wrongBundle;
    return InspectedBundle (*version);
}
} // namespace tubamp::update::detail
