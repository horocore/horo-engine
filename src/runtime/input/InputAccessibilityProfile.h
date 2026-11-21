#pragma once

/** @file InputAccessibilityProfile.h @brief Target-private accessibility fields in the canonical input profile codec. */
#include "Horo/Runtime/Input.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Input::Detail {
    /** @brief Decodes bounded typed accessibility fields only from the supported wire schema.
     * @param binding One binding JSON object. @param sourceSchema Exact validated source version before canonical migration.
     * @return Neutral/default or validated policy; malformed, negative, floating, unknown or unsupported fields fail explicitly.
     */
    [[nodiscard]] Result<BindingAccessibilityPolicy> DecodeBindingAccessibility(const nlohmann::json &binding, std::uint32_t sourceSchema);
    /** @brief Encodes one already validated policy into the canonical version-two binding representation.
     * @param policy Domain-validated policy. @return Owned inert JSON; no file is written or router state changed.
     */
    [[nodiscard]] nlohmann::json EncodeBindingAccessibility(const BindingAccessibilityPolicy &policy);
}  // namespace Horo::Input::Detail
