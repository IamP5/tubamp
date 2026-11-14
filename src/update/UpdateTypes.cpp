#include "UpdateTypes.h"

namespace tubamp::update
{
namespace
{
    std::optional<std::uint32_t> parsePart (std::string_view s) noexcept
    {
        if (s.empty() || s.size() > 5 || (s.size() > 1 && s.front() == '0'))
            return std::nullopt;

        std::uint32_t value = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return std::nullopt;
            value = value * 10 + std::uint32_t (c - '0');
        }
        return value;
    }
}

std::optional<Version> Version::parse (std::string_view text) noexcept
{
    const auto a = text.find ('.');
    if (a == std::string_view::npos)
        return std::nullopt;
    const auto b = text.find ('.', a + 1);
    if (b == std::string_view::npos)
        return std::nullopt;

    const auto major = parsePart (text.substr (0, a));
    const auto minor = parsePart (text.substr (a + 1, b - a - 1));
    const auto patch = parsePart (text.substr (b + 1));

    if (! major || ! minor || ! patch || *major > 0xffff || *minor > 0xff || *patch > 0xff)
        return std::nullopt;

    return Version { std::uint16_t (*major), std::uint8_t (*minor), std::uint8_t (*patch) };
}

std::string Version::toString() const
{
    return std::to_string (major) + "." + std::to_string (minor) + "." + std::to_string (patch);
}
} // namespace tubamp::update
