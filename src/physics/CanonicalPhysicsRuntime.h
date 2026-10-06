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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Physics::Detail {
    class PhysicsEventProjection;
    struct CanonicalSceneBodyBatchState;

    /** @brief Move-only unpublished native batch; destruction aborts on the Physics owner lane. */
    class CanonicalSceneBodyBatch final {
    public:
        explicit CanonicalSceneBodyBatch(std::shared_ptr<CanonicalSceneBodyBatchState> state) noexcept;
        ~CanonicalSceneBodyBatch();
        CanonicalSceneBodyBatch(CanonicalSceneBodyBatch &&) noexcept;
        CanonicalSceneBodyBatch &operator=(CanonicalSceneBodyBatch &&) noexcept;
        CanonicalSceneBodyBatch(const CanonicalSceneBodyBatch &) = delete;
        CanonicalSceneBodyBatch &operator=(const CanonicalSceneBodyBatch &) = delete;
        /** @brief Returns reserved Horo identities in descriptor order, not resident bodies. */
        [[nodiscard]] std::span<const BodyHandle> Handles() const noexcept;
        [[nodiscard]] std::span<const ShapeHandle> Shapes() const noexcept;
        [[nodiscard]] std::span<const ConstraintHandle> Constraints() const noexcept;
        [[nodiscard]] std::span<const ConstraintHandle> RetiredConstraints() const noexcept;
        [[nodiscard]] Result<void> PrepareRetirement(std::span<const BodyHandle> bodies, std::span<const ShapeHandle> shapes,
                                                     std::span<const ConstraintHandle> constraints) const;
        [[nodiscard]] Result<void> PrepareConstraints(std::span<const PhysicsConstraintDescriptor> descriptors) const;
        /** @brief Tests retained pending ownership without dereferencing a destroyed world. */
        [[nodiscard]] bool IsPending() const noexcept;
        /** @brief Rechecks native lifetime and exact pending ownership before aggregate publication. */
        [[nodiscard]] Result<void> ValidatePublication() const;
        /** @brief Publishes a validated fully prepared batch without further allocation. */
        void Publish() const noexcept;

    private:
        std::shared_ptr<CanonicalSceneBodyBatchState> state_;
    };

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

    /** @brief Bounded Horo-only values projected from resident scene and query-fixture registries. */
    struct CanonicalDebugProjection final {
        std::vector<PhysicsDebugRecord> bodies;
        std::vector<PhysicsDebugRecord> shapes;
        std::vector<PhysicsDebugRecord> constraints;
        std::uint64_t truncatedBodies{};
        std::uint64_t truncatedShapes{};
        std::uint64_t truncatedConstraints{};
    };

    /** @brief Copies stable Horo identities from the current owner-thread canonical world after one completed tick. */
    [[nodiscard]] CanonicalDebugProjection ProjectCanonicalDebug(CanonicalWorldHandle world, const PhysicsDebugBudget &budget);

    /** @brief Exact resident body and optional authored object identified by the joined native state scan. */
    struct CanonicalNonFiniteBody final {
        BodyHandle body;
        std::uint64_t sceneEntity{};
        bool retirable{true}; /**< False when the native body itself vanished; quarantine cannot safely remove it. */
    };

    /** @brief Borrowed owner-thread notification for retiring authored binding tables at the same safe point. */
    struct CanonicalRetirementSink final {
        PhysicsWorld::QuarantineSink *target{};

        /** @brief Removes the retired body and its collider bindings from the borrowed aggregate. */
        void Retire(const BodyHandle body) const noexcept {
            if (target)
                target->Retire(body);
        }

        /** @brief Removes one retired constraint binding from the borrowed aggregate. */
        void Retire(const ConstraintHandle constraint) const noexcept {
            if (target)
                target->Retire(constraint);
        }
    };

    /** @brief Starts private Jolt process registration or reports omitted/incompatible composition. */
    [[nodiscard]] Result<CanonicalRuntimeHandle> CreateCanonicalRuntime(CanonicalFailurePoint failurePoint = CanonicalFailurePoint::None);
    /** @brief Releases types, factory and allocator hooks after every native world has retired. */
    void DestroyCanonicalRuntime(CanonicalRuntimeHandle runtime) noexcept;
    /** @brief Builds one isolated native world from an already validated snapshot. */
    [[nodiscard]] Result<CanonicalWorldHandle> CreateCanonicalWorld(CanonicalRuntimeHandle runtime, const PhysicsWorldSettings &settings,
                                                                    CanonicalFailurePoint failurePoint = CanonicalFailurePoint::None,
                                                                    const PhysicsWorldSimulationBinding &simulation = {});
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

    /** @brief Synthetic contact conditions retained only for native boundary regression coverage. */
    struct CanonicalContactTestOptions final {
        bool sensor{};
        bool persisted{};
        std::uint32_t contactPointCount{1};
    };

    /** @brief Invokes the installed contact listener with copied native evidence for boundary regression coverage. */
    [[nodiscard]] bool InvokeCanonicalContactCallbackForTesting(CanonicalWorldHandle world, const PhysicsQueryFixture &first,
                                                                const PhysicsQueryFixture &second, std::uint64_t simulationTick,
                                                                CanonicalContactSink contactSink, CanonicalContactTestOptions options);
    /** @brief Admits one analytic scene shape into an unpublished owner-thread world. */
    [[nodiscard]] Result<ShapeHandle> CreateCanonicalSceneShape(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                const PhysicsShapeDescriptor &descriptor);
    /** @brief Admits one immutable compound scene shape from resident child shapes. */
    [[nodiscard]] Result<ShapeHandle> CreateCanonicalSceneCompoundShape(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                        std::span<const PhysicsSceneShapeInstance> instances);
    /** @brief Admits one scene body after its shape has been staged. */
    [[nodiscard]] Result<BodyHandle> CreateCanonicalSceneBody(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                              const PhysicsSceneBodyDescriptor &descriptor);
    /** @brief Allocates detached native bodies and prepares broadphase insertion without exposing new solver state.
     * @param world Borrowed native owner, which cancels the batch before world teardown.
     * @param owner Exact public world identity.
     * @param descriptors Complete nonempty body group, at most 256, referring to already owned shapes.
     * @return Unpublished RAII batch or typed failure with old resident state unchanged.
     */
    [[nodiscard]] Result<CanonicalSceneBodyBatch> PrepareCanonicalSceneBodies(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                              std::span<const PhysicsSceneBodyDescriptor> descriptors,
                                                                              std::span<const PhysicsSceneGroupShape> shapes = {},
                                                                              std::span<const PhysicsSceneGroupBody> groupBodies = {});
    /** @brief Reports a live unpublished body batch; stepping/direct body admission must wait for its owner transaction. */
    [[nodiscard]] bool HasPendingCanonicalSceneBodies(CanonicalWorldHandle world) noexcept;
    /** @brief Aborts unpublished native bodies before native-world destruction; retained wrappers become stale. */
    void CancelPendingCanonicalSceneBodies(CanonicalWorldHandle world) noexcept;
    /** @brief Validates one resident body replacement before queue admission or any tick mutation. */
    [[nodiscard]] Result<PhysicsBodyDescriptor> ResolveCanonicalBodyMutation(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                             const PhysicsBodyMutation &mutation);
    /** @brief Applies one prevalidated owner-thread replacement and reconciles retained policy. */
    [[nodiscard]] Result<void> ApplyCanonicalBodyMutation(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                          const PhysicsBodyMutation &mutation);
    /** @brief Copies the last applied policy for one exact resident scene body. */
    [[nodiscard]] Result<PhysicsBodyDescriptor> ReadCanonicalSceneBodyPolicy(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                             BodyHandle body);
    /** @brief Reads translated native state alongside retained body policy on the owner thread. */
    [[nodiscard]] Result<PhysicsBodyReconciliation> ReadCanonicalSceneBodyReconciliation(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                                         BodyHandle body);
    /** @brief Copies bounded scene-body activity counts on the owner thread; native islands remain private and unsupported. */
    [[nodiscard]] Result<PhysicsActivationObservation> ReadCanonicalSceneActivation(CanonicalWorldHandle world, PhysicsWorldId owner);
    /** @brief Reads optional authored identity for structured input-boundary diagnostics. */
    [[nodiscard]] std::uint64_t CanonicalSceneEntity(CanonicalWorldHandle world, BodyHandle body) noexcept;
    /** @brief Binds inert authored identity to a successfully admitted resident body. */
    void SetCanonicalSceneEntity(CanonicalWorldHandle world, BodyHandle body, std::uint64_t sceneEntity) noexcept;
    /** @brief Scans resident native pose, velocity and bounds after a joined step and before publication. */
    [[nodiscard]] std::optional<CanonicalNonFiniteBody> FindCanonicalNonFiniteBody(CanonicalWorldHandle world, bool postStep,
                                                                                   std::size_t &cursor) noexcept;
    /** @brief Avoids suppressing an unrelated query fixture that shares a separately issued body slot. */
    [[nodiscard]] bool CanonicalQueryFixtureUsesBodyHandle(CanonicalWorldHandle world, BodyHandle body) noexcept;
    /** @brief Validates a complete CCD world replacement before any native mutation. */
    [[nodiscard]] Result<void> ValidateCanonicalContinuousCollision(CanonicalWorldHandle world,
                                                                    const PhysicsContinuousCollisionPolicy &policy);
    /** @brief Applies admitted policy at the joined owner pre-step safe point and wakes/reconciles affected bodies. */
    [[nodiscard]] Result<void> ApplyCanonicalContinuousCollision(CanonicalWorldHandle world,
                                                                 const PhysicsContinuousCollisionPolicy &policy);
    /** @brief Copies effective world policy and its lifetime-scoped revision without native references. */
    [[nodiscard]] Result<PhysicsContinuousCollisionObservation> ReadCanonicalContinuousCollision(CanonicalWorldHandle world);

    /** @brief Removes a corrupt resident body and every attached native constraint at the owner-thread post-step safe point. */
    void QuarantineCanonicalSceneBody(CanonicalWorldHandle world, BodyHandle body, const CanonicalRetirementSink &sink) noexcept;
    /** @brief Marks one resident body as corrupt for deterministic containment tests without feeding NaN to Jolt. */
    [[nodiscard]] bool InjectCanonicalNonFiniteBodyForTesting(CanonicalWorldHandle world, BodyHandle body, float value,
                                                              std::uint8_t component, bool postStep) noexcept;
    /** @brief Admits one scene constraint after both body endpoints have been staged. */
    [[nodiscard]] Result<ConstraintHandle> CreateCanonicalSceneConstraint(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                          const PhysicsConstraintDescriptor &descriptor);
    /** @brief Removes one exact resident native joint at the owner-thread structural safe point. */
    [[nodiscard]] Result<void> DestroyCanonicalSceneConstraint(CanonicalWorldHandle world, ConstraintHandle constraint);
    /** @brief Reads a hinge/slider coordinate from an exact resident native joint on the owner thread. */
    [[nodiscard]] Result<PhysicsJointState> ReadCanonicalSceneJointState(CanonicalWorldHandle world, ConstraintHandle constraint);
    /** @brief Exercises the same bounded callback inbox from native-boundary tests. */
    void SubmitCanonicalDiagnosticForTesting(CanonicalWorldHandle world, CanonicalDiagnosticKind kind, std::string_view message) noexcept;
    /** @brief Invokes the installed native callback hook under a bounded test route. */
    void InvokeCanonicalDiagnosticCallbackForTesting(CanonicalWorldHandle world, CanonicalDiagnosticKind kind, std::string_view message);
    /** @brief Copies current native resource ownership counts without traversing solver data. */
    [[nodiscard]] CanonicalResourceCounts InspectCanonicalResources(CanonicalRuntimeHandle runtime) noexcept;
}  // namespace Horo::Physics::Detail
