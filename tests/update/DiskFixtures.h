#pragma once

// What the disk tests stand on: a temporary home with every root injected, child
// processes, tubamp.component fixtures (synthetic by default, the real built component
// when TUBAMP_UPDATE_COMPONENT names one), a zip forger for hostile archives, and offers
// made through the signed path. Nothing here touches the real ~/Library or /Library.

#include "TestSigner.h"
#include "detail/Bundle.h"

#include <map>
#include <string>
#include <vector>

namespace tubamp::update::test
{
namespace fs = std::filesystem;

/** A fresh directory under $TMPDIR, removed (permissions restored first) when it dies. */
class TempDir
{
public:
    TempDir();
    ~TempDir();
    TempDir (const TempDir&) = delete;
    const fs::path& path() const noexcept { return dir; }

private:
    fs::path dir;
};

/** Runs a program and waits. Returns the exit code, or 128 + signal. */
int run (const std::vector<std::string>& argv, bool quiet = true);

/** A child process with two pipes: it writes one byte on fd 4 when ready and blocks
    reading fd 3 until the parent calls release(). */
class Child
{
public:
    explicit Child (const std::vector<std::string>& args);   // runs `tubamp_update_tool child <args>`
    ~Child();
    bool waitReady();
    void release();
    void kill();
    /** Exit code, or 128 + signal. */
    int wait();
    pid_t pid() const noexcept { return id; }

private:
    pid_t id = -1;
    int go = -1, ready = -1;
    bool reaped = false;
    int status = 0;
};

/** The child's side of Child's two pipes. */
void childReady();
void childAwaitRelease();

/** <root>/home is $HOME; <root>/System/... stands in for /Library/Audio/Plug-Ins/Components. */
struct Home
{
    explicit Home (fs::path root);

    fs::path root, home, system, plugIns, components, privateDir;
    detail::SupportPaths support;

    fs::path live() const { return components / "tubamp.component"; }
    fs::path previous() const { return privateDir / "previous" / "tubamp.component"; }
    fs::path image (const fs::path& bundle) const { return bundle / "Contents" / "MacOS" / "tubamp"; }

    detail::InstallSite detect() const;
    detail::InstallSite detect (const fs::path& runningImage) const;
    /** detect() must give SelfUpdatable; the test aborts otherwise. */
    detail::SelfUpdatable site() const;

    void installLive (const fs::path& bundle) const;
    void installPrevious (const fs::path& bundle) const;
    std::vector<fs::path> stagingDirs() const;
};

enum class Variant
{
    good,
    armOnly,                    // x86_64 slice removed before signing (synthetic only)
    otherBundleId,
    otherType,
    otherSubtype,
    otherManufacturer,
    twoComponents,
    otherExecutable,
    componentVersionMismatch,   // AudioComponents.version is not the short version's integer
    resourceModified,           // a sealed resource changed after signing
    executableModified,         // one byte of the Mach-O changed after signing
    fileAdded,                  // a file added inside the bundle after signing
    unsignedBundle,
};
const char* nameOf (Variant);

/** Bundles and zips, built once per run and cached. */
class Fixtures
{
public:
    Fixtures();
    bool real() const noexcept { return realComponent.has_value(); }
    std::string description() const;

    /** A signed tubamp.component at `version`. `synthetic` forces the synthetic source. */
    fs::path bundle (std::string_view version, Variant = Variant::good, bool synthetic = false);
    /** `ditto -c -k --keepParent` of bundle(): the release archive format. */
    fs::path zip (std::string_view version, Variant = Variant::good, bool synthetic = false);

private:
    TempDir cache;
    std::optional<fs::path> realComponent;
    std::map<std::string, fs::path> built;
};

Fixtures& fixtures();

Bytes readAll (const fs::path&);
void writeText (const fs::path&, std::string_view);

/** An Offer for these archive bytes, made the only way one can be: signed, opened, evaluated. */
std::optional<detail::Offer> offerFor (ByteView archive, std::string_view version, std::string_view running);

struct ForgedEntry
{
    ForgedEntry (std::string entryName, std::uint32_t entryMode = 0100644, std::string contents = {}, std::uint16_t how = 0)
        : name (std::move (entryName)), mode (entryMode), data (std::move (contents)), method (how) {}

    std::string name;
    std::uint32_t mode;                    // st_mode in the external attributes
    std::string data;
    std::uint16_t method;                  // 0 stored, 8 deflated, anything else as given
    std::uint16_t flags = 0;
    std::optional<std::uint32_t> declaredSize, crc;
    std::optional<std::string> localName;  // a local header that names another file
};

/** A zip written entry by entry, so any name, mode, size or method can be expressed. */
Bytes forgeZip (const std::vector<ForgedEntry>&, std::string_view trailer = {});

/** A minimal well-formed bundle as forged entries. */
std::vector<ForgedEntry> forgedBundle();

/** Every path under `dir` with inode, size and mode, one per line, sorted. */
std::string listing (const fs::path& dir);

int fileMode (const fs::path&);
std::uint64_t inodeOf (const fs::path&);
bool codesignVerifies (const fs::path& bundle);
} // namespace tubamp::update::test
