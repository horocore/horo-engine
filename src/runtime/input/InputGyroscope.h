#pragma once

/** @file InputGyroscope.h @brief Target-private gyro normalization and canonical profile projection. */
#include "Horo/Runtime/Input.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Input::Detail {
    /** @brief Reads one available finite SI sample without allocation or time integration.
     * @param sample Immutable facts from the explicitly selected source. @param policy Validated physical calibration.
     * @return Saturated unit rate, or zero for unavailable/malformed evidence.
     */
    [[nodiscard]] float ReadGyroscopeAxis(const SensorSample &sample, const GyroscopeBindingPolicy &policy) noexcept;
    /** @brief Decodes gyro-only fields without allowing legacy documents or other controls to hide motion configuration.
     * @param binding Canonical binding JSON. @param kind Already decoded control kind. @param schema Validated wire version.
     * @return Admitted calibration or a typed malformed-profile error.
     */
    [[nodiscard]] Result<GyroscopeBindingPolicy> DecodeGyroscopeBinding(const nlohmann::json &binding, BindingControlKind kind,
                                                                        std::uint32_t schema);
    /** @brief Encodes an already validated physical calibration. @param policy Admitted calibration. @return Owned JSON. */
    [[nodiscard]] nlohmann::json EncodeGyroscopeBinding(const GyroscopeBindingPolicy &policy);
}  // namespace Horo::Input::Detail
