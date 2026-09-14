#pragma once

/** @file CanonicalPhysicsRuntime.h
 * @brief Native-free declarations for private canonical process and world ownership.
 */

#include "Horo/Physics/PhysicsWorldSettings.h"

#include <optional>
#include <string_view>

namespace Horo::Physics::Detail {
    /** @brief Private deterministic rollback probe; production entry points use None. */
    enum class CanonicalFailurePoint : std::uint8_t {
        None,
        AllocatorRegistered,
        FactoryCreated,
        TypesRegistered,
        ScratchCreated,
        JobsCreated,
        SystemInitialized
    };

    /** @brief Owner-thread resource accounting used to verify reverse-order partial rollback. */
    struct CanonicalResourceCounts final {
        std::uint32_t worlds{};
        std::uint32_t scratchAllocators{};
        std::uint32_t jobSystems{};
        std::uint32_t physicsSystems{};
        bool operator==(const CanonicalResourceCounts &) const noexcept = default;
    };

    struct CanonicalRuntimeHandle final {
        void *value{};
    };

    struct CanonicalWorldHandle final {
        void *value{};
    };

    /** @brief Native-free classification used only across the private canonical adapter seam. */
    enum class CanonicalDiagnosticKind : std::uint8_t {
        Validation,
        Assertion,
        Fatal,
    };

    /** @brief Owner-thread result of draining bounded callback evidence after one native step. */
    struct CanonicalStepOutcome final {
        std::optional<Error> diagnostic;
    };

    /** @brief Starts private Jolt process registration or reports omitted/incompatible composition. */
    [[nodiscard]] Result<CanonicalRuntimeHandle> CreateCanonicalRuntime(CanonicalFailurePoint failurePoint = CanonicalFailurePoint::None);
    /** @brief Releases types, factory and allocator hooks after every native world has retired. */
    void DestroyCanonicalRuntime(CanonicalRuntimeHandle runtime) noexcept;
    /** @brief Builds one isolated native world from an already validated snapshot. */
    [[nodiscard]] Result<CanonicalWorldHandle> CreateCanonicalWorld(CanonicalRuntimeHandle runtime, const PhysicsWorldSettings &settings,
                                                                    CanonicalFailurePoint failurePoint = CanonicalFailurePoint::None);
    /** @brief Releases one native world in reverse dependency order. */
    void DestroyCanonicalWorld(CanonicalWorldHandle world) noexcept;
    /** @brief Runs and joins one serial native fixed step before returning to publication code. */
    [[nodiscard]] Result<CanonicalStepOutcome> StepCanonicalWorld(CanonicalWorldHandle world, float fixedDeltaSeconds);
    /** @brief Exercises the same bounded callback inbox from native-boundary tests. */
    void SubmitCanonicalDiagnosticForTesting(CanonicalWorldHandle world, CanonicalDiagnosticKind kind, std::string_view message) noexcept;
    /** @brief Invokes the installed native callback hook under a bounded test route. */
    void InvokeCanonicalDiagnosticCallbackForTesting(CanonicalWorldHandle world, CanonicalDiagnosticKind kind, std::string_view message);
    /** @brief Copies current native resource ownership counts without traversing solver data. */
    [[nodiscard]] CanonicalResourceCounts InspectCanonicalResources(CanonicalRuntimeHandle runtime) noexcept;
}  // namespace Horo::Physics::Detail
