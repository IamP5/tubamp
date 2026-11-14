#pragma once

// PRIVATE to src/update. The disk boundary for the bundle: where this copy lives, the
// staging protocol, the swap, cleanup.
//
// Layout (all under the user's home, all owned by the user):
//
//   Library/Audio/Plug-Ins/Components/tubamp.component    the installed bundle
//   Library/Audio/Plug-Ins/.tubamp/                        outside every scanned folder
//     previous/tubamp.component                            kept for a manual restore
//     update.XXXXXX/                                       one attempt (mkdtemp, 0700)
//       lock                                               flock while the attempt lives
//       tubamp.component/                                  new bundle, old one after swap
//
// .tubamp must be on the live bundle's volume, or the copy is NotSelfUpdatable
// (noAtomicSwap): the swap is one renamex_np(RENAME_SWAP) and never crosses volumes.
//
// Lock order: a Staging's own lock, then the support lock. reconcile() holds the support
// lock and only ever tries staging locks with LOCK_NB, so the two never wait on each other.

#include "Prefs.h"
#include "Trust.h"

namespace tubamp::update::detail
{
namespace fs = std::filesystem;

/** What is installed, read from the disk. The only record of it: restart pending is
    derived by comparing `installed` with the compiled-in version. */
struct DiskState
{
    std::optional<Version> installed;   // CFBundleShortVersionString of the live bundle
    bool systemCopy = false;            // a tubamp.component in /Library that is not this copy

    bool operator== (const DiskState&) const = default;
};

inline const fs::path defaultSystemComponents = "/Library/Audio/Plug-Ins/Components";

struct Layout
{
    fs::path componentsDir;         // <home>/Library/Audio/Plug-Ins/Components
    fs::path privateDir;            // <home>/Library/Audio/Plug-Ins/.tubamp
    SupportPaths support;
    fs::path systemComponentsDir;   // only read: is there a system copy?

    fs::path liveBundle() const { return componentsDir / bundleRoot; }
    fs::path previousBundle() const { return privateDir / "previous" / bundleRoot; }
};

/** A copy that may replace itself. Invariant: the running image is exactly the live
    bundle, which is a real directory; componentsDir and privateDir are writable by this
    user, share one volume, and the volume supports RENAME_SWAP. Only detect makes one. */
class SelfUpdatable
{
public:
    const Layout& layout() const noexcept { return paths; }

private:
    friend struct InstallSite;
    explicit SelfUpdatable (Layout l) : paths (std::move (l)) {}
    Layout paths;
};

struct InstallSite
{
    struct Off {};   // TUBAMP_UPDATES=OFF
    /** Checks and announces, never installs. */
    struct NotSelfUpdatable
    {
        ManualReason why;
        SupportPaths support;
        fs::path runningBundle;
        fs::path systemComponentsDir;
    };

    std::variant<Off, NotSelfUpdatable, SelfUpdatable> kind;

    /** `runningImage` is the Mach-O this code was loaded from (dladdr), not the host's
        executable. Every path is a parameter so tests run in a temporary home. Reads
        only: stat, lstat, access, getattrlist. The checks, in order:
          notAnAudioUnit   the Standalone app, or the image is not <x>.component/Contents/MacOS/<y>
          systemDomain     the bundle's folder is `systemComponents`
          unexpectedPath   the bundle is not <home>/Library/Audio/Plug-Ins/Components/tubamp.component,
                           or that path is a symlink
          readOnly         Components, the live bundle, .tubamp or the support folder (or the
                           nearest existing parent of the last two) is not writable, or
                           .tubamp exists and is not a real directory
          noAtomicSwap     .tubamp (or its parent) is on another device than Components and
                           the live bundle, or that volume lacks VOL_CAP_INT_RENAME_SWAP */
    static InstallSite detect (const fs::path& runningImage, const fs::path& home,
                               bool updatesCompiledIn, bool isStandalone,
                               const fs::path& systemComponents = defaultSystemComponents);
};

/** The machine's view of the site: whether it can check, and whether it can install. */
namespace site
{
    struct Off { bool operator== (const Off&) const = default; };
    struct Manual { ManualReason why; bool operator== (const Manual&) const = default; };
    struct Writable { bool operator== (const Writable&) const = default; };
}
using Site = std::variant<site::Off, site::Manual, site::Writable>;

inline Site siteOf (const InstallSite& s)
{
    if (std::holds_alternative<InstallSite::Off> (s.kind))
        return site::Off {};
    if (const auto* manual = std::get_if<InstallSite::NotSelfUpdatable> (&s.kind))
        return site::Manual { manual->why };
    return site::Writable {};
}

// The attempt: a chain of move-only values with private constructors. The function
// named as friend is each one's only producer.

/** One attempt's directory under privateDir, plus an exclusive flock on <dir>/lock. The
    lock says whether the directory is in use: the kernel drops it when the owner dies.
    mkdtemp and the lock happen under the support lock, so reconcile() never sees a live
    attempt's directory before its lock exists. The destructor closes the lock and
    deletes nothing; reconcile() collects. */
class Staging
{
public:
    static Result<Staging, Failure> create (const SelfUpdatable&);   // noSpace | notWritable
    Staging (Staging&&) noexcept;
    Staging (const Staging&) = delete;
    ~Staging();

