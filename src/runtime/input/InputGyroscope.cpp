#include "InputGyroscope.h"

#include "InputErrors.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace Horo::Input {
    /** @copydoc GyroscopeBindingPolicy::IsValid */
    bool GyroscopeBindingPolicy::IsValid() const noexcept {
        return axis < SensorAxis::Count && std::isfinite(radiansPerSecondAtFullScale) && radiansPerSecondAtFullScale >= 0.001F &&
               radiansPerSecondAtFullScale <= 1000.0F;
    }

    /** @copydoc Detail::ReadGyroscopeAxis */
    float Detail::ReadGyroscopeAxis(const RawInputSnapshot &snapshot, const GyroscopeBindingPolicy &policy) noexcept {
        const auto &sample = snapshot.sensors[static_cast<std::size_t>(SensorKind::Gyroscope)];
        if (!policy.IsValid() || sample.availability != SensorAvailability::Available || sample.timestampNanoseconds <= 0 ||
            !std::ranges::all_of(sample.value, [](const float value) {
            return std::isfinite(value);
        }))
            return 0;
        const float rate = sample.value[static_cast<std::size_t>(policy.axis)];
        if (std::abs(rate) >= policy.radiansPerSecondAtFullScale)
            return std::copysign(1.0F, rate);
        return rate / policy.radiansPerSecondAtFullScale;
    }

    /** @copydoc Detail::DecodeGyroscopeBinding */
    Result<GyroscopeBindingPolicy> Detail::DecodeGyroscopeBinding(const nlohmann::json &binding, const BindingControlKind kind,
                                                                  const std::uint32_t schema) {
        using Output = Result<GyroscopeBindingPolicy>;
        const auto field = binding.find("gyroscope");
        if (kind != BindingControlKind::GyroscopeAxis && field == binding.end())
            return Output::Success({});
        const auto malformed = [] {
            return Output::Failure(MakeError(Errors::ProfileMalformed, "Gyroscope binding calibration is malformed or unsupported."));
        };
        if (kind != BindingControlKind::GyroscopeAxis || schema != CurrentInputBindingProfileSchema || field == binding.end() ||
            !field->is_object() || field->size() != 2 || !field->contains("axis") || !field->contains("radiansPerSecondAtFullScale"))
            return malformed();
        const auto &axis = field->at("axis");
        const auto &calibration = field->at("radiansPerSecondAtFullScale");
        if (!axis.is_number_integer() || !calibration.is_number() || calibration < 0.001 || calibration > 1000.0)
            return malformed();
        // Check the JSON number before narrowing so large/negative values cannot wrap to a real axis.
        if (axis < 0 || axis >= static_cast<unsigned>(SensorAxis::Count))
            return malformed();
        const GyroscopeBindingPolicy policy{static_cast<SensorAxis>(axis.get<unsigned>()), calibration.get<float>()};
        return policy.IsValid() ? Output::Success(policy) : malformed();
    }

    /** @copydoc Detail::EncodeGyroscopeBinding */
    nlohmann::json Detail::EncodeGyroscopeBinding(const GyroscopeBindingPolicy &policy) {
        return {{"axis", static_cast<unsigned>(policy.axis)}, {"radiansPerSecondAtFullScale", policy.radiansPerSecondAtFullScale}};
    }
}  // namespace Horo::Input
