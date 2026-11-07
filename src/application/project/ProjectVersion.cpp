#include "Horo/Application/ProjectVersion.h"

#include "ProjectVersionErrors.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <vector>

namespace Horo::Application {
    namespace {
        constexpr std::size_t kMaximumVersionBytes = 64;

        Result<std::uint32_t> ParseNumericComponent(const std::string_view component) {
            if (component.empty() || (component.size() > 1 && component.front() == '0')) {
                return Result<std::uint32_t>::Failure(MakeError(ProjectErrors::VersionInvalid));
            }
            std::uint32_t value{};
            if (const auto [end, error] = std::from_chars(component.data(), component.data() + component.size(), value);
                error != std::errc{} || end != component.data() + component.size()) {
                return Result<std::uint32_t>::Failure(MakeError(ProjectErrors::VersionInvalid));
            }
            return Result<std::uint32_t>::Success(value);
        }

        bool IsPrereleaseCharacter(const char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-';
        }

        bool IsNumericIdentifier(const std::string_view value) noexcept {
            return !value.empty() && std::ranges::all_of(value, [](const char c) {
                return c >= '0' && c <= '9';
            });
        }

        std::vector<std::string_view> SplitIdentifiers(const std::string_view value) {
            std::vector<std::string_view> identifiers;
            for (std::size_t begin = 0;;) {
                const std::size_t end = value.find('.', begin);
                identifiers.push_back(value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin));
                if (end == std::string_view::npos)
                    break;
                begin = end + 1;
            }
            return identifiers;
        }

        template <typename Hash> Result<Hash> ParseHash(const std::string_view text) {
            constexpr std::string_view prefix = "sha256:";
            if (text.size() != prefix.size() + 64 || !text.starts_with(prefix)) {
                return Result<Hash>::Failure(MakeError(ProjectErrors::HashInvalid));
            }
            Hash result;
            const auto hexValue = [](const char c) {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            const std::string_view encoded = text.substr(prefix.size());
            auto cursor = encoded.begin();
            for (auto &byte : result.bytes) {
                const int high = hexValue(*cursor++);
                const int low = hexValue(*cursor++);
                if (high < 0 || low < 0) {
                    return Result<Hash>::Failure(MakeError(ProjectErrors::HashInvalid));
                }
                const auto parsed = static_cast<std::byte>((high << 4) | low);
                byte = std::to_integer<std::uint8_t>(parsed);
            }
            return Result<Hash>::Success(result);
        }

        template <typename Hash> std::string FormatHash(const Hash &hash) {
            constexpr char digits[] = "0123456789abcdef";
            std::string result = "sha256:";
            result.reserve(71);
            for (const std::uint8_t value : hash.bytes) {
                const auto byte = static_cast<std::byte>(value);
                const auto numeric = std::to_integer<unsigned int>(byte);
                result.push_back(digits[numeric >> 4]);
                result.push_back(digits[numeric & 0x0f]);
            }
            return result;
        }
    }  // namespace

    /** @copydoc ParseHoroVersion */
    Result<HoroVersion> ParseHoroVersion(const std::string_view text) {
        if (text.empty() || text.size() > kMaximumVersionBytes || text.find('+') != std::string_view::npos) {
            return Result<HoroVersion>::Failure(MakeError(ProjectErrors::VersionInvalid));
        }
        const std::size_t dash = text.find('-');
        const std::string_view core = text.substr(0, dash);
        const std::size_t firstDot = core.find('.');
        const std::size_t secondDot = firstDot == std::string_view::npos ? firstDot : core.find('.', firstDot + 1);
        if (firstDot == std::string_view::npos || secondDot == std::string_view::npos ||
            core.find('.', secondDot + 1) != std::string_view::npos) {
            return Result<HoroVersion>::Failure(MakeError(ProjectErrors::VersionInvalid));
        }

        auto major = ParseNumericComponent(core.substr(0, firstDot));
        auto minor = ParseNumericComponent(core.substr(firstDot + 1, secondDot - firstDot - 1));
        auto patch = ParseNumericComponent(core.substr(secondDot + 1));
        if (major.HasError() || minor.HasError() || patch.HasError()) {
            return Result<HoroVersion>::Failure(MakeError(ProjectErrors::VersionInvalid));
        }

        std::string prerelease;
        if (dash != std::string_view::npos) {
            const std::string_view value = text.substr(dash + 1);
            for (const std::string_view identifier : SplitIdentifiers(value)) {
                if (identifier.empty() || !std::ranges::all_of(identifier, IsPrereleaseCharacter) ||
                    (IsNumericIdentifier(identifier) && identifier.size() > 1 && identifier.front() == '0')) {
                    return Result<HoroVersion>::Failure(MakeError(ProjectErrors::VersionInvalid));
                }
            }
            prerelease.assign(value);
        }
        return Result<HoroVersion>::Success({major.Value(), minor.Value(), patch.Value(), std::move(prerelease)});
    }

    /** @copydoc FormatHoroVersion */
    std::string FormatHoroVersion(const HoroVersion &version) {
        return version.prerelease.empty() ? std::format("{}.{}.{}", version.major, version.minor, version.patch)
                                          : std::format("{}.{}.{}-{}", version.major, version.minor, version.patch, version.prerelease);
    }

    /** @copydoc CompareHoroVersions */
    std::strong_ordering CompareHoroVersions(const HoroVersion &lhs, const HoroVersion &rhs) noexcept {
        if (lhs.major != rhs.major)
            return lhs.major <=> rhs.major;
        if (lhs.minor != rhs.minor)
            return lhs.minor <=> rhs.minor;
        if (lhs.patch != rhs.patch)
            return lhs.patch <=> rhs.patch;
        if (lhs.prerelease.empty() != rhs.prerelease.empty()) {
            return lhs.prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
        }
        if (lhs.prerelease == rhs.prerelease)
            return std::strong_ordering::equal;

        const auto left = SplitIdentifiers(lhs.prerelease);
        const auto right = SplitIdentifiers(rhs.prerelease);
        const std::size_t common = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < common; ++i) {
            const bool leftNumeric = IsNumericIdentifier(left[i]);
            if (const bool rightNumeric = IsNumericIdentifier(right[i]); leftNumeric != rightNumeric)
                return leftNumeric ? std::strong_ordering::less : std::strong_ordering::greater;
            if (left[i] == right[i])
                continue;
            if (leftNumeric && left[i].size() != right[i].size())
                return left[i].size() <=> right[i].size();
            return left[i] < right[i] ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return left.size() <=> right.size();
    }

    /** @copydoc ParsePersistentContractHash */
    Result<PersistentContractHash> ParsePersistentContractHash(const std::string_view text) {
        return ParseHash<PersistentContractHash>(text);
    }

    /** @copydoc FormatPersistentContractHash */
    std::string FormatPersistentContractHash(const PersistentContractHash &hash) {
        return FormatHash(hash);
    }

    /** @copydoc ParseCompatibilityDecisionHash */
    Result<CompatibilityDecisionHash> ParseCompatibilityDecisionHash(const std::string_view text) {
        return ParseHash<CompatibilityDecisionHash>(text);
    }

    /** @copydoc FormatCompatibilityDecisionHash */
    std::string FormatCompatibilityDecisionHash(const CompatibilityDecisionHash &hash) {
        return FormatHash(hash);
    }
}  // namespace Horo::Application
