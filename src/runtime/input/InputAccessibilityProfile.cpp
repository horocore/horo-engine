#include "InputAccessibilityProfile.h"

#include "InputErrors.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace Horo::Input::Detail {
    namespace {
        /** @brief Preserves the previous default when an optional boolean is absent; coercion is forbidden. */
        bool ReadBoolean(const nlohmann::json &object, const char *key, bool &destination) {
            const auto field = object.find(key);
            if (field == object.end())
                return true;
            if (!field->is_boolean())
                return false;
            destination = field->get<bool>();
            return true;
        }

        /** @brief Reads exact non-negative integers without floating-point conversion, signed wrap or narrowing. */
        bool ReadDuration(const nlohmann::json &object, const char *key, std::uint64_t &destination) {
            const auto field = object.find(key);
            if (field == object.end())
                return true;
            if (field->is_number_unsigned()) {
                destination = field->get<std::uint64_t>();
                return true;
            }
            if (!field->is_number_integer())
                return false;
            const auto value = field->get<std::int64_t>();
            if (value < 0)
                return false;
            destination = static_cast<std::uint64_t>(value);
            return true;
        }
    }  // namespace

    /** @copydoc DecodeBindingAccessibility */
    Result<BindingAccessibilityPolicy> DecodeBindingAccessibility(const nlohmann::json &binding, const std::uint32_t sourceSchema) {
        using Output = Result<BindingAccessibilityPolicy>;
        const auto field = binding.find("accessibility");
        if (field == binding.end())
            return Output::Success({});
        if (sourceSchema != CurrentInputBindingProfileSchema || !field->is_object())
            return Output::Failure(
                MakeError(Errors::ProfileMalformed, "Accessibility binding fields require the canonical profile schema and an object."));
        constexpr std::array<std::string_view, 6> allowed{"toggle", "stickyModifiers",         "holdMicroseconds",
                                                          "repeat", "repeatDelayMicroseconds", "repeatIntervalMicroseconds"};
        for (const auto &item : field->items())
            if (std::ranges::find(allowed, item.key()) == allowed.end())
                return Output::Failure(MakeError(Errors::ProfileMalformed, "Input accessibility policy contains an unknown field."));
        BindingAccessibilityPolicy policy;
        const bool valid = ReadBoolean(*field, "toggle", policy.digital.toggle) &&
                           ReadBoolean(*field, "stickyModifiers", policy.stickyModifiers) &&
                           ReadBoolean(*field, "repeat", policy.digital.repeat) &&
                           ReadDuration(*field, "holdMicroseconds", policy.digital.holdMicroseconds) &&
                           ReadDuration(*field, "repeatDelayMicroseconds", policy.digital.repeatDelayMicroseconds) &&
                           ReadDuration(*field, "repeatIntervalMicroseconds", policy.digital.repeatIntervalMicroseconds);
        if (!valid || !policy.IsValid())
            return Output::Failure(
                MakeError(Errors::ProfileMalformed, "Input accessibility policy has invalid types or bounded durations."));
        return Output::Success(policy);
    }

    /** @copydoc EncodeBindingAccessibility */
    nlohmann::json EncodeBindingAccessibility(const BindingAccessibilityPolicy &policy) {
        return {{"toggle", policy.digital.toggle},
                {"stickyModifiers", policy.stickyModifiers},
                {"holdMicroseconds", policy.digital.holdMicroseconds},
                {"repeat", policy.digital.repeat},
                {"repeatDelayMicroseconds", policy.digital.repeatDelayMicroseconds},
                {"repeatIntervalMicroseconds", policy.digital.repeatIntervalMicroseconds}};
    }
}  // namespace Horo::Input::Detail
