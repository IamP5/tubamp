#include "Files.h"

#include <cerrno>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tubamp::update::detail
{
void Fd::reset (int descriptor) noexcept
{
    if (fd >= 0)
        ::close (fd);
    fd = descriptor;
}

std::optional<SupportLock> SupportLock::acquire (const SupportPaths& support, const CancelToken& stop)
{
    std::error_code ignored;
    fs::create_directories (support.dir, ignored);
    Fd fd (::open ((support.dir / "lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (! fd)
        return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + supportLockPatience;
    while (::flock (fd.get(), LOCK_EX | LOCK_NB) != 0)
    {
        if ((errno != EWOULDBLOCK && errno != EINTR) || stop.requested()
            || std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for (supportLockPoll);
    }
    return SupportLock (std::move (fd));
}

std::optional<Bytes> readFile (const fs::path& path, std::size_t cap)
{
    Fd fd (::open (path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st {};
    if (! fd || ::fstat (fd.get(), &st) != 0 || ! S_ISREG (st.st_mode) || std::uint64_t (st.st_size) > cap)
        return std::nullopt;

    Bytes bytes (std::size_t (st.st_size));
    for (std::size_t done = 0; done < bytes.size();)
    {
        const auto n = ::read (fd.get(), bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return std::nullopt;
        done += std::size_t (n);
    }
    return bytes;
}

bool writeAll (int fd, ByteView bytes) noexcept
{
    for (std::size_t done = 0; done < bytes.size();)
    {
        const auto n = ::write (fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += std::size_t (n);
    }
    return true;
}

bool replaceFile (const fs::path& path, ByteView bytes)
{
    auto temporary = path;
    temporary += ".tmp";
    Fd fd (::open (temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd && writeAll (fd.get(), bytes) && ::fsync (fd.get()) == 0)
    {
        fd.reset();
        if (::rename (temporary.c_str(), path.c_str()) == 0)
            return true;
    }
    ::unlink (temporary.c_str());
    return false;
}

Failure failureOf (int err) noexcept
{
    return (err == ENOSPC || err == EDQUOT) ? Failure::noSpace : Failure::notWritable;
}
} // namespace tubamp::update::detail
