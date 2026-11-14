// The disk half of the updater, run by `tubamp_update_tool test`: prefs and the envelope
// cache, InstallSite::detect, the install chain (ArchiveSink, unpack, inspect, commit),
// reconcile() and the crash table. Every root is a temporary directory. Process death is
// real: a child (`tubamp_update_tool child ...`) _exits at a fault point, is killed by
// SIGXFSZ mid-write, or gets SIGKILL, and the parent then inspects and reconciles.

#include "DiskFixtures.h"
#include "UpdateTests.h"

#include "detail/EnvelopeCache.h"
#include "detail/Files.h"

#include <atomic>
#include <mach/machine.h>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace tubamp::update::test
{
using namespace detail;

namespace
{
int* passedCount = nullptr;
int* failedCount = nullptr;

void check (bool ok, const std::string& what)
{
    if (ok)
        ++*passedCount;
    else
    {
        ++*failedCount;
        std::printf ("  FAIL  %s\n", what.c_str());
    }
}

void group (const char* name) { std::printf ("%s\n", name); }

Version v (std::string_view s) { return *Version::parse (s); }

ByteView view (const Bytes& b) { return ByteView (b.data(), b.size()); }

using CommitOutcome = std::variant<Committed, AlreadyInstalled>;

Result<Staging, Failure> stage (const SelfUpdatable& site) { return Staging::create (site); }

Result<VerifiedArchive, Failure> download (const Staging& staging, ByteView zip, const Offer& offer)
{
    auto sink = ArchiveSink::open (staging, offer);
    if (! sink)
        return sink.error();
    for (std::size_t at = 0; at < zip.size(); at += 7919)
        if (! sink.value().write (zip.subspan (at, std::min<std::size_t> (7919, zip.size() - at))))
            break;
    return std::move (sink.value()).finish();
}

Result<UnpackedBundle, Failure> downloadAndUnpack (const Staging& staging, ByteView zip, const Offer& offer)
{
    auto archive = download (staging, zip, offer);
    if (! archive)
        return archive.error();
    return unpack (std::move (archive).value(), staging);
}

Result<InspectedBundle, Failure> stageThrough (const SelfUpdatable&, const Staging&, ByteView, const Offer&);

/** The install job as phase 2b's worker will run it, ending in reconcile(). */
Result<CommitOutcome, Failure> installJob (const SelfUpdatable& site, const fs::path& zipPath,
                                           std::string_view version, std::string_view running)
{
    const auto zip = readAll (zipPath);
    const auto offer = offerFor (view (zip), version, running);
    if (! offer)
        return Failure::integrity;
    auto outcome = [&]() -> Result<CommitOutcome, Failure>
    {
        auto staging = stage (site);
        if (! staging)
            return staging.error();
        auto inspected = stageThrough (site, staging.value(), view (zip), *offer);
        if (! inspected)
            return inspected.error();
        return commit (std::move (inspected).value(), staging.value(), site);
    }();
    reconcile (site);
    return outcome;
}

bool committed (const Result<CommitOutcome, Failure>& r) { return r && std::holds_alternative<Committed> (r.value()); }

template <class T>
bool failedWith (const Result<T, Failure>& r, Failure expected) { return ! r && r.error() == expected; }

std::string onlyLock (const fs::path& staging)
{
    std::string names;
    std::error_code ec;
    for (fs::directory_iterator it (staging, ec), end; ! ec && it != end; it.increment (ec))
        names += it->path().filename().string() + " ";
    return names;
}

/** A temporary home with R installed and, optionally, P kept as previous. */
struct World
{
    TempDir dir;
    Home home { dir.path() };

    explicit World (std::optional<std::string_view> live = "0.3.0", std::optional<std::string_view> previous = std::nullopt)
    {
        if (live)
            home.installLive (fixtures().bundle (*live));
        if (previous)
            home.installPrevious (fixtures().bundle (*previous));
    }
};

// ---------------------------------------------------------------------------------

void prefsStore()
{
    group ("disk.prefs");
    using namespace std::chrono;
    TempDir dir;
    const SupportPaths support { dir.path() / "support" };
    const sys_seconds now { seconds (1'800'000'000) };

    check (loadPrefs (support) == Prefs {}, "a missing prefs.json loads the defaults");

    const auto written = mutatePrefs (support, PrefsPatch { false, true, v ("0.4.0") });
    const auto loaded = loadPrefs (support);
    check (loaded == written && ! loaded.autoCheck && loaded.disclosed && loaded.announced == v ("0.4.0"),
           "mutatePrefs round-trips through prefs.json");
    check (! fs::exists (support.dir / "prefs.json.tmp"), "a completed write leaves no temporary file");
    check (mutatePrefs (support, PrefsPatch { {}, {}, v ("0.3.0") }).announced == v ("0.4.0"),
           "announced never goes down on disk");

    writeText (support.dir / "prefs.json", "{\"autoCheck\":\"no\",\"disclosed\":true,\"announced\":\"0.4\"}");
    const auto mixed = loadPrefs (support);
    check (mixed.autoCheck && mixed.disclosed && ! mixed.announced, "a field of the wrong type falls back to its default alone");
    writeText (support.dir / "prefs.json", "not json");
    check (loadPrefs (support) == Prefs {}, "garbage loads the defaults");

    TempDir fresh;
    const SupportPaths daily { fresh.path() / "support" };
    check (claimDailyCheck (daily, now), "first claim wins");
    check (! claimDailyCheck (daily, now + hours (23)), "a claim 23 h later loses");
    check (claimDailyCheck (daily, now + hours (24)), "a claim 24 h later wins");
    check (claimDailyCheck (daily, now - hours (5)), "a claim with the clock moved backwards wins");
    mutatePrefs (daily, PrefsPatch { false, {}, {} });
    check (! claimDailyCheck (daily, now + hours (100)), "no claim while automatic checks are off");

    TempDir race;
    const auto raceDir = race.path() / "support";
    std::string winnersPerRound;
    bool everyRoundOneWinner = true;
    for (int round = 0; round < 20; ++round)
    {
        const auto at = now + hours (24 * round);
        std::vector<std::unique_ptr<Child>> children;
        for (int i = 0; i < 8; ++i)
            children.push_back (std::make_unique<Child> (std::vector<std::string> { "claim", raceDir.string(), std::to_string (at.time_since_epoch().count()) }));
        bool allReady = true;
        for (auto& c : children)
            allReady = c->waitReady() && allReady;
        for (auto& c : children)
            c->release();
        int winners = 0;
        for (auto& c : children)
            winners += c->wait() == 0 ? 1 : 0;
        everyRoundOneWinner = everyRoundOneWinner && allReady && winners == 1;
        winnersPerRound += std::to_string (winners) + " ";
    }
    check (everyRoundOneWinner, "8 processes racing claimDailyCheck, 20 rounds a day apart: one winner each (" + winnersPerRound + ")");

    TempDir locked;
    const SupportPaths readOnly { locked.path() / "support" };
    fs::create_directories (readOnly.dir);
    ::chmod (readOnly.dir.c_str(), 0500);
    check (! claimDailyCheck (readOnly, now), "an unwritable support folder never claims, so never checks on every open");
    check (! mutatePrefs (readOnly, PrefsPatch { false, {}, {} }).autoCheck, "an unwritable folder still returns the patched value");
    ::chmod (readOnly.dir.c_str(), 0700);
}

void envelopeCache()
{
    group ("disk.cache");
    TempDir dir;
    const SupportPaths support { dir.path() / "support" };
    const TestSigner signer, stranger;
    const TrustRoot trust ({ signer.publicKey() });
    auto envelope = [&] (std::string_view version, const TestSigner& by)
    {
        const auto payload = std::string ("{\"product\":\"com.t0audio.tubamp\",\"version\":\"") + std::string (version)
                           + "\",\"minimumMacOS\":\"11.0\",\"archive\":{\"sha256\":\"" + std::string (64, 'a') + "\",\"size\":10}}";
        return makeEnvelope (payload, { &by });
    };
    auto cached = [&] { const auto m = loadCachedEnvelope (support, trust); return m ? std::optional (m->version()) : std::nullopt; };

    check (! cached(), "nothing cached at first");
    const auto e040 = envelope ("0.4.0", signer);
    storeEnvelope (support, view (e040), trust);
    check (cached() == v ("0.4.0") && readAll (support.dir / "latest.signed") == e040, "a verified envelope is stored byte for byte");
    storeEnvelope (support, view (envelope ("0.3.0", signer)), trust);
    check (cached() == v ("0.4.0"), "a lower version never replaces the cache");
    storeEnvelope (support, view (envelope ("0.9.0", stranger)), trust);
    check (cached() == v ("0.4.0"), "an envelope signed by an untrusted key is never written");
    const Bytes garbage = toBytes ("tubamp-update-v1\ngarbage");
    storeEnvelope (support, view (garbage), trust);
    check (cached() == v ("0.4.0"), "garbage is never written");
    storeEnvelope (support, view (envelope ("0.5.0", signer)), trust);
    check (cached() == v ("0.5.0"), "a higher version replaces the cache");
    check (! fs::exists (support.dir / "latest.signed.tmp"), "no temporary file remains");
    check (! loadCachedEnvelope (support, TrustRoot ({ stranger.publicKey() })), "the cache is re-verified on load: another trust root sees nothing");
    writeText (support.dir / "latest.signed", "tubamp-update-v1\nsig 00");
    check (! cached(), "a damaged cache is nothing cached");
}

void site()
{
    group ("disk.site");
    using R = ManualReason;
    auto reason = [] (const InstallSite& s) -> std::optional<R>
    {
        if (const auto* m = std::get_if<InstallSite::NotSelfUpdatable> (&s.kind))
            return m->why;
        return std::nullopt;
    };

    World w;
    const auto& h = w.home;
    check (std::holds_alternative<InstallSite::Off> (InstallSite::detect (h.image (h.live()), h.home, false, false, h.system).kind),
           "updates compiled out: Off");
    check (reason (InstallSite::detect (h.image (h.live()), h.home, true, true, h.system)) == R::notAnAudioUnit, "Standalone: notAnAudioUnit");
    check (reason (h.detect (h.home / "Applications" / "tubamp.app" / "Contents" / "MacOS" / "tubamp")) == R::notAnAudioUnit,
           "an image outside a .component: notAnAudioUnit");

    const auto good = h.detect();
    const auto* writable = std::get_if<SelfUpdatable> (&good.kind);
    check (writable != nullptr, "the per-user bundle at the expected path: SelfUpdatable");
    if (writable != nullptr)
        check (writable->layout().liveBundle() == h.live() && writable->layout().privateDir == h.privateDir
                   && writable->layout().support.dir == h.support.dir,
               "the layout is derived from the injected home");
    check (std::holds_alternative<site::Writable> (siteOf (good)), "siteOf: Writable");

    fs::copy (h.live(), h.system / "tubamp.component", fs::copy_options::recursive);
    const auto system = h.detect (h.image (h.system / "tubamp.component"));
    check (reason (system) == R::systemDomain, "running from the system Components: systemDomain");
    if (const auto* m = std::get_if<InstallSite::NotSelfUpdatable> (&system.kind))
        check (readDisk (*m) == DiskState { v ("0.3.0"), false }, "readDisk for the system copy: its version, no other system copy");

    fs::copy (h.live(), h.components / "tubamp 2.component", fs::copy_options::recursive);
    const auto renamed = h.detect (h.image (h.components / "tubamp 2.component"));
    check (reason (renamed) == R::unexpectedPath, "a renamed bundle: unexpectedPath");
    if (const auto* m = std::get_if<InstallSite::NotSelfUpdatable> (&renamed.kind))
        check (readDisk (*m) == DiskState { v ("0.3.0"), true }, "readDisk sees the other copy in the system folder");
    check (reason (h.detect (h.image (h.home / "Desktop" / "tubamp.component"))) == R::unexpectedPath,
           "a bundle elsewhere in the home: unexpectedPath");
    fs::remove_all (h.system / "tubamp.component");

    {
        World linked (std::nullopt);
        const auto elsewhere = linked.home.root / "elsewhere" / "tubamp.component";
        fs::create_directories (elsewhere.parent_path());
        fs::copy (fixtures().bundle ("0.3.0"), elsewhere, fs::copy_options::recursive);
        fs::create_directory_symlink (elsewhere, linked.home.live());
        check (reason (linked.home.detect()) == R::unexpectedPath, "the live path is a symlink: unexpectedPath");
    }

    auto readOnlyCase = [&] (const fs::path& dir, const std::string& what)
    {
        ::chmod (dir.c_str(), 0555);
        check (reason (h.detect()) == R::readOnly, what + " read-only: readOnly");
        ::chmod (dir.c_str(), 0755);
    };
    readOnlyCase (h.components, "Components");
    readOnlyCase (h.live(), "the live bundle");
    readOnlyCase (h.plugIns, "Plug-Ins, where .tubamp would be made,");
    fs::create_directories (h.support.dir.parent_path());
    readOnlyCase (h.support.dir.parent_path(), "the support folder's parent");
    writeText (h.privateDir, "not a folder");
    check (reason (h.detect()) == R::readOnly, ".tubamp is a file: readOnly");
    fs::remove (h.privateDir);
    check (std::holds_alternative<SelfUpdatable> (h.detect().kind), "SelfUpdatable again once the folders are restored");

    auto mountFat = [] (const fs::path& image, const fs::path& at)
    {
        fs::create_directories (at);
        return run ({ "/usr/bin/hdiutil", "create", "-quiet", "-size", "8m", "-fs", "MS-DOS", "-volname", "TUBAMPTEST",
                      "-layout", "NONE", image.string() }) == 0
            && run ({ "/usr/bin/hdiutil", "attach", "-quiet", "-nobrowse", "-noautoopen", "-mountpoint", at.string(),
                      image.string() }) == 0;
    };
    auto unmount = [] (const fs::path& at) { run ({ "/usr/bin/hdiutil", "detach", "-quiet", "-force", at.string() }); };

    {
        World other;
        if (mountFat (other.home.root / "staging.dmg", other.home.privateDir))
        {
            check (reason (other.home.detect()) == R::noAtomicSwap, ".tubamp on another device: noAtomicSwap");
            unmount (other.home.privateDir);
        }
        else
            std::printf ("  SKIP  .tubamp on another device: hdiutil could not attach an image\n");
    }
    {
        World fat (std::nullopt);
        if (mountFat (fat.home.root / "plugins.dmg", fat.home.plugIns))
        {
            fs::create_directories (fat.home.components);
            fs::copy (fixtures().bundle ("0.3.0"), fat.home.live(), fs::copy_options::recursive);
            check (reason (fat.home.detect()) == R::noAtomicSwap, "a FAT volume without RENAME_SWAP: noAtomicSwap");
            unmount (fat.home.plugIns);
        }
        else
            std::printf ("  SKIP  a volume without RENAME_SWAP: hdiutil could not attach an image\n");
    }
}

void sink()
{
    group ("disk.sink");
    World w;
    const auto site = w.home.site();
    const auto zip = readAll (fixtures().zip ("0.4.0"));
    const auto offer = *offerFor (view (zip), "0.4.0", "0.3.0");

    {
        auto staging = stage (site).value();
        check (fileMode (staging.dir()) == 0700 && staging.dir().parent_path() == w.home.privateDir,
               "staging is a 0700 directory inside .tubamp");
        auto s = ArchiveSink::open (staging, offer);
        check (s && onlyLock (staging.dir()) == "lock ", "while downloading, the staging directory holds only the lock (the archive is unlinked)");
        s.value().write (view (zip));
        check (s.value().received() == zip.size(), "received() counts the bytes written");
        check (bool (std::move (s.value()).finish()), "the exact signed bytes give a VerifiedArchive");
    }
    {
        auto staging = stage (site).value();
        check (staging.dir() != stage (site).value().dir(), "each attempt gets its own mkdtemp name");
    }
    auto finishWith = [&] (const Bytes& bytes)
    {
        auto staging = stage (site).value();
        return download (staging, view (bytes), offer);
    };
    auto shortBy1 = zip; shortBy1.pop_back();
    auto flipped = zip; flipped[flipped.size() / 2] ^= 1;
    auto longBy1 = zip; longBy1.push_back (0);
    check (failedWith (finishWith (shortBy1), Failure::integrity), "one byte short: integrity");
    check (failedWith (finishWith (flipped), Failure::integrity), "one byte changed: integrity");
    check (failedWith (finishWith (longBy1), Failure::integrity), "one byte past the signed size: integrity");
    {
        auto staging = stage (site).value();
        auto s = ArchiveSink::open (staging, offer);
        const bool first = s.value().write (view (zip));
        const std::uint8_t extra = 0;
        check (first && ! s.value().write (ByteView (&extra, 1)) && s.value().received() == zip.size(),
               "the sink refuses the byte past the signed size");
    }
}

ForgedEntry sized (ForgedEntry e, std::uint32_t size) { e.declaredSize = size; return e; }
ForgedEntry flagged (ForgedEntry e, std::uint16_t flags) { e.flags = flags; return e; }
ForgedEntry withCrc (ForgedEntry e, std::uint32_t crc) { e.crc = crc; return e; }

void unpacking()
{
    group ("disk.unpack");
    World w;
    const auto site = w.home.site();

    {
        const auto zip = readAll (fixtures().zip ("0.4.0"));
        auto staging = stage (site).value();
        const auto unpacked = downloadAndUnpack (staging, view (zip), *offerFor (view (zip), "0.4.0", "0.3.0"));
        const auto bundle = staging.dir() / "tubamp.component";
        check (bool (unpacked), "the ditto release zip unpacks");
        check (codeSealIntact (bundle), "the ad-hoc seal survives: codeSealIntact on the unpacked bundle");
        check (codesignVerifies (bundle), "the ad-hoc seal survives: codesign --verify --deep --strict");
        check (fileMode (bundle / "Contents" / "MacOS" / "tubamp") == 0755 && fileMode (bundle / "Contents" / "Info.plist") == 0644
                   && fileMode (bundle / "Contents") == 0755,
               "modes come from the plan: executable 0755, files 0644, directories 0755");
    }

    auto attempt = [&] (const Bytes& zip, Failure expected, const std::string& what, bool nothingWritten)
    {
        auto staging = stage (site).value();
        const auto offer = offerFor (view (zip), "0.4.0", "0.3.0");
        const auto result = downloadAndUnpack (staging, view (zip), *offer);
        check (failedWith (result, expected), what);
        if (nothingWritten)
            check (onlyLock (staging.dir()) == "lock ", what + ": nothing was written");
        return staging.dir();
    };
    auto with = [] (std::vector<ForgedEntry> extra)
    {
        auto entries = forgedBundle();
        entries.insert (entries.end(), extra.begin(), extra.end());
        return forgeZip (entries);
    };

    {
        auto staging = stage (site).value();
        const auto zip = forgeZip (forgedBundle());
        check (bool (downloadAndUnpack (staging, view (zip), *offerFor (view (zip), "0.4.0", "0.3.0"))), "a forged well-formed zip unpacks");
    }
    const std::pair<ForgedEntry, std::string> policy[] = {
        { { "/etc/x", 0100644, "x" }, "an absolute path" },
        { { "tubamp.component/../x", 0100644, "x" }, "a .. segment" },
        { { "other.component/x", 0100644, "x" }, "an entry outside the bundle" },
        { { "__MACOSX/tubamp.component/._Info.plist", 0100644, "x" }, "an AppleDouble entry" },
        { { "tubamp.component/Contents/link", 0120755, "/etc/passwd" }, "a symlink" },
        { { "tubamp.component/Contents/fifo", 010644, "" }, "a FIFO" },
        { { "tubamp.component/Contents/dev", 060644, "" }, "a block device" },
        { { "tubamp.component/Contents/info.PLIST", 0100644, "x" }, "a case-insensitive duplicate" },
        { sized ({ "tubamp.component/big", 0100644, "x" }, 0xfffffffe), "declared sizes over the cap" },
    };
    for (const auto& [entry, what] : policy)
        attempt (with ({ entry }), Failure::archive, "refuses " + what, true);

    std::vector<ForgedEntry> many = forgedBundle();
    for (std::size_t i = many.size(); i <= maxArchiveEntries; ++i)
        many.push_back ({ "tubamp.component/f" + std::to_string (i), 0100644, "x" });
    attempt (forgeZip (many), Failure::archive, "refuses 513 entries", true);
    attempt (with ({ flagged ({ "tubamp.component/enc", 0100644, "x" }, 1) }), Failure::archive, "refuses an encrypted entry", true);
    attempt (with ({ { "tubamp.component/bz", 0100644, "x", 12 } }), Failure::archive, "refuses an unknown compression method", true);
    attempt (forgeZip (forgedBundle(), "trailing"), Failure::archive, "refuses bytes after the end record", true);
    attempt (toBytes ("not a zip at all, just text"), Failure::archive, "refuses a file that is not a zip", true);

    {
        auto e = forgedBundle();
        e.push_back ({ "tubamp.component/Contents/setuid", 0104755, "x" });
        auto staging = stage (site).value();
        const auto zip = forgeZip (e);
        downloadAndUnpack (staging, view (zip), *offerFor (view (zip), "0.4.0", "0.3.0"));
        check (fileMode (staging.dir() / "tubamp.component" / "Contents" / "setuid") == 0644, "a stored 04755 file is written 0644");
    }
    {
        std::vector<ForgedEntry> noDirs = { forgedBundle()[2], forgedBundle()[4] };
        auto staging = stage (site).value();
        const auto zip = forgeZip (noDirs);
        const auto r = downloadAndUnpack (staging, view (zip), *offerFor (view (zip), "0.4.0", "0.3.0"));
        check (r && fileMode (staging.dir() / "tubamp.component" / "Contents" / "MacOS") == 0755,
               "a zip without directory entries gets its parents made 0755");
    }
    {
        auto e = forgedBundle();
        e.push_back (sized ({ "tubamp.component/bomb", 0100644, std::string (100000, 'z'), 8 }, 1000));
        const auto dir = attempt (forgeZip (e), Failure::archive, "refuses an entry that inflates past its declared size", false);
        check (fs::file_size (dir / "tubamp.component" / "bomb") <= 1000, "and writes no byte past the declared size");
    }
    attempt (with ({ withCrc ({ "tubamp.component/crc", 0100644, "data" }, 12345) }), Failure::archive, "refuses a CRC-32 mismatch", false);
    attempt (with ({ sized ({ "tubamp.component/stored", 0100644, "data" }, 3) }), Failure::archive, "refuses a stored entry whose sizes disagree", false);
    {
        ForgedEntry lying { "tubamp.component/a", 0100644, "data" };
        lying.localName = "tubamp.component/b";
        attempt (with ({ lying }), Failure::archive, "refuses a local header that names another file", false);
    }
}

void inspecting()
{
    group ("disk.inspect");
    World w;
    const auto site = w.home.site();
    auto inspectZip = [&] (const SelfUpdatable& s, const fs::path& zipPath, std::string_view offered)
    {
        const auto zip = readAll (zipPath);
        const auto offer = *offerFor (view (zip), offered, "0.3.0");
        auto staging = stage (s).value();
        auto unpacked = downloadAndUnpack (staging, view (zip), offer);
        if (! unpacked)
            return Result<Version, Failure> (unpacked.error());
        auto inspected = inspect (std::move (unpacked).value(), staging, offer, s);
        if (! inspected)
            return Result<Version, Failure> (inspected.error());
        return Result<Version, Failure> (inspected.value().version());
    };

    const auto liveInode = inodeOf (w.home.live());
    const auto good = inspectZip (site, fixtures().zip ("0.4.0"), "0.4.0");
    check (good && good.value() == v ("0.4.0"), "the good bundle at the offered version is inspected");

    const Variant pins[] = { Variant::otherBundleId, Variant::otherType, Variant::otherSubtype, Variant::otherManufacturer,
                             Variant::twoComponents, Variant::otherExecutable, Variant::componentVersionMismatch };
    for (const auto variant : pins)
    {
        check (codesignVerifies (fixtures().bundle ("0.4.0", variant)), std::string ("fixture validly signed: ") + nameOf (variant));
        check (failedWith (inspectZip (site, fixtures().zip ("0.4.0", variant), "0.4.0"), Failure::wrongBundle),
               std::string ("wrongBundle: ") + nameOf (variant));
    }
    check (failedWith (inspectZip (site, fixtures().zip ("0.4.1"), "0.4.0"), Failure::wrongBundle),
           "wrongBundle: a signed 0.4.1 bundle offered as 0.4.0");
    for (const auto variant : { Variant::resourceModified, Variant::executableModified, Variant::fileAdded, Variant::unsignedBundle })
    {
        check (! codesignVerifies (fixtures().bundle ("0.4.0", variant)), std::string ("fixture fails codesign: ") + nameOf (variant));
        check (failedWith (inspectZip (site, fixtures().zip ("0.4.0", variant), "0.4.0"), Failure::wrongBundle),
               std::string ("wrongBundle: ") + nameOf (variant));
    }
    check (inodeOf (w.home.live()) == liveInode && versionOf (w.home.live()) == v ("0.3.0"), "no refusal touched the live bundle");

    World universal (std::nullopt);
    universal.home.installLive (fixtures().bundle ("0.3.0", Variant::good, true));
    check (failedWith (inspectZip (universal.home.site(), fixtures().zip ("0.4.0", Variant::armOnly), "0.4.0"), Failure::wrongBundle),
           "wrongBundle: the live bundle has x86_64 and the new one does not");
    World armLive (std::nullopt);
    armLive.home.installLive (fixtures().bundle ("0.3.0", Variant::armOnly));
    const auto armToArm = inspectZip (armLive.home.site(), fixtures().zip ("0.4.0", Variant::armOnly), "0.4.0");
    if (runningCpuType() == CPU_TYPE_ARM64)
        check (bool (armToArm), "an arm64 live bundle accepts an arm64 update on an arm64 host");
    else
        check (failedWith (armToArm, Failure::wrongBundle), "an x86_64 host refuses an update without its slice");
}


Result<InspectedBundle, Failure> stageThrough (const SelfUpdatable& site, const Staging& staging, ByteView zip, const Offer& offer)
{
    auto unpacked = downloadAndUnpack (staging, zip, offer);
    if (! unpacked)
        return unpacked.error();
    return inspect (std::move (unpacked).value(), staging, offer, site);
}

/** A staging directory whose owner died: a lock file nobody holds, or none at all. */
fs::path staleStaging (const Home& home, std::optional<fs::path> bundle, bool withLockFile = true)
{
    fs::create_directories (home.privateDir);
    auto name = (home.privateDir / "update.XXXXXX").string();
    ::mkdtemp (name.data());
    const fs::path dir (name);
    if (withLockFile)
        writeText (dir / "lock", "");
    if (bundle)
        fs::copy (*bundle, dir / "tubamp.component", fs::copy_options::recursive);
    return dir;
}

void committing()
{
    group ("disk.commit");
    {
        World w;
        const auto site = w.home.site();
        const auto zip = readAll (fixtures().zip ("0.4.0"));
        const auto offer = *offerFor (view (zip), "0.4.0", "0.3.0");
        const auto oldInode = inodeOf (w.home.live());
        {
            auto staging = stage (site).value();
            auto inspected = stageThrough (site, staging, view (zip), offer);
            const auto staged = staging.dir() / "tubamp.component";
            const auto stagedInode = inodeOf (staged);
            const auto result = inspected ? commit (std::move (inspected).value(), staging, site)
                                          : Result<CommitOutcome, Failure> (inspected.error());
            const auto* done = result ? std::get_if<Committed> (&result.value()) : nullptr;
            check (done != nullptr && done->from == v ("0.3.0") && done->to == v ("0.4.0"), "commit swaps 0.3.0 for 0.4.0");
            check (versionOf (w.home.live()) == v ("0.4.0") && inodeOf (w.home.live()) == stagedInode,
                   "the live path now holds the staged directory itself (a rename, not a copy)");
            check (inodeOf (staged) == oldInode && versionOf (staged) == v ("0.3.0"), "the staging directory holds the old bundle");
        }
        const auto disk = reconcile (site);
        check (disk == DiskState { v ("0.4.0"), false }, "reconcile reports 0.4.0 installed");
        check (versionOf (w.home.previous()) == v ("0.3.0") && inodeOf (w.home.previous()) == oldInode,
               "reconcile keeps the old bundle as previous, moved not copied");
        check (w.home.stagingDirs().empty(), "and leaves no staging directory");

        const auto liveInode = inodeOf (w.home.live());
        auto staging = stage (site).value();
        auto again = stageThrough (site, staging, view (zip), offer);
        const auto second = again ? commit (std::move (again).value(), staging, site) : Result<CommitOutcome, Failure> (again.error());
        const auto* already = second ? std::get_if<AlreadyInstalled> (&second.value()) : nullptr;
        check (already != nullptr && already->installed == v ("0.4.0") && inodeOf (w.home.live()) == liveInode,
               "committing 0.4.0 over 0.4.0: AlreadyInstalled, and the live inode is unchanged");
    }
    {
        World w;
        const auto site = w.home.site();
        const auto zip = readAll (fixtures().zip ("0.4.0"));
        const auto offer = *offerFor (view (zip), "0.4.0", "0.3.0");
        auto staging = stage (site).value();
        auto inspected = stageThrough (site, staging, view (zip), offer);
        fs::remove_all (w.home.live());
        check (inspected && failedWith (commit (std::move (inspected).value(), staging, site), Failure::notWritable)
                   && ! fs::exists (w.home.live()),
               "the live bundle vanished before the swap: notWritable, and nothing is put in its place");
    }
    {
        World w;
        const auto site = w.home.site();
        std::atomic<bool> stop { false };
        std::atomic<int> samples { 0 }, misses { 0 };
        std::atomic<std::uint32_t> highest { 0 };
        std::thread poller ([&]
        {
            while (! stop.load())
            {
                const auto version = versionOf (w.home.live());
                const bool hasExecutable = fs::exists (w.home.image (w.home.live()));
                ++samples;
                if (! version || ! hasExecutable)
                    ++misses;
                else if (version->audioComponentVersion() > highest.load())
                    highest = version->audioComponentVersion();
            }
        });
        bool all = true;
        for (const auto* version : { "0.4.0", "0.5.0", "0.6.0" })
            all = committed (installJob (site, fixtures().zip (version), version, "0.3.0")) && all;
        stop = true;
        poller.join();
        check (all && misses.load() == 0 && highest.load() == v ("0.6.0").audioComponentVersion(),
               "the live bundle was readable at every one of " + std::to_string (samples.load())
                   + " samples across three commits (" + std::to_string (misses.load()) + " misses)");
        check (versionOf (w.home.previous()) == v ("0.5.0") && w.home.stagingDirs().empty(),
               "after three installs exactly one previous bundle remains: the last one replaced");
    }
    {
        World w;
        const auto zip = fixtures().zip ("0.4.0");
        Child a ({ "install", w.home.root.string(), zip.string(), "0.4.0", "0.3.0", "race" });
        Child b ({ "install", w.home.root.string(), zip.string(), "0.4.0", "0.3.0", "race" });
        const bool ready = a.waitReady() && b.waitReady();
        a.release();
        b.release();
        const auto ca = a.wait(), cb = b.wait();
        check (ready && ((ca == 0 && cb == 3) || (ca == 3 && cb == 0)),
               "two processes committing at once: one Committed, one AlreadyInstalled (exit " + std::to_string (ca) + ", " + std::to_string (cb) + ")");
        reconcile (w.home.site());
        check (versionOf (w.home.live()) == v ("0.4.0") && versionOf (w.home.previous()) == v ("0.3.0") && w.home.stagingDirs().empty(),
               "after the race: 0.4.0 live, 0.3.0 previous, no staging");
    }
}

void reconciling()
{
    group ("disk.reconcile");
    {
        World w ("0.3.0", "0.2.0");
        const auto site = w.home.site();
        reconcile (site);
        const auto before = listing (w.home.root);
        check (reconcile (site) == DiskState { v ("0.3.0"), false } && listing (w.home.root) == before,
               "a settled disk: reconcile changes nothing");
        fs::copy (w.home.live(), w.home.system / "tubamp.component", fs::copy_options::recursive);
        check (reconcile (site).systemCopy, "a copy in the system folder is reported");
    }
    {
        World w;
        Child holder ({ "hold", w.home.root.string() });
        check (holder.waitReady(), "a child holds a staging directory");
        reconcile (w.home.site());
        check (w.home.stagingDirs().size() == 1, "a staging directory whose owner is alive is left alone");
        holder.release();
        holder.wait();
        reconcile (w.home.site());
        check (w.home.stagingDirs().empty(), "once the owner exits, it is collected");
    }
    {
        World w;
        staleStaging (w.home, std::nullopt, false);
        staleStaging (w.home, fixtures().bundle ("0.4.0"), true);
        const auto target = w.home.root / "target";
        fs::create_directories (target);
        writeText (target / "keep.txt", "keep");
        fs::create_directory_symlink (target, w.home.privateDir / "update.evil");
        reconcile (w.home.site());
        check (w.home.stagingDirs().empty(), "directories with no lock file or a lock nobody holds are collected");
        check (fs::exists (target / "keep.txt"), "a symlink named like staging is removed, its target untouched");
    }
    {
        World w;
        const auto target = w.home.root / "target";
        fs::create_directories (target / "tubamp.component");
        writeText (target / "tubamp.component" / "keep.txt", "keep");
        fs::create_directories (w.home.privateDir);
        fs::create_directory_symlink (target, w.home.privateDir / "previous");
        reconcile (w.home.site());
        check (! fs::exists (fs::symlink_status (w.home.privateDir / "previous")) && fs::exists (target / "tubamp.component" / "keep.txt"),
               "previous/ as a symlink is unlinked, never followed");
    }

    struct Row
    {
        const char* what;
        const char* live;
        std::optional<Variant> previous;
        const char* previousVersion;
        std::vector<const char*> stale;
        const char* expectedPrevious;   // nullptr: none
    };
    const Row rows[] = {
        { "slot 0.3.0 < stale 0.4.0 < installed 0.5.0: the stale one moves in", "0.5.0", Variant::good, "0.3.0", { "0.4.0" }, "0.4.0" },
        { "stale 0.3.0 < slot 0.4.0: the slot stays", "0.5.0", Variant::good, "0.4.0", { "0.3.0" }, "0.4.0" },
        { "slot 0.6.0 above installed 0.5.0: discarded", "0.5.0", Variant::good, "0.6.0", {}, nullptr },
        { "stale 0.5.0, the installed version: deleted, never kept", "0.5.0", std::nullopt, nullptr, { "0.5.0" }, nullptr },
        { "unsealed slot: discarded, the sealed stale 0.3.0 kept", "0.5.0", Variant::resourceModified, "0.4.0", { "0.3.0" }, "0.3.0" },
        { "two stale bundles: the higher below installed wins", "0.5.0", std::nullopt, nullptr, { "0.3.0", "0.4.0" }, "0.4.0" },
    };
    for (const auto& row : rows)
    {
        World w (row.live);
        if (row.previous)
            w.home.installPrevious (fixtures().bundle (row.previousVersion, *row.previous));
        for (const auto* version : row.stale)
            staleStaging (w.home, fixtures().bundle (version));
        const auto slotInode = inodeOf (w.home.previous());
        const auto site = w.home.site();
        reconcile (site);
        const auto kept = versionOf (w.home.previous());
        const bool right = row.expectedPrevious ? kept == v (row.expectedPrevious) : ! fs::exists (w.home.previous());
        check (right && w.home.stagingDirs().empty(), row.what);
        if (row.previous && row.expectedPrevious && std::string (row.expectedPrevious) == row.previousVersion)
            check (inodeOf (w.home.previous()) == slotInode, std::string (row.what) + " (same inode)");
        const auto settled = listing (w.home.root);
        reconcile (site);
        check (listing (w.home.root) == settled, std::string (row.what) + ": a second reconcile changes nothing");
    }
    {
        World w ("0.5.0", "0.3.0");
        staleStaging (w.home, fixtures().bundle ("0.4.0"));
        const auto site = w.home.site();
        const auto slotInode = inodeOf (w.home.previous());
        writeText (w.home.live() / "Contents" / "Info.plist", "damaged");
        check (! reconcile (site).installed && w.home.stagingDirs().empty() && inodeOf (w.home.previous()) == slotInode,
               "an unreadable installed version: leftovers deleted, previous left alone");
    }
    {
        World w;
        fs::create_directories (w.home.support.dir);
        writeText (w.home.support.dir / "prefs.json", "{}");
        writeText (w.home.support.dir / "prefs.json.tmp", "{\"autoC");
        writeText (w.home.support.dir / "latest.signed.tmp", "tubamp-update-v1\n");
        reconcile (w.home.site());
        check (! fs::exists (w.home.support.dir / "prefs.json.tmp") && ! fs::exists (w.home.support.dir / "latest.signed.tmp")
                   && fs::exists (w.home.support.dir / "prefs.json"),
               "temporary files of a dead writer are deleted, the real files kept");
    }
}

void crashTable()
{
    group ("disk.crash");
    struct Row
    {
        const char* fault;
        const char* what;
        bool committedBeforeDeath;
        std::uint64_t fileSizeLimit = 0;
    };

    std::uint64_t largest = 0;
    for (const auto& entry : fs::recursive_directory_iterator (fixtures().bundle ("0.4.0")))
        if (entry.is_regular_file())
            largest = std::max<std::uint64_t> (largest, entry.file_size());

    const std::vector<Row> rows = {
        { "afterStaging", "Download: staging made, nothing fetched", false },
        { "midDownload", "Download: half the archive written", false },
        { "afterDownload", "Download: archive verified", false },
        { "midUnpack", "Unpack: SIGXFSZ on the first file over 512 bytes", false, 512 },
        { "midUnpack", "Unpack: SIGXFSZ inside the largest file", false, largest / 2 },
        { "afterUnpack", "Unpack: complete", false },
        { "afterInspect", "Inspect: complete bundle 0.4.0 staged", false },
        { "afterCommit", "Commit: swapped, staging still locked", true },
        { "none", "no crash: the whole job", true },
    };

    for (const auto& row : rows)
    {
        World w ("0.3.0", "0.2.0");
        const auto liveInode = inodeOf (w.home.live());
        std::vector<std::string> args { "install", w.home.root.string(), fixtures().zip ("0.4.0").string(), "0.4.0", "0.3.0", row.fault };
        if (row.fileSizeLimit != 0)
            args.push_back (std::to_string (row.fileSizeLimit));
        Child child (args);
        const auto code = child.wait();
        const std::string name = std::string (row.what) + ": ";

        const bool died = std::string (row.fault) == "none" ? code == 0
                        : row.fileSizeLimit != 0          ? code == 128 + SIGXFSZ
                                                          : code == 42;
        check (died, name + "the child ended as planned (exit " + std::to_string (code) + ")");
        if (row.fileSizeLimit != 0)
        {
            const auto dirs = w.home.stagingDirs();
            const bool partial = dirs.size() == 1 && fs::exists (dirs.front() / "tubamp.component")
                              && ! sealedVersion (dirs.front() / "tubamp.component");
            check (partial, name + "it died with a partial, unsealed bundle staged");
        }
        if (! row.committedBeforeDeath)
            check (inodeOf (w.home.live()) == liveInode && versionOf (w.home.live()) == v ("0.3.0"), name + "the live bundle is untouched");
        else
            check (versionOf (w.home.live()) == v ("0.4.0"), name + "0.4.0 is live");

        const auto site = w.home.site();
        reconcile (site);
        const auto expectedLive = row.committedBeforeDeath ? "0.4.0" : "0.3.0";
        const auto expectedPrevious = row.committedBeforeDeath ? "0.3.0" : "0.2.0";
        check (versionOf (w.home.live()) == v (expectedLive) && versionOf (w.home.previous()) == v (expectedPrevious)
                   && w.home.stagingDirs().empty(),
               name + "reconcile leaves " + expectedLive + " live, " + expectedPrevious + " previous, no staging");
        const auto settled = listing (w.home.root);
        reconcile (site);
        check (listing (w.home.root) == settled, name + "a second reconcile changes nothing");

        if (! row.committedBeforeDeath)
        {
            const auto retry = installJob (site, fixtures().zip ("0.4.0"), "0.4.0", "0.3.0");
            check (committed (retry) && versionOf (w.home.live()) == v ("0.4.0") && versionOf (w.home.previous()) == v ("0.3.0")
                       && w.home.stagingDirs().empty(),
                   name + "the next attempt ends exactly like an uninterrupted one");
        }
    }
}

void killSweeps()
{
    group ("disk.kill");
    int interrupted = 0, cases = 0;
    for (const int micros : { 0, 50, 150, 400, 1000, 2500, 6000, 15000 })
    {
        World w ("0.4.0", "0.2.0");
        staleStaging (w.home, fixtures().bundle ("0.3.0"));
        const auto bulky = staleStaging (w.home, std::nullopt);
        for (int i = 0; i < 400; ++i)
            writeText (bulky / ("f" + std::to_string (i)), "x");
        const auto liveInode = inodeOf (w.home.live());

        Child child ({ "reconcile", w.home.root.string() });
        child.waitReady();
        std::this_thread::sleep_for (std::chrono::microseconds (micros));
        child.kill();
        interrupted += child.wait() == 128 + SIGKILL ? 1 : 0;
        ++cases;

        reconcile (w.home.site());
        check (inodeOf (w.home.live()) == liveInode && versionOf (w.home.previous()) == v ("0.3.0") && w.home.stagingDirs().empty(),
               "SIGKILL " + std::to_string (micros) + " us into reconcile: the next reconcile converges");
    }
    std::printf ("  note  %d of %d reconcile children were killed before they finished\n", interrupted, cases);

    for (const int millis : { 1, 5, 20 })
    {
        World w;
        Child child ({ "prefs-loop", w.home.support.dir.string() });
        child.waitReady();
        std::this_thread::sleep_for (std::chrono::milliseconds (millis));
        child.kill();
        child.wait();
        check (loadPrefs (w.home.support).disclosed, "SIGKILL " + std::to_string (millis) + " ms into a stream of prefs writes: prefs.json is a complete file");
        reconcile (w.home.site());
        check (! fs::exists (w.home.support.dir / "prefs.json.tmp"), "and reconcile removes any temporary it left");
    }
}

// ---------------------------------------------------------------------------------
// The child side. Exit codes: 0 Committed, 3 AlreadyInstalled, 10 + Failure, 42 at the
// fault point (by _exit: no destructor runs, like a crash).
// ---------------------------------------------------------------------------------

int childInstall (const Home& home, const fs::path& zipPath, std::string_view version, std::string_view running,
                  std::string_view fault, std::uint64_t fileSizeLimit)
{
    auto at = [&] (std::string_view point) { if (fault == point) ::_exit (42); };
    const auto site = home.site();
    const auto zip = readAll (zipPath);
    const auto offer = *offerFor (view (zip), version, running);
    auto failed = [] (Failure f) { return 10 + int (f); };

    int code = 0;
    {
        auto staging = stage (site);
        if (! staging)
            return failed (staging.error());
        at ("afterStaging");

        auto sink = ArchiveSink::open (staging.value(), offer);
        if (! sink)
            return failed (sink.error());
        const auto half = zip.size() / 2;
        sink.value().write (view (zip).first (half));
        at ("midDownload");
        sink.value().write (view (zip).subspan (half));
        auto archive = std::move (sink.value()).finish();
        if (! archive)
            return failed (archive.error());
        at ("afterDownload");

        if (fileSizeLimit != 0)
        {
            const rlimit noCore { 0, 0 }, limit { rlim_t (fileSizeLimit), rlim_t (fileSizeLimit) };
            ::setrlimit (RLIMIT_CORE, &noCore);
            ::setrlimit (RLIMIT_FSIZE, &limit);
        }
        auto unpacked = unpack (std::move (archive).value(), staging.value());
        if (! unpacked)
            return failed (unpacked.error());
        at ("afterUnpack");

        auto inspected = inspect (std::move (unpacked).value(), staging.value(), offer, site);
        if (! inspected)
            return failed (inspected.error());
        at ("afterInspect");

        if (fault == "race")
        {
            childReady();
            childAwaitRelease();
        }
        const auto result = commit (std::move (inspected).value(), staging.value(), site);
        if (! result)
            return failed (result.error());
        at ("afterCommit");
        code = std::holds_alternative<Committed> (result.value()) ? 0 : 3;
    }
    reconcile (site);
    return code;
}
} // namespace

void runDiskTests (int& passed, int& failed)
{
    passedCount = &passed;
    failedCount = &failed;
    std::printf ("disk fixtures: %s\n", fixtures().description().c_str());
    prefsStore();
    envelopeCache();
    site();
    sink();
    unpacking();
    inspecting();
    committing();
    reconciling();
    crashTable();
    killSweeps();
}

int runDiskChild (int argc, char** argv)
{
    const std::vector<std::string> args (argv + 2, argv + argc);
    if (args.size() == 3 && args[0] == "claim")
    {
        childReady();
        childAwaitRelease();
        return claimDailyCheck ({ args[1] }, std::chrono::sys_seconds (std::chrono::seconds (std::stoll (args[2])))) ? 0 : 1;
    }
    if (args.size() == 2 && args[0] == "hold")
    {
        const Home home (args[1]);
        const auto staging = Staging::create (home.site());
        childReady();
        childAwaitRelease();
        return staging ? 0 : 1;
    }
    if (args.size() == 2 && args[0] == "reconcile")
    {
        const Home home (args[1]);
        const auto site = home.site();
        childReady();
        reconcile (site);
        return 0;
    }
    if (args.size() == 2 && args[0] == "prefs-loop")
    {
        const SupportPaths support { args[1] };
        mutatePrefs (support, PrefsPatch { true, true, {} });
        childReady();
        for (bool on = false;; on = ! on)
            mutatePrefs (support, PrefsPatch { on, true, {} });
    }
    if ((args.size() == 6 || args.size() == 7) && args[0] == "install")
        return childInstall (Home (args[1]), args[2], args[3], args[4], args[5],
                             args.size() == 7 ? std::stoull (args[6]) : 0);
    std::fprintf (stderr, "unknown child command\n");
    return 2;
}
} // namespace tubamp::update::test
