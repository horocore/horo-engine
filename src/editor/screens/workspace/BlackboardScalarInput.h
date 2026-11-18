#pragma once

/** @file BlackboardScalarInput.h @brief Converts bounded editor text to typed numeric Blackboard defaults. */

#include "Horo/AI/BlackboardSchema.h"

#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

namespace Horo::Editor {
    namespace BlackboardInputDetail {
        /** @brief Validates a complete locale-independent number without reducing precision. */
        template <class Scalar> [[nodiscard]] std::optional<AI::BlackboardScalarValue> Read(std::string_view text) {
            if (text.empty())
                return std::nullopt;
            Scalar value{};
            const auto *end = text.data() + text.size();
            const auto result = std::from_chars(text.data(), end, value);
            if (result.ec != std::errc{} || result.ptr != end)
                return std::nullopt;
            if constexpr (std::is_floating_point_v<Scalar>) {
                if (!std::isfinite(value))
                    return std::nullopt;
            }
            return AI::BlackboardScalarValue{value};
        }
    }  // namespace BlackboardInputDetail

    /** @brief Parses the complete text as the exact selected numeric type, or returns no candidate. */
    [[nodiscard]] inline std::optional<AI::BlackboardScalarValue> ReadBlackboardNumericDefault(AI::BlackboardValueKind kind,
                                                                                               std::string_view text) {
        using enum AI::BlackboardValueKind;
        if (kind == SignedInteger)
            return BlackboardInputDetail::Read<std::int64_t>(text);
        if (kind == Scalar)
            return BlackboardInputDetail::Read<double>(text);
        return std::nullopt;
    }

    /** @brief Presents an exact round-trippable primitive numeric default or empty text when unavailable. */
    [[nodiscard]] inline std::string BlackboardNumericDefaultText(const std::optional<AI::BlackboardValue> &value) {
        if (const auto *scalar = value ? std::get_if<AI::BlackboardScalarValue>(&*value) : nullptr) {
            if (const auto *integer = std::get_if<std::int64_t>(scalar))
                return std::format("{}", *integer);
            if (const auto *number = std::get_if<double>(scalar))
                return std::format("{}", *number);
        }
        return {};
    }
}  // namespace Horo::Editor
