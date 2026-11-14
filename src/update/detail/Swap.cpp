#include "Bundle.h"
#include "Files.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tubamp::update::detail
{
namespace
{
    constexpr std::string_view stagingPrefix = "update.";

    bool isRealDirectory (const fs::path& p)
    {
        struct stat st {};
        return ::lstat (p.c_str(), &st) == 0 && S_ISDIR (st.st_mode);
    }

    bool present (const fs::path& p)
    {
        struct stat st {};
        return ::lstat (p.c_str(), &st) == 0;
    }

    std::optional<fs::path> makeStagingDir (const fs::path& privateDir)
    {
        if (::mkdir (privateDir.c_str(), 0700) != 0 && errno != EEXIST)
            return std::nullopt;
        if (! isRealDirectory (privateDir))
        {
            errno = ENOTDIR;
            return std::nullopt;
        }
        auto name = (privateDir / (std::string (stagingPrefix) + "XXXXXX")).string();
        if (::mkdtemp (name.data()) == nullptr)
            return std::nullopt;
        return fs::path (name);
    }

    /** A staging directory whose owner is gone, with its lock now held by us (or with no
        lock file at all: the owner died between mkdtemp and open). */
    struct Ownerless
    {
        fs::path dir;
        Fd lock;
    };

    std::vector<Ownerless> collectOwnerless (const fs::path& privateDir)
    {
        std::vector<Ownerless> found;
        std::error_code ec;
        for (fs::directory_iterator it (privateDir, ec), end; ! ec && it != end; it.increment (ec))
        {
            const auto path = it->path();
            if (path.filename().string().rfind (stagingPrefix, 0) != 0)
                continue;
            if (! isRealDirectory (path))
            {
                ::unlink (path.c_str());
                continue;
            }
            Fd lock (::open ((path / "lock").c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC));
            if (! lock || ::flock (lock.get(), LOCK_EX | LOCK_NB) == 0)
                found.push_back ({ path, std::move (lock) });
        }
        return found;
    }

    void removeTree (const fs::path& p)
    {
        std::error_code ignored;
        fs::remove_all (p, ignored);
    }

    void removeTemporaries (const SupportPaths& support)
    {
        std::error_code ec;
        for (fs::directory_iterator it (support.dir, ec), end; ! ec && it != end; it.increment (ec))
            if (it->path().extension() == ".tmp")
                ::unlink (it->path().c_str());
    }

    /** previous/ and previous/tubamp.component may only be real directories. */
    void sanitizeSlot (const Layout& layout)
    {
        const auto slotDir = layout.previousBundle().parent_path();
        for (const auto& p : { slotDir, layout.previousBundle() })
            if (present (p) && ! isRealDirectory (p))
                ::unlink (p.c_str());
    }

    /** Moves `bundle` into a fresh ownerless directory, then deletes it: a crash between
        the two leaves garbage the next reconcile collects, never a half-deleted slot. */
    void discard (const fs::path& bundle, const fs::path& privateDir)
    {
        if (const auto trash = makeStagingDir (privateDir);
            trash && ::rename (bundle.c_str(), (*trash / bundleRoot).c_str()) == 0)
            removeTree (*trash);
    }

    void keepAsPrevious (const fs::path& bundle, const Layout& layout)
    {
        const auto slot = layout.previousBundle();
        ::mkdir (slot.parent_path().c_str(), 0700);
        if (present (slot))
            ::renamex_np (bundle.c_str(), slot.c_str(), RENAME_SWAP);
        else
            ::rename (bundle.c_str(), slot.c_str());
    }
}

Staging::Staging (fs::path dir, int lockFd) : path (std::move (dir)), lock (lockFd) {}
Staging::Staging (Staging&& other) noexcept : path (std::move (other.path)), lock (std::exchange (other.lock, -1)) {}
Staging::~Staging() { Fd closed (lock); }

Result<Staging, Failure> Staging::create (const SelfUpdatable& site)
{
    const auto& layout = site.layout();
    const auto supportLock = SupportLock::acquire (layout.support);
    if (! supportLock)
        return Failure::notWritable;
    const auto dir = makeStagingDir (layout.privateDir);
    if (! dir)
        return failureOf (errno);
    Fd lock (::open ((*dir / "lock").c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (! lock || ::flock (lock.get(), LOCK_EX | LOCK_NB) != 0)
        return failureOf (errno);
    return Staging (*dir, lock.release());
}

Result<std::variant<Committed, AlreadyInstalled>, Failure>
    commit (InspectedBundle&& bundle, const Staging& staging, const SelfUpdatable& site)
{
    using Outcome = std::variant<Committed, AlreadyInstalled>;
    const auto& layout = site.layout();
    const auto lock = SupportLock::acquire (layout.support);
    if (! lock)
        return Failure::notWritable;

    const auto live = layout.liveBundle();
    const auto installed = isRealDirectory (live) ? versionOf (live) : std::nullopt;
    if (! installed)
        return Failure::notWritable;
    if (*installed >= bundle.version())
        return Outcome (AlreadyInstalled { *installed });

    if (::renamex_np ((staging.dir() / bundleRoot).c_str(), live.c_str(), RENAME_SWAP) != 0)
        return failureOf (errno);
    return Outcome (Committed { *installed, bundle.version() });
}

DiskState reconcile (const SelfUpdatable& site)
{
    const auto& layout = site.layout();
    const auto lock = SupportLock::acquire (layout.support);
    const auto disk = readDisk (layout);
    if (! lock)
        return disk;

    removeTemporaries (layout.support);
    if (! isRealDirectory (layout.privateDir))
        return disk;

    sanitizeSlot (layout);
    const auto ownerless = collectOwnerless (layout.privateDir);

    if (disk.installed)
    {
        struct Candidate { fs::path bundle; Version version; bool inSlot; };
        std::optional<Candidate> keep;
        const auto consider = [&] (const fs::path& bundle, bool inSlot)
        {
            const auto version = present (bundle) ? sealedVersion (bundle) : std::nullopt;
            if (version && *version < *disk.installed && (! keep || *version > keep->version))
                keep = Candidate { bundle, *version, inSlot };
        };
        consider (layout.previousBundle(), true);
        for (const auto& o : ownerless)
            consider (o.dir / bundleRoot, false);

        if (keep && ! keep->inSlot)
            keepAsPrevious (keep->bundle, layout);
        else if (! keep && present (layout.previousBundle()))
            discard (layout.previousBundle(), layout.privateDir);
    }

    for (const auto& o : ownerless)
        removeTree (o.dir);
    return disk;
}
} // namespace tubamp::update::detail
