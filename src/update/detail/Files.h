#pragma once

// PRIVATE to src/update. POSIX plumbing shared by the disk files: an owned descriptor,
// the support lock, capped whole-file reads, replace-by-rename writes, and what a
// bundle on disk says about itself (Info.plist, Mach-O CPU types). Reads use O_NOFOLLOW
// on the last component; nothing here loads or executes code.

#include "Prefs.h"
#include "Rules.h"

#include <filesystem>
#include <utility>

namespace tubamp::update::detail
{
namespace fs = std::filesystem;

class Fd
{
public:
    Fd() = default;
    explicit Fd (int descriptor) noexcept : fd (descriptor) {}
    Fd (Fd&& other) noexcept : fd (std::exchange (other.fd, -1)) {}
    Fd& operator= (Fd&& other) noexcept { reset (std::exchange (other.fd, -1)); return *this; }
    Fd (const Fd&) = delete;
    ~Fd() { reset(); }

    int get() const noexcept { return fd; }
    int release() noexcept { return std::exchange (fd, -1); }
    explicit operator bool() const noexcept { return fd >= 0; }
    void reset (int descriptor = -1) noexcept;

private:
    int fd = -1;
};

/** How long acquire() keeps trying, and how often. Holders keep the lock for
    milliseconds; a process stopped while it holds the lock (a debugger, a crash reporter)
    keeps it indefinitely, and the waiter must not. */
inline constexpr std::chrono::milliseconds supportLockPatience { 2000 }, supportLockPoll { 5 };

/** flock(LOCK_EX) on <support>/lock, creating the folder and the file if needed. Held for
    milliseconds. The kernel drops it when the process dies, so it is never stale.
    acquire() polls with LOCK_NB and gives up, with nullopt, after supportLockPatience or
    within supportLockPoll of `stop` being requested. Every caller treats nullopt as "do
    nothing this time": reconcile() and the next editor open try again. */
class SupportLock
{
public:
    static std::optional<SupportLock> acquire (const SupportPaths&, const CancelToken& stop = neverCancelled());

private:
    explicit SupportLock (Fd f) : fd (std::move (f)) {}
    Fd fd;
};

/** The whole regular file, or nullopt if it is missing, a symlink, or above `cap`. */
std::optional<Bytes> readFile (const fs::path&, std::size_t cap);

bool writeAll (int fd, ByteView) noexcept;

/** Write <path>.tmp, fsync, rename over `path`. The caller holds the SupportLock, so one
    fixed temporary name is enough, and reconcile() can delete a leftover one. */
bool replaceFile (const fs::path&, ByteView);

/** errno of a failed write or rename while staging. */
Failure failureOf (int err) noexcept;

inline constexpr std::string_view pinnedType = "aufx", pinnedSubtype = "Tamp", pinnedManufacturer = "Tuba";
inline constexpr std::string_view pinnedExecutable = "tubamp";

struct AudioComponentEntry
{
    std::string type, subtype, manufacturer;
    std::optional<std::int64_t> version;
};

struct BundleFacts
{
    std::string identifier, executable, shortVersion;
    std::vector<AudioComponentEntry> components;
};

/** <bundle>/Contents/Info.plist, at most 1 MiB, parsed by CoreFoundation. */
std::optional<BundleFacts> readBundleFacts (const fs::path& bundle);

/** CFBundleShortVersionString of the bundle, the only record of what is installed. */
std::optional<Version> versionOf (const fs::path& bundle);

/** The version the plist pins: bundle id, executable, exactly one aufx/Tamp/Tuba entry,
    and CFBundleShortVersionString and the AudioComponents integer naming one Version. */
std::optional<Version> pinnedVersion (const BundleFacts&);

/** CPU types in a thin or fat Mach-O; empty if it is neither. */
std::vector<int> cpuTypesOf (const fs::path& machO);

/** The CPU type of the slice running this code. */
int runningCpuType() noexcept;

struct Layout;
struct DiskState;
/** The live bundle's version and the system copy, for reconcile(). Site.cpp. */
DiskState readDisk (const Layout&);
} // namespace tubamp::update::detail