    const fs::path& dir() const noexcept { return path; }

private:
    Staging (fs::path, int lockFd);
    fs::path path;
    int lock = -1;
};

/** The downloaded zip, proven to be the signed archive: `fd` is an unlinked file whose
    length and SHA-256 (hashed as written) equal the Offer's. Only ArchiveSink::finish
    makes one. */
class VerifiedArchive
{
public:
    VerifiedArchive (VerifiedArchive&&) noexcept;
    VerifiedArchive (const VerifiedArchive&) = delete;
    ~VerifiedArchive();
    int descriptor() const noexcept { return fd; }

private:
    friend class ArchiveSink;
    explicit VerifiedArchive (int descriptor);
    int fd = -1;
};

/** Where the fetcher's bytes go: open(O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW, 0600) in the
    staging directory, unlinked at once. write() hashes each chunk and refuses the byte
    past the signed size. The first refusal is remembered: after the fetcher stops,
    finish() reports it (integrity for an overrun, noSpace or notWritable for the disk). */
class ArchiveSink
{
public:
    static Result<ArchiveSink, Failure> open (const Staging&, const Offer&);
    ArchiveSink (ArchiveSink&&) noexcept;
    ArchiveSink (const ArchiveSink&) = delete;
    ~ArchiveSink();

    bool write (ByteView) noexcept;                       // false: disk error or overrun
    std::uint64_t received() const noexcept { return count; }
    Result<VerifiedArchive, Failure> finish() &&;         // integrity on any mismatch

private:
    ArchiveSink (int descriptor, ArchiveSpec expected);
    int fd = -1;
    std::uint64_t count = 0;
    ArchiveSpec expected;
    Sha256Stream hash;
    std::optional<Failure> fault;
};

class UnpackedBundle;
/** Reads the zip's central directory through the fd (pread only: the bytes unpacked are
    the bytes hashed), runs planExtraction over every entry first, then creates exactly
    the planned entries with openat(O_EXCL|O_NOFOLLOW) under the staging directory,
    making missing parent directories 0755. Each file is inflated to its declared size
    and no further, and must match its CRC-32. Accepts what `ditto -c -k` writes: stored
    or deflated entries, no encryption, no zip64, one disk. archive on any violation,
    noSpace on ENOSPC or EDQUOT, notWritable on other write errors. */
Result<UnpackedBundle, Failure> unpack (VerifiedArchive&&, const Staging&);

class UnpackedBundle
{
public:
    UnpackedBundle (UnpackedBundle&&) noexcept = default;
    UnpackedBundle (const UnpackedBundle&) = delete;

private:
    friend Result<UnpackedBundle, Failure> unpack (VerifiedArchive&&, const Staging&);
    UnpackedBundle() = default;
};

class InspectedBundle;
/** Info.plist pins (bundle id, CFBundleExecutable "tubamp", exactly one AudioComponents
    entry aufx/Tamp/Tuba), the offered version in both version keys, a Mach-O slice for
    every CPU type the live bundle has and for the running one, and an intact code seal
    (codeSealIntact). Nothing is executed or loaded. wrongBundle on any mismatch. */
Result<InspectedBundle, Failure> inspect (UnpackedBundle&&, const Staging&, const Offer&,
                                          const SelfUpdatable&);

class InspectedBundle
{
public:
    InspectedBundle (InspectedBundle&&) noexcept = default;
    InspectedBundle (const InspectedBundle&) = delete;
    Version version() const noexcept { return v; }

private:
    friend Result<InspectedBundle, Failure> inspect (UnpackedBundle&&, const Staging&, const Offer&,
                                                     const SelfUpdatable&);
    explicit InspectedBundle (Version version) : v (version) {}
    Version v;
};

struct Committed { Version from, to; };
struct AlreadyInstalled { Version installed; };   // another process got there first

/** THE commit point, under the support lock: re-read the installed version; if it is
    not below the bundle's, AlreadyInstalled; else one renamex_np(RENAME_SWAP) between
    the staged bundle and the live one. notWritable when the swap is refused, and when
    the live bundle is gone or has no readable version: an uninstalled copy is not
    brought back by an update. */
Result<std::variant<Committed, AlreadyInstalled>, Failure>
    commit (InspectedBundle&&, const Staging&, const SelfUpdatable&);

/** Converge the disk. Idempotent, safe in any process at any time, the only code that
    deletes. Under the support lock it:
      - deletes the support folder's *.tmp files (a writer that died mid-write);
      - finds the ownerless staging directories (flock LOCK_NB wins, or no lock file);
      - keeps in previous/ the highest pinned, sealed bundle below the installed version
        among previous/ and those directories (a tie keeps previous/), moving it there
        with one rename or RENAME_SWAP, and discards previous/ when there is none;
      - deletes every ownerless staging directory.
    When the installed version is unreadable, previous/ is left alone. The live bundle
    is never touched. A crash inside leaves a disk this function accepts as input. */
DiskState reconcile (const SelfUpdatable&);

/** For NotSelfUpdatable copies: the running bundle's version on disk, and whether a
    system copy other than the running one exists. Writes nothing. */
DiskState readDisk (const InstallSite::NotSelfUpdatable&);
} // namespace tubamp::update::detail
