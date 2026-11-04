#pragma once

#include "Horo/Physics/PhysicsWorld.h"

#include <memory>
#include <optional>

namespace Horo::Application::Internal {
    /** @brief Finite application-selected storage, prepared before any native resource creation. */
    [[nodiscard]] Physics::PhysicsStateRuntimeBudget ProductPhysicsStateBudget() noexcept;
    /** @brief Captures the deliberate finite Canonical Save world profile before any resources exist.
     * Caller-selected profiles remain independent; success does not prove capture fits its retained quota. */
    [[nodiscard]] Result<Physics::PhysicsWorldSettings> ProductPhysicsSaveWorldSettings();
    /** @brief Binds only the verified executing image; a typed unsupported platform remains ordinary-Physics-only.
     * @return Empty on bound/Null, an owned explicit platform-unavailable diagnostic, or original admission failure.
     * No optional error issues state capability or repairs a failed native admission. */
    [[nodiscard]] Result<std::optional<Error>> PrepareProductPhysicsState(Physics::PhysicsRuntime &runtime);
    /** @brief Collects SAME Runtime ownership only after the caller has settled scenes and workers.
     * @details Pending/error preserves the runtime and handle. Only actual Drained releases both. */
    [[nodiscard]] Result<bool> PollProductPhysicsRetirement(std::unique_ptr<Physics::PhysicsRuntime> &runtime,
                                                            std::optional<Physics::PhysicsRuntimeRetirement> &retirement,
                                                            std::uint32_t maximumRecords);
}  // namespace Horo::Application::Internal
