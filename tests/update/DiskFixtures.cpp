#include "DiskFixtures.h"

#include "detail/Files.h"

#include <zlib.h>

#include <cerrno>
#include <crt_externs.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tubamp::update::test
{
using namespace detail;

namespace
{
    [[noreturn]] void abortTest (const std::string& why)
    {
        std::fprintf (stderr, "disk fixture: %s\n", why.c_str());
        std::abort();
    }

    void makeWritable (const fs::path& dir)
    {
        std::error_code ec;
        ::chmod (dir.c_str(), 0700);
        for (fs::recursive_directory_iterator it (dir, fs::directory_options::skip_permission_denied, ec), end;
             ! ec && it != end; it.increment (ec))
            if (it->is_directory (ec) && ! it->is_symlink (ec))
                ::chmod (it->path().c_str(), 0700);
    }

    std::string selfPath()
    {
        char buffer[4096];
        std::uint32_t size = sizeof buffer;
        if (_NSGetExecutablePath (buffer, &size) != 0)
            abortTest ("cannot find the tool's own path");
        return buffer;
    }

    int highDescriptor (int fd)
    {
        const int moved = ::fcntl (fd, F_DUPFD_CLOEXEC, 10);
        ::close (fd);
        return moved;
    }

    int decodeStatus (int status)
    {
        if (WIFEXITED (status))
            return WEXITSTATUS (status);
        if (WIFSIGNALED (status))
            return 128 + WTERMSIG (status);
        return -1;
    }

    pid_t spawn (const std::vector<std::string>& argv, bool quiet, int goFd, int readyFd)
    {
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        posix_spawn_file_actions_init (&actions);
        posix_spawnattr_init (&attributes);
        posix_spawnattr_setflags (&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
        posix_spawn_file_actions_addinherit_np (&actions, 0);
        if (quiet)
        {
            posix_spawn_file_actions_addopen (&actions, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen (&actions, 2, "/dev/null", O_WRONLY, 0);
        }
        else
        {
            posix_spawn_file_actions_addinherit_np (&actions, 1);
            posix_spawn_file_actions_addinherit_np (&actions, 2);
        }
        if (goFd >= 0)
            posix_spawn_file_actions_adddup2 (&actions, goFd, 3);
        if (readyFd >= 0)
            posix_spawn_file_actions_adddup2 (&actions, readyFd, 4);

        std::vector<char*> args;
        for (const auto& a : argv)
            args.push_back (const_cast<char*> (a.c_str()));
        args.push_back (nullptr);
        pid_t pid = -1;
        const auto r = ::posix_spawn (&pid, argv.front().c_str(), &actions, &attributes, args.data(), *_NSGetEnviron());
        posix_spawn_file_actions_destroy (&actions);
        posix_spawnattr_destroy (&attributes);
        if (r != 0)
            abortTest ("cannot spawn " + argv.front());
        return pid;
    }

    void copyTree (const fs::path& from, const fs::path& to)
    {
        std::error_code ec;
        fs::create_directories (to.parent_path(), ec);
        fs::copy (from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec)
            abortTest ("copy " + from.string() + ": " + ec.message());
    }

    void plistBuddy (const fs::path& plist, const std::vector<std::string>& commands)
    {
        std::vector<std::string> argv { "/usr/libexec/PlistBuddy" };
        for (const auto& c : commands)
        {
            argv.push_back ("-c");
            argv.push_back (c);
        }
        argv.push_back (plist.string());
        if (run (argv) != 0)
            abortTest ("PlistBuddy failed on " + plist.string());
    }

    constexpr std::string_view infoPlist = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>tubamp</string>
<key>CFBundleIdentifier</key><string>com.t0audio.tubamp</string>
<key>CFBundleName</key><string>tubamp</string>
<key>CFBundlePackageType</key><string>BNDL</string>
<key>CFBundleShortVersionString</key><string>0.0.0</string>
<key>CFBundleVersion</key><string>0.0.0</string>
<key>AudioComponents</key><array><dict>
<key>type</key><string>aufx</string><key>subtype</key><string>Tamp</string>
<key>manufacturer</key><string>Tuba</string><key>name</key><string>t0audio: tubamp</string>
<key>factoryFunction</key><string>tubampAUFactory</string><key>version</key><integer>0</integer>
</dict></array></dict></plist>
)";

    void flipByte (const fs::path& file, std::uint64_t offset)
    {
        Fd fd (::open (file.c_str(), O_RDWR));
        std::uint8_t b = 0;
        if (! fd || ::pread (fd.get(), &b, 1, off_t (offset)) != 1)
            abortTest ("cannot read " + file.string());
        b ^= 0xff;
        if (::pwrite (fd.get(), &b, 1, off_t (offset)) != 1)
            abortTest ("cannot write " + file.string());
    }

    std::uint64_t firstSliceOffset (const fs::path& machO)
    {
        const auto bytes = readAll (machO);
        auto be32 = [&] (std::size_t at)
        { return (std::uint32_t (bytes[at]) << 24) | (std::uint32_t (bytes[at + 1]) << 16) | (std::uint32_t (bytes[at + 2]) << 8) | bytes[at + 3]; };
        return be32 (0) == 0xcafebabe ? be32 (16) : 0;
    }

    void put16 (Bytes& b, std::uint32_t v) { b.push_back (std::uint8_t (v)); b.push_back (std::uint8_t (v >> 8)); }
    void put32 (Bytes& b, std::uint32_t v) { put16 (b, v & 0xffff); put16 (b, v >> 16); }
    void putText (Bytes& b, std::string_view s) { b.insert (b.end(), s.begin(), s.end()); }

    std::string rawDeflate (const std::string& data)
    {
        z_stream s {};
        deflateInit2 (&s, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        std::string out (deflateBound (&s, uLong (data.size())) + 16, '\0');
        s.next_in = reinterpret_cast<Bytef*> (const_cast<char*> (data.data()));
        s.avail_in = uInt (data.size());
        s.next_out = reinterpret_cast<Bytef*> (out.data());
        s.avail_out = uInt (out.size());
        deflate (&s, Z_FINISH);
        out.resize (s.total_out);
        deflateEnd (&s);
        return out;
    }
}

TempDir::TempDir()
{
    auto pattern = (fs::temp_directory_path() / "tubamp-update-test.XXXXXX").string();
    if (::mkdtemp (pattern.data()) == nullptr)
        abortTest ("mkdtemp failed");
    dir = fs::canonical (pattern);
}

TempDir::~TempDir()
{
    if (std::getenv ("TUBAMP_UPDATE_KEEP") != nullptr)
        return;
    makeWritable (dir);
    std::error_code ec;
    fs::remove_all (dir, ec);
}

int run (const std::vector<std::string>& argv, bool quiet)
{
    const auto pid = spawn (argv, quiet, -1, -1);
    int status = 0;
    while (::waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
    return decodeStatus (status);
}

Child::Child (const std::vector<std::string>& args)
{
    int goPipe[2], readyPipe[2];
    if (::pipe (goPipe) != 0 || ::pipe (readyPipe) != 0)
        abortTest ("pipe failed");
    const int goRead = highDescriptor (goPipe[0]), readyWrite = highDescriptor (readyPipe[1]);
    go = highDescriptor (goPipe[1]);
    ready = highDescriptor (readyPipe[0]);

    std::vector<std::string> argv { selfPath(), "child" };
    argv.insert (argv.end(), args.begin(), args.end());
    id = spawn (argv, std::getenv ("TUBAMP_UPDATE_CHILD_OUTPUT") == nullptr, goRead, readyWrite);
    ::close (goRead);
    ::close (readyWrite);
}

Child::~Child()
{
    release();
    if (ready >= 0)
        ::close (ready);
    if (! reaped)
    {
        kill();
        wait();
    }
}

bool Child::waitReady()
{
    char byte = 0;
    ssize_t n;
    while ((n = ::read (ready, &byte, 1)) < 0 && errno == EINTR) {}
    return n == 1;
}

void Child::release()
{
    if (go >= 0)
        ::close (go);
    go = -1;
}

void Child::kill()
{
    if (! reaped)
        ::kill (id, SIGKILL);
}

int Child::wait()
{
    if (! reaped)
    {
        while (::waitpid (id, &status, 0) < 0 && errno == EINTR) {}
        reaped = true;
    }
    return decodeStatus (status);
}

void childReady()
{
    const char byte = 1;
    (void) ::write (4, &byte, 1);
}

void childAwaitRelease()
{
    char byte;
    while (::read (3, &byte, 1) != 0 && errno == EINTR) {}
}

Home::Home (fs::path r)
    : root (std::move (r)),
      home (root / "home"),
      system (root / "System" / "Library" / "Audio" / "Plug-Ins" / "Components"),
      plugIns (home / "Library" / "Audio" / "Plug-Ins"),
      components (plugIns / "Components"),
      privateDir (plugIns / ".tubamp"),
      support { home / "Library" / "Application Support" / "tubamp" / "update" }
{
    fs::create_directories (components);
    fs::create_directories (system);
}

InstallSite Home::detect() const { return detect (image (live())); }

InstallSite Home::detect (const fs::path& runningImage) const
{
    return InstallSite::detect (runningImage, home, true, false, system);
}

SelfUpdatable Home::site() const
{
    auto s = detect();
    if (const auto* writable = std::get_if<SelfUpdatable> (&s.kind))
        return *writable;
    abortTest ("the temporary home is not self-updatable");
}

void Home::installLive (const fs::path& bundle) const
{
    std::error_code ec;
    fs::remove_all (live(), ec);
    copyTree (bundle, live());
}

void Home::installPrevious (const fs::path& bundle) const
{
    std::error_code ec;
    fs::remove_all (previous(), ec);
    copyTree (bundle, previous());
}

std::vector<fs::path> Home::stagingDirs() const
{
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (fs::directory_iterator it (privateDir, ec), end; ! ec && it != end; it.increment (ec))
        if (it->path().filename().string().rfind ("update.", 0) == 0)
            dirs.push_back (it->path());
    return dirs;
}

const char* nameOf (Variant v)
{
    switch (v)
    {
        case Variant::good: return "good";
        case Variant::armOnly: return "arm64 only";
        case Variant::otherBundleId: return "bundle id com.evil.tubamp";
        case Variant::otherType: return "type aumf";
        case Variant::otherSubtype: return "subtype Tamx";
        case Variant::otherManufacturer: return "manufacturer Tubb";
        case Variant::twoComponents: return "two AudioComponents entries";
        case Variant::otherExecutable: return "CFBundleExecutable other";
        case Variant::componentVersionMismatch: return "AudioComponents.version not the short version's integer";
        case Variant::resourceModified: return "sealed resource changed after signing";
        case Variant::executableModified: return "Mach-O byte changed after signing";
        case Variant::fileAdded: return "file added after signing";
        case Variant::unsignedBundle: return "unsigned";
    }
    return "?";
}

Fixtures::Fixtures()
{
    if (const char* component = std::getenv ("TUBAMP_UPDATE_COMPONENT"); component != nullptr && *component != '\0')
    {
        if (! fs::exists (fs::path (component) / "Contents" / "MacOS" / "tubamp"))
            abortTest (std::string ("TUBAMP_UPDATE_COMPONENT is not a built tubamp.component: ") + component);
        realComponent = fs::canonical (component);
    }
}

std::string Fixtures::description() const
{
    return realComponent ? "the real component " + realComponent->string() + ", copied and signed ad hoc"
                         : std::string ("the synthetic component (universal module " TUBAMP_UPDATE_FIXTURE_EXECUTABLE ")");
}

fs::path Fixtures::bundle (std::string_view version, Variant variant, bool synthetic)
{
    const bool useReal = realComponent && ! synthetic && variant != Variant::armOnly;
    const auto key = std::string (version) + "-" + std::to_string (int (variant)) + (useReal ? "-real" : "-synthetic");
    if (const auto it = built.find (key); it != built.end())
        return it->second;

    const auto bundlePath = cache.path() / key / "tubamp.component";
    const auto contents = bundlePath / "Contents";
    const auto executable = contents / "MacOS" / "tubamp";
    const auto plist = contents / "Info.plist";
    if (useReal)
        copyTree (*realComponent, bundlePath);
    else
    {
        fs::create_directories (contents / "MacOS");
        fs::copy_file (TUBAMP_UPDATE_FIXTURE_EXECUTABLE, executable);
        ::chmod (executable.c_str(), 0755);
        writeText (plist, infoPlist);
    }

    const auto v = *Version::parse (version);
    const auto integer = std::to_string (v.audioComponentVersion());
    plistBuddy (plist, { "Set :CFBundleShortVersionString " + std::string (version),
                         "Set :CFBundleVersion " + std::string (version),
                         "Set :AudioComponents:0:version " + integer });

    const std::map<Variant, std::string> plistEdits = {
        { Variant::otherBundleId, "Set :CFBundleIdentifier com.evil.tubamp" },
        { Variant::otherType, "Set :AudioComponents:0:type aumf" },
        { Variant::otherSubtype, "Set :AudioComponents:0:subtype Tamx" },
        { Variant::otherManufacturer, "Set :AudioComponents:0:manufacturer Tubb" },
        { Variant::twoComponents, "Copy :AudioComponents:0 :AudioComponents:1" },
        { Variant::otherExecutable, "Set :CFBundleExecutable other" },
        { Variant::componentVersionMismatch, "Set :AudioComponents:0:version 999" },
    };
    if (const auto edit = plistEdits.find (variant); edit != plistEdits.end())
        plistBuddy (plist, { edit->second });
    if (variant == Variant::armOnly
        && run ({ "/usr/bin/lipo", executable.string(), "-remove", "x86_64", "-output", executable.string() }) != 0)
        abortTest ("lipo failed");
    if (variant == Variant::resourceModified)
    {
        fs::create_directories (contents / "Resources");
        writeText (contents / "Resources" / "data.txt", "sealed");
    }
    if (variant == Variant::otherExecutable)
    {
        fs::copy_file (executable, contents / "MacOS" / "other");
        run ({ "/usr/bin/codesign", "--force", "--sign", "-", executable.string() });
    }

    if (variant == Variant::unsignedBundle)
    {
        run ({ "/usr/bin/codesign", "--remove-signature", executable.string() });
    }
    else if (run ({ "/usr/bin/codesign", "--force", "--sign", "-", bundlePath.string() }) != 0)
        abortTest ("codesign failed on " + bundlePath.string());

    if (variant == Variant::resourceModified)
        writeText (contents / "Resources" / "data.txt", "changed");
    if (variant == Variant::executableModified)
        flipByte (executable, firstSliceOffset (executable) + 0x21);
    if (variant == Variant::fileAdded)
    {
        fs::create_directories (contents / "Resources");
        writeText (contents / "Resources" / "extra.txt", "added");
    }

    built[key] = bundlePath;
    return bundlePath;
}

fs::path Fixtures::zip (std::string_view version, Variant variant, bool synthetic)
{
    const auto source = bundle (version, variant, synthetic);
    const auto archive = source.parent_path() / "release.zip";
    if (! fs::exists (archive)
        && run ({ "/usr/bin/ditto", "-c", "-k", "--keepParent", source.string(), archive.string() }) != 0)
        abortTest ("ditto failed on " + source.string());
    return archive;
}

Fixtures& fixtures()
{
    static Fixtures f;
    return f;
}

Bytes readAll (const fs::path& file)
{
    std::ifstream in (file, std::ios::binary);
    return Bytes (std::istreambuf_iterator<char> (in), {});
}

void writeText (const fs::path& file, std::string_view text)
{
    std::ofstream out (file, std::ios::binary | std::ios::trunc);
    out.write (text.data(), std::streamsize (text.size()));
}

std::optional<Offer> offerFor (ByteView archive, std::string_view version, std::string_view running)
{
    static const TestSigner signer;
    const TrustRoot trust ({ signer.publicKey() });
    const auto digest = sha256 (archive);
    const auto payload = std::string ("{\"product\":\"com.t0audio.tubamp\",\"version\":\"") + std::string (version)
                       + "\",\"minimumMacOS\":\"11.0\",\"archive\":{\"sha256\":\"" + hex (digest)
                       + "\",\"size\":" + std::to_string (archive.size()) + "}}";
    auto manifest = openEnvelope (makeEnvelope (payload, { &signer }), trust);
    if (! manifest)
        return std::nullopt;
    const auto evaluation = evaluate (manifest.value(), OfferContext { *Version::parse (running), std::nullopt, { 15, 0 } });
    if (const auto* offer = std::get_if<Offer> (&evaluation))
        return *offer;
    return std::nullopt;
}

Bytes forgeZip (const std::vector<ForgedEntry>& entries, std::string_view trailer)
{
    Bytes out, central;
    for (const auto& e : entries)
    {
        const auto stored = e.method == 8 ? rawDeflate (e.data) : e.data;
        const auto crc = e.crc.value_or (std::uint32_t (::crc32 (0, reinterpret_cast<const Bytef*> (e.data.data()), uInt (e.data.size()))));
        const auto size = e.declaredSize.value_or (std::uint32_t (e.data.size()));
        const auto offset = std::uint32_t (out.size());
        const auto& localName = e.localName.value_or (e.name);

        put32 (out, 0x04034b50); put16 (out, 20); put16 (out, e.flags); put16 (out, e.method);
        put32 (out, 0); put32 (out, crc); put32 (out, std::uint32_t (stored.size())); put32 (out, size);
        put16 (out, std::uint32_t (localName.size())); put16 (out, 0);
        putText (out, localName);
        putText (out, stored);

        put32 (central, 0x02014b50); put16 (central, (3u << 8) | 20); put16 (central, 20);
        put16 (central, e.flags); put16 (central, e.method); put32 (central, 0); put32 (central, crc);
        put32 (central, std::uint32_t (stored.size())); put32 (central, size);
        put16 (central, std::uint32_t (e.name.size())); put16 (central, 0); put16 (central, 0);
        put16 (central, 0); put16 (central, 0); put32 (central, e.mode << 16); put32 (central, offset);
        putText (central, e.name);
    }
    const auto centralOffset = std::uint32_t (out.size());
    out.insert (out.end(), central.begin(), central.end());
    put32 (out, 0x06054b50); put16 (out, 0); put16 (out, 0);
    put16 (out, std::uint32_t (entries.size())); put16 (out, std::uint32_t (entries.size()));
    put32 (out, std::uint32_t (central.size())); put32 (out, centralOffset); put16 (out, 0);
    putText (out, trailer);
    return out;
}

std::vector<ForgedEntry> forgedBundle()
{
    return {
        { "tubamp.component/", 040755 },
        { "tubamp.component/Contents/", 040755 },
        { "tubamp.component/Contents/Info.plist", 0100644, std::string (infoPlist) },
        { "tubamp.component/Contents/MacOS/", 040755 },
        { "tubamp.component/Contents/MacOS/tubamp", 0100755, std::string (4000, 'x'), 8 },
    };
}

std::string listing (const fs::path& dir)
{
    std::vector<std::string> lines;
    std::error_code ec;
    for (fs::recursive_directory_iterator it (dir, ec), end; ! ec && it != end; it.increment (ec))
    {
        struct stat st {};
        ::lstat (it->path().c_str(), &st);
        std::ostringstream line;
        line << it->path().string() << " ino=" << st.st_ino << " size=" << (S_ISDIR (st.st_mode) ? 0 : st.st_size)
             << " mode=" << std::oct << st.st_mode;
        lines.push_back (line.str());
    }
    std::sort (lines.begin(), lines.end());
    std::string text;
    for (const auto& l : lines)
        text += l + "\n";
    return text;
}

int fileMode (const fs::path& p)
{
    struct stat st {};
    return ::lstat (p.c_str(), &st) == 0 ? int (st.st_mode & 07777) : -1;
}

std::uint64_t inodeOf (const fs::path& p)
{
    struct stat st {};
    return ::lstat (p.c_str(), &st) == 0 ? st.st_ino : 0;
}

bool codesignVerifies (const fs::path& bundle)
{
    return run ({ "/usr/bin/codesign", "--verify", "--deep", "--strict", bundle.string() }) == 0;
}
} // namespace tubamp::update::test
