#include "Bundle.h"
#include "Files.h"

#include <zlib.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace tubamp::update::detail
{
VerifiedArchive::VerifiedArchive (int descriptor) : fd (descriptor) {}
VerifiedArchive::VerifiedArchive (VerifiedArchive&& other) noexcept : fd (std::exchange (other.fd, -1)) {}
VerifiedArchive::~VerifiedArchive() { Fd closed (fd); }

ArchiveSink::ArchiveSink (int descriptor, ArchiveSpec spec) : fd (descriptor), expected (spec) {}

ArchiveSink::ArchiveSink (ArchiveSink&& other) noexcept
    : fd (std::exchange (other.fd, -1)), count (other.count), expected (other.expected), hash (other.hash),
      fault (other.fault)
{
}

ArchiveSink::~ArchiveSink() { Fd closed (fd); }

Result<ArchiveSink, Failure> ArchiveSink::open (const Staging& staging, const Offer& offer)
{
    const auto path = staging.dir() / "archive";
    Fd fd (::open (path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (! fd || ::unlink (path.c_str()) != 0)
        return failureOf (errno);
    return ArchiveSink (fd.release(), offer.archive());
}

bool ArchiveSink::write (ByteView bytes) noexcept
{
    if (fault)
        return false;
    if (bytes.size() > expected.size - count)
        fault = Failure::integrity;
    else if (! writeAll (fd, bytes))
        fault = failureOf (errno);
    if (fault)
        return false;
    hash.update (bytes);
    count += bytes.size();
    return true;
}

Result<VerifiedArchive, Failure> ArchiveSink::finish() &&
{
    if (fault)
        return *fault;
    if (count != expected.size || hash.finish() != expected.sha256)
        return Failure::integrity;
    return VerifiedArchive (std::exchange (fd, -1));
}

namespace
{
    constexpr std::uint32_t endOfCentralSignature = 0x06054b50, centralSignature = 0x02014b50,
                            localSignature = 0x04034b50;
    constexpr std::uint8_t unixHost = 3;
    constexpr std::size_t chunk = 64 * 1024;

    std::uint16_t le16 (const std::uint8_t* p) { return std::uint16_t (p[0] | (p[1] << 8)); }
    std::uint32_t le32 (const std::uint8_t* p) { return std::uint32_t (le16 (p)) | (std::uint32_t (le16 (p + 2)) << 16); }

    bool readAt (int fd, std::uint64_t offset, std::uint8_t* out, std::size_t n)
    {
        for (std::size_t done = 0; done < n;)
        {
            const auto got = ::pread (fd, out + done, n - done, off_t (offset + done));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return false;
            done += std::size_t (got);
        }
        return true;
    }

    struct ZipEntry
    {
        ArchiveEntry entry;   // path is the raw stored name
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint64_t compressed = 0, localOffset = 0;
    };

    struct CentralDirectory
    {
        std::vector<ZipEntry> entries;
        std::uint64_t start = 0;   // file data must end before this
    };

    EntryKind kindOf (std::uint16_t madeBy, std::uint32_t external, const std::string& name)
    {
        const auto mode = (madeBy >> 8) == unixHost ? (external >> 16) & S_IFMT : 0;
        switch (mode)
        {
            case S_IFDIR: return EntryKind::directory;
            case S_IFREG: return EntryKind::file;
            case S_IFLNK: return EntryKind::symlink;
            case 0: return (! name.empty() && name.back() == '/') ? EntryKind::directory : EntryKind::file;
            default: return EntryKind::other;
        }
    }

    std::optional<CentralDirectory> readCentralDirectory (int fd, std::uint64_t fileSize)
    {
        constexpr std::size_t endSize = 22;
        if (fileSize < endSize)
            return std::nullopt;
        const auto tailSize = std::size_t (std::min<std::uint64_t> (fileSize, endSize + 0xffff));
        std::vector<std::uint8_t> tail (tailSize);
        if (! readAt (fd, fileSize - tailSize, tail.data(), tailSize))
            return std::nullopt;

        std::optional<std::size_t> found;
        for (std::size_t i = tailSize - endSize + 1; i-- > 0;)
            if (le32 (&tail[i]) == endOfCentralSignature && i + endSize + le16 (&tail[i + 20]) == tailSize)
            {
                found = i;
                break;
            }
        if (! found)
            return std::nullopt;

        const auto* end = &tail[*found];
        const auto endOffset = fileSize - tailSize + *found;
        const std::uint64_t count = le16 (end + 10), size = le32 (end + 12), offset = le32 (end + 16);
        if (le16 (end + 4) != 0 || le16 (end + 6) != 0 || le16 (end + 8) != count || count > maxArchiveEntries
            || offset > endOffset || size != endOffset - offset)
            return std::nullopt;

        std::vector<std::uint8_t> cd (std::size_t (size) + 1);
        if (! readAt (fd, offset, cd.data(), std::size_t (size)))
            return std::nullopt;

        CentralDirectory dir { {}, offset };
        std::size_t at = 0;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            constexpr std::size_t fixed = 46;
            if (size - at < fixed || le32 (&cd[at]) != centralSignature)
                return std::nullopt;
            const auto* h = &cd[at];
            const std::size_t nameLength = le16 (h + 28), extra = le16 (h + 30), comment = le16 (h + 32);
            if (size - at - fixed < nameLength + extra + comment)
                return std::nullopt;

            const auto flags = le16 (h + 8), method = le16 (h + 10);
            const std::uint32_t compressed = le32 (h + 20), uncompressed = le32 (h + 24), local = le32 (h + 42);
            if ((flags & 0x0041) != 0 || (method != 0 && method != 8)
                || compressed == 0xffffffff || uncompressed == 0xffffffff || local == 0xffffffff)
                return std::nullopt;

            std::string name (reinterpret_cast<const char*> (h + fixed), nameLength);
            const auto kind = kindOf (le16 (h + 4), le32 (h + 38), name);
            dir.entries.push_back ({ { std::move (name), uncompressed, kind }, method, le32 (h + 16), compressed, local });
            at += fixed + nameLength + extra + comment;
        }
        if (at != size)
            return std::nullopt;
        return dir;
    }

    using Step = std::optional<Failure>;

    /** The directory that will hold the entry's last component, creating missing parents. */
    std::variant<Fd, Failure> enterParent (int root, const std::vector<std::string>& components)
    {
        Fd dir (::dup (root));
        for (std::size_t i = 0; dir && i + 1 < components.size(); ++i)
        {
            const bool made = ::mkdirat (dir.get(), components[i].c_str(), 0755) == 0;
            if (! made && errno != EEXIST)
                return failureOf (errno);
            Fd next (::openat (dir.get(), components[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (! next)
                return (errno == ENOTDIR || errno == ELOOP) ? Failure::archive : failureOf (errno);
            if (made && ::fchmod (next.get(), 0755) != 0)
                return failureOf (errno);
            dir = std::move (next);
        }
        if (! dir)
            return failureOf (errno);
        return dir;
    }

    Step makeDirectory (int parent, const std::string& name, unsigned mode)
    {
        if (::mkdirat (parent, name.c_str(), 0700) != 0 && errno != EEXIST)
            return failureOf (errno);
        Fd dir (::openat (parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (! dir)
            return (errno == ENOTDIR || errno == ELOOP) ? Failure::archive : failureOf (errno);
        if (::fchmod (dir.get(), mode_t (mode)) != 0)
            return failureOf (errno);
        return std::nullopt;
    }

    /** The entry's data region, after checking that its local header names the same file. */
    std::optional<std::uint64_t> dataStart (int archive, const ZipEntry& z, std::uint64_t centralStart)
    {
        std::uint8_t local[30];
        if (! readAt (archive, z.localOffset, local, sizeof local) || le32 (local) != localSignature)
            return std::nullopt;
        const std::size_t nameLength = le16 (local + 26), extra = le16 (local + 28);
        std::string name (nameLength, '\0');
        if (! readAt (archive, z.localOffset + sizeof local, reinterpret_cast<std::uint8_t*> (name.data()), nameLength)
            || name != z.entry.path)
            return std::nullopt;
        const auto start = z.localOffset + sizeof local + nameLength + extra;
        if (start > centralStart || z.compressed > centralStart - start)
            return std::nullopt;
        return start;
    }

    /** Copies or inflates exactly `size` bytes into `out`, checking the CRC-32. */
    Step writeContents (int archive, const ZipEntry& z, std::uint64_t start, std::uint64_t size, int out)
    {
        std::vector<std::uint8_t> in (chunk), produced (chunk);
        std::uint64_t written = 0, consumed = 0;
        auto crc = ::crc32 (0, nullptr, 0);
        auto emit = [&] (const std::uint8_t* bytes, std::size_t n) -> Step
        {
            if (n > size - written)
                return Failure::archive;
            if (! writeAll (out, ByteView (bytes, n)))
                return failureOf (errno);
            crc = ::crc32 (crc, bytes, uInt (n));
            written += n;
            return std::nullopt;
        };

        if (z.method == 0)
        {
            if (z.compressed != size)
                return Failure::archive;
            while (consumed < size)
            {
                const auto n = std::size_t (std::min<std::uint64_t> (chunk, size - consumed));
                if (! readAt (archive, start + consumed, in.data(), n))
                    return Failure::archive;
                consumed += n;
                if (auto failure = emit (in.data(), n))
                    return failure;
            }
        }
        else
        {
            z_stream stream {};
            if (::inflateInit2 (&stream, -MAX_WBITS) != Z_OK)
                return Failure::archive;
            struct End { z_stream& s; ~End() { ::inflateEnd (&s); } } end { stream };

            for (int status = Z_OK; status != Z_STREAM_END;)
            {
                if (stream.avail_in == 0)
                {
                    const auto n = std::size_t (std::min<std::uint64_t> (chunk, z.compressed - consumed));
                    if (n == 0 || ! readAt (archive, start + consumed, in.data(), n))
                        return Failure::archive;
                    consumed += n;
                    stream.next_in = in.data();
                    stream.avail_in = uInt (n);
                }
                stream.next_out = produced.data();
                stream.avail_out = uInt (produced.size());
                status = ::inflate (&stream, Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END)
                    return Failure::archive;
                if (auto failure = emit (produced.data(), produced.size() - stream.avail_out))
                    return failure;
            }
        }

        if (written != size || crc != z.crc)
            return Failure::archive;
        if (::fsync (out) != 0)
            return failureOf (errno);
        return std::nullopt;
    }

    Step extractFile (int archive, const ZipEntry& z, std::uint64_t centralStart, const PlannedEntry& planned, int parent)
    {
        const auto start = dataStart (archive, z, centralStart);
        if (! start)
            return Failure::archive;
        Fd out (::openat (parent, planned.components.back().c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (! out)
            return errno == EEXIST ? Failure::archive : failureOf (errno);
        if (::fchmod (out.get(), mode_t (planned.mode)) != 0)
            return failureOf (errno);
        return writeContents (archive, z, *start, planned.size, out.get());
    }
}

Result<UnpackedBundle, Failure> unpack (VerifiedArchive&& verified, const Staging& staging)
{
    const VerifiedArchive archive (std::move (verified));
    struct stat st {};
    if (::fstat (archive.descriptor(), &st) != 0)
        return Failure::archive;
    const auto zip = readCentralDirectory (archive.descriptor(), std::uint64_t (st.st_size));
    if (! zip)
        return Failure::archive;

    std::vector<ArchiveEntry> entries;
    for (const auto& z : zip->entries)
        entries.push_back (z.entry);
    const auto plan = planExtraction (entries);
    if (! plan)
        return Failure::archive;

    Fd root (::open (staging.dir().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (! root)
        return failureOf (errno);

    // planExtraction returns one PlannedEntry per ArchiveEntry, in order.
    for (std::size_t i = 0; i < plan.value().size(); ++i)
    {
        const auto& planned = plan.value()[i];
        auto parent = enterParent (root.get(), planned.components);
        if (const auto* failure = std::get_if<Failure> (&parent))
            return *failure;
        const int dir = std::get<Fd> (parent).get();
        const auto step = planned.kind == EntryKind::directory
                              ? makeDirectory (dir, planned.components.back(), planned.mode)
                              : extractFile (archive.descriptor(), zip->entries[i], zip->start, planned, dir);
        if (step)
            return *step;
    }
    return UnpackedBundle();
}
} // namespace tubamp::update::detail
