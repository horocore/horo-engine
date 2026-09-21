#pragma once

/** @file CanonicalPhysicsRuntime.h
 * @brief Native-free declarations for private canonical process and world ownership.
 */

#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsConstraintDescriptor.h"
#include "Horo/Physics/PhysicsEvents.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsShapeDescriptor.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Horo::Physics::Detail {
    class PhysicsEventProjection;

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

    /** @brief Non-owning callback seam for copied contact evidence produced during one native step. */
    struct CanonicalContactSink final {
        PhysicsEventProjection *context{}; /**< Owner-thread projection state, valid until the joined step returns. */
        bool (*append)(PhysicsEventProjection *context, const PhysicsContactObservation &observation) noexcept {};
        /**< Copies one complete Horo observation; the native adapter never retains the value. */
    };

    /** @brief Admits one owner-thread query fixture without exposing its native representation. */
    [[nodiscard]] Result<PhysicsQueryFixture> CreateCanonicalQueryFixture(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                          const PhysicsQueryFixtureDescriptor &fixture);
    /** @brief Retires one exact owner-thread query fixture and its native body/shape. */
    [[nodiscard]] Result<void> DestroyCanonicalQueryFixture(CanonicalWorldHandle world, const PhysicsQueryFixture &fixture);
    /** @brief Executes one immediate query and projects only stable Horo evidence. */
    [[nodiscard]] Result<PhysicsQueryResult> ExecuteCanonicalQuery(CanonicalWorldHandle world, const PhysicsQueryDescriptor &descriptor,
                                                                   std::span<PhysicsQueryHit> hits);

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
    /** @brief Runs and joins one serial native fixed step before returning to publication code.
     * @param world Prepared native world.
     * @param fixedDeltaSeconds Exact validated world fixed delta.
     * @param simulationTick Tick identity copied into every callback observation.
     * @param contactSink Non-owning projection seam active only until the joined step returns.
     */
    [[nodiscard]] Result<CanonicalStepOutcome> StepCanonicalWorld(CanonicalWorldHandle world, float fixedDeltaSeconds,
                                                                  std::uint64_t simulationTick = 0, CanonicalContactSink contactSink = {});
    /** @brief Invokes the installed contact listener with copied native evidence for boundary regression coverage. */
    [[nodiscard]] bool InvokeCanonicalContactCallbackForTesting(CanonicalWorldHandle world, const PhysicsQueryFixture &first,
                                                                const PhysicsQueryFixture &second, std::uint64_t simulationTick,
                                                                bool sensor, bool persisted, CanonicalContactSink contactSink);
    /** @brief Admits one analytic scene shape into an unpublished owner-thread world. */
    [[nodiscard]] Result<ShapeHandle> CreateCanonicalSceneShape(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                const PhysicsShapeDescriptor &descriptor);
    /** @brief Admits one immutable compound scene shape from resident child shapes. */
    [[nodiscard]] Result<ShapeHandle> CreateCanonicalSceneCompoundShape(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                        std::span<const PhysicsSceneShapeInstance> instances);
    /** @brief Admits one scene body after its shape has been staged. */
    [[nodiscard]] Result<BodyHandle> CreateCanonicalSceneBody(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                              const PhysicsSceneBodyDescriptor &descriptor);
    /** @brief Admits one scene constraint after both body endpoints have been staged. */
    [[nodiscard]] Result<ConstraintHandle> CreateCanonicalSceneConstraint(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                          const PhysicsConstraintDescriptor &descriptor);
    /** @brief Exercises the same bounded callback inbox from native-boundary tests. */
    void SubmitCanonicalDiagnosticForTesting(CanonicalWorldHandle world, CanonicalDiagnosticKind kind, std::string_view message) noexcept;
    /** @brief Invokes the installed native callback hook under a bounded test route. */
    void InvokeCanonicalDiagnosticCallbackForTesting(CanonicalWorldHandle world, CanonicalDiagnosticKind kind, std::string_view message);
    /** @brief Copies current native resource ownership counts without traversing solver data. */
    [[nodiscard]] CanonicalResourceCounts InspectCanonicalResources(CanonicalRuntimeHandle runtime) noexcept;
}  // namespace Horo::Physics::Detail
