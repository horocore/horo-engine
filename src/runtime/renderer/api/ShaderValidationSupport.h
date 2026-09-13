#pragma once

#include "Horo/Runtime/Render/ShaderManifest.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace Horo::Render::ShaderValidationDetail {
    template <typename EnumT> [[nodiscard]] constexpr bool IsKnown(const EnumT value, const EnumT last) noexcept {
        return static_cast<std::underlying_type_t<EnumT>>(value) <= static_cast<std::underlying_type_t<EnumT>>(last);
    }

    [[nodiscard]] constexpr bool SameTargetRequirement(const ShaderTargetRequirement &left, const ShaderTargetRequirement &right) noexcept {
        return left.backend == right.backend && left.payloadFormat == right.payloadFormat &&
               left.descriptorVersion == right.descriptorVersion && left.interfaceSchemaVersion == right.interfaceSchemaVersion &&
               left.maximumBindings == right.maximumBindings && left.maximumInlineConstantBytes == right.maximumInlineConstantBytes &&
               left.supportsCompute == right.supportsCompute && left.supportsStorageResources == right.supportsStorageResources;
    }

    template <typename RangeT, typename IdT, typename ProjectionT>
    [[nodiscard]] const std::ranges::range_value_t<RangeT> *FindSorted(const RangeT &values, const IdT id,
                                                                       ProjectionT projection) noexcept {
        const auto found = std::ranges::lower_bound(values, id, {}, projection);
        return found != values.end() && std::invoke(projection, *found) == id ? std::to_address(found) : nullptr;
    }

    [[nodiscard]] inline bool IsValidIdentity(const std::string_view value, const std::size_t maximumBytes,
                                              const bool allowPlus = false) noexcept {
        if (value.empty() || value.size() > maximumBytes)
            return false;
        return std::ranges::all_of(value, [allowPlus](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            const bool alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
            const bool digit = byte >= '0' && byte <= '9';
            return alpha || digit || byte == '.' || byte == '_' || byte == '-' || byte == '/' || (allowPlus && byte == '+');
        });
    }
}  // namespace Horo::Render::ShaderValidationDetail
