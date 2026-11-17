#pragma once

#include "Horo/Prefab/PrefabDocument.h"
#include "PrefabValueJsonInternal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Prefab::Detail {
    [[nodiscard]] bool AddPayloadBytes(std::size_t &total, std::size_t bytes, std::size_t maximum) noexcept;
    [[nodiscard]] Result<Gameplay::BehaviorComponent> ParseBehavior(const Json &value, std::size_t &payloadBytes,
                                                                    std::size_t maximumPayloadBytes);
    [[nodiscard]] Result<std::optional<PrefabComposition>> ParseComposition(const Json &value, const PrefabProjectPolicy &policy);
    [[nodiscard]] Result<PrefabDocument> ParseDocumentImpl(const std::string_view source, const PrefabLimitProfile &limits,
                                                           const PrefabSourceParseLimits &parseLimits);
}  // namespace Horo::Prefab::Detail
