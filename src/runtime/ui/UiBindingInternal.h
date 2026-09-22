#pragma once

#include "Horo/Runtime/Ui/UiBinding.h"

#include <algorithm>
#include <memory>

namespace Horo::Runtime::Ui::BindingInternal {
    [[nodiscard]] constexpr bool IsLowercaseAlphaNumeric(const unsigned char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
    }

    [[nodiscard]] constexpr bool IsIdentifierSeparator(const unsigned char value) noexcept {
        return value == '.' || value == '-' || value == '_';
    }

    [[nodiscard]] inline bool IsCanonicalNamespacedId(const std::string_view value, const std::size_t maximumBytes) noexcept {
        if (value.empty() || value.size() > maximumBytes)
            return false;

        bool previousSeparator = true;
        std::size_t segmentCount = 0;
        for (const unsigned char character : value) {
            if (character == '.') {
                if (previousSeparator)
                    return false;
                previousSeparator = true;
                ++segmentCount;
                continue;
            }
            if (IsIdentifierSeparator(character)) {
                if (previousSeparator)
                    return false;
                previousSeparator = true;
                continue;
            }
            if (!IsLowercaseAlphaNumeric(character))
                return false;
            previousSeparator = false;
        }
        if (previousSeparator)
            return false;
        ++segmentCount;
        return segmentCount >= 2;
    }

    [[nodiscard]] inline bool IsCanonicalPropertyId(const std::string_view value) noexcept {
        if (value.empty() || value.size() > MaximumUiBindingPropertyIdBytes)
            return false;
        return std::ranges::all_of(value, [](const unsigned char character) noexcept {
            return IsLowercaseAlphaNumeric(character) || character == '_';
        });
    }

    [[nodiscard]] inline const UiBindingPropertyDescriptor *FindProperty(const std::span<const UiBindingPropertyDescriptor> properties,
                                                                         const UiBindingPropertyId &id) noexcept {
        const auto found = std::ranges::lower_bound(properties, id, {}, &UiBindingPropertyDescriptor::id);
        return found != properties.end() && found->id == id ? std::to_address(found) : nullptr;
    }
}  // namespace Horo::Runtime::Ui::BindingInternal
