#include "Horo/Physics/PhysicsFilterIdentity.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Converts one lowercase hexadecimal digit, rejecting uppercase and non-hex input. */
        [[nodiscard]] std::optional<std::byte> ParseHexDigit(const char value) noexcept {
            if (value >= '0' && value <= '9')
                return static_cast<std::byte>(value - '0');
            if (value >= 'a' && value <= 'f')
                return static_cast<std::byte>(value - 'a' + 10);
            return std::nullopt;
        }

        /** @brief Checks exact UUID length and separator placement. */
        [[nodiscard]] bool HasCanonicalUuidShape(const std::string_view value) noexcept {
            constexpr std::array HyphenOffsets{8U, 13U, 18U, 23U};
            if (value.size() != 36)
                return false;
            return std::ranges::all_of(HyphenOffsets, [value](const std::size_t offset) {
                return value[offset] == '-';
            });
        }

        /** @brief Decodes validated-shape UUID characters without reading across a separator. */
        [[nodiscard]] Result<std::array<std::uint8_t, 16>> DecodeUuidBytes(const std::string_view value) {
            std::array<std::uint8_t, 16> bytes{};
            std::size_t output{};
            std::optional<std::byte> high;
            for (const char character : value) {
                if (character == '-')
                    continue;
                const auto digit = ParseHexDigit(character);
                if (!digit.has_value())
                    return Result<std::array<std::uint8_t, 16>>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "Physics project IDs require lowercase hexadecimal UUID digits."));
                if (!high.has_value()) {
                    high = digit;
                    continue;
                }
                bytes[output++] = std::to_integer<std::uint8_t>((*high << 4U) | *digit);
                high.reset();
            }
            if (output != bytes.size() || high.has_value())
                return Result<std::array<std::uint8_t, 16>>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics project IDs contain an unexpected UUID separator."));
            return Result<std::array<std::uint8_t, 16>>::Success(bytes);
        }
    }  // namespace

    Result<std::array<std::uint8_t, 16>> ParsePhysicsProjectId(const std::string_view value) {
        if (!HasCanonicalUuidShape(value))
            return Result<std::array<std::uint8_t, 16>>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics project IDs require the canonical 36-character UUID form."));
        auto decoded = DecodeUuidBytes(value);
        if (decoded.HasError())
            return decoded;
        const auto bytes = std::move(decoded).Value();
        if (std::ranges::all_of(bytes, [](const std::uint8_t byte) {
            return byte == 0;
        }))
            return Result<std::array<std::uint8_t, 16>>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics project IDs must be non-zero."));
        return Result<std::array<std::uint8_t, 16>>::Success(bytes);
    }

    std::string FormatPhysicsProjectId(const std::array<std::uint8_t, 16> &bytes) {
        constexpr std::string_view Digits = "0123456789abcdef";
        std::string result;
        result.reserve(36);
        for (std::size_t index{}; index < bytes.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                result.push_back('-');
            const auto byte = static_cast<std::byte>(bytes[index]);
            result.push_back(Digits[std::to_integer<std::size_t>(byte >> 4U)]);
            result.push_back(Digits[std::to_integer<std::size_t>(byte & static_cast<std::byte>(0x0FU))]);
        }
        return result;
    }
}  // namespace Horo::Physics::Detail
