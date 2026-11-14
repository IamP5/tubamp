#include "Bundle.h"
#include "Files.h"

#include <sys/attr.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tubamp::update::detail
{
namespace
{
    std::optional<struct stat> statOf (const fs::path& p)
    {
        struct stat st {};
        if (::stat (p.c_str(), &st) != 0)
            return std::nullopt;
        return st;
    }

    std::optional<struct stat> lstatOf (const fs::path& p)
    {
        struct stat st {};
        if (::lstat (p.c_str(), &st) != 0)
            return std::nullopt;
        return st;
    }

    bool sameFile (const fs::path& a, const fs::path& b)
    {
        const auto x = statOf (a), y = statOf (b);
        return x && y && x->st_dev == y->st_dev && x->st_ino == y->st_ino;
    }

    bool writableDir (const fs::path& p)
    {
        const auto st = statOf (p);
        return st && S_ISDIR (st->st_mode) && ::access (p.c_str(), W_OK | X_OK) == 0;
    }

    /** The folder itself if it exists, else the nearest existing parent we would create it in. */
    fs::path nearestExisting (fs::path p)
    {
        while (! lstatOf (p) && p.has_relative_path())
            p = p.parent_path();
        return p;
    }

    bool shapedLikeComponent (const fs::path& image)
    {
        const auto macos = image.parent_path(), contents = macos.parent_path(), bundle = contents.parent_path();
        return macos.filename() == "MacOS" && contents.filename() == "Contents" && bundle.extension() == ".component";
    }

    bool volumeSupportsSwap (const fs::path& onVolume)
    {
        attrlist request {};
        request.bitmapcount = ATTR_BIT_MAP_COUNT;
        request.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
        struct __attribute__ ((packed)) { std::uint32_t length; vol_capabilities_attr_t caps; } reply {};
        if (::getattrlist (onVolume.c_str(), &request, &reply, sizeof reply, 0) != 0)
            return false;
        const auto bit = VOL_CAP_INT_RENAME_SWAP;
        return (reply.caps.valid[VOL_CAPABILITIES_INTERFACES] & bit) != 0
            && (reply.caps.capabilities[VOL_CAPABILITIES_INTERFACES] & bit) != 0;
    }

    bool systemCopyBeside (const fs::path& systemComponents, const fs::path& ownBundle)
    {
        const auto system = systemComponents / bundleRoot;
        return lstatOf (system).has_value() && ! sameFile (system, ownBundle);
    }
}

InstallSite InstallSite::detect (const fs::path& runningImage, const fs::path& home, bool updatesCompiledIn,
                                 bool isStandalone, const fs::path& systemComponents)
{
    if (! updatesCompiledIn)
        return { Off {} };

    const SupportPaths support { home / "Library" / "Application Support" / "tubamp" / "update" };
    const auto runningBundle = runningImage.parent_path().parent_path().parent_path();
    const auto manual = [&] (ManualReason why)
    {
        return InstallSite { NotSelfUpdatable { why, support, runningBundle, systemComponents } };
    };

    if (isStandalone || ! shapedLikeComponent (runningImage))
        return manual (ManualReason::notAnAudioUnit);
    if (sameFile (runningBundle.parent_path(), systemComponents))
        return manual (ManualReason::systemDomain);

    const auto plugIns = home / "Library" / "Audio" / "Plug-Ins";
    const Layout layout { plugIns / "Components", plugIns / ".tubamp", support, systemComponents };
    const auto live = layout.liveBundle();
    const auto liveLink = lstatOf (live);
    if (! liveLink || ! S_ISDIR (liveLink->st_mode) || ! sameFile (runningBundle, live))
        return manual (ManualReason::unexpectedPath);

    const auto privateLink = lstatOf (layout.privateDir);
    if (! writableDir (layout.componentsDir) || ! writableDir (live)
        || (privateLink && ! S_ISDIR (privateLink->st_mode))
        || ! writableDir (nearestExisting (layout.privateDir)) || ! writableDir (nearestExisting (support.dir)))
        return manual (ManualReason::readOnly);

    const auto components = statOf (layout.componentsDir), liveStat = statOf (live);
    const auto staging = statOf (nearestExisting (layout.privateDir));
    if (! components || ! liveStat || ! staging || staging->st_dev != components->st_dev
        || liveStat->st_dev != components->st_dev || ! volumeSupportsSwap (layout.componentsDir))
        return manual (ManualReason::noAtomicSwap);

    return { SelfUpdatable (layout) };
}

DiskState readDisk (const InstallSite::NotSelfUpdatable& site)
{
    return { versionOf (site.runningBundle), systemCopyBeside (site.systemComponentsDir, site.runningBundle) };
}

DiskState readDisk (const Layout& layout)
{
    return { versionOf (layout.liveBundle()), systemCopyBeside (layout.systemComponentsDir, layout.liveBundle()) };
}
} // namespace tubamp::update::detail
