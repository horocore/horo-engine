#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalSolver.h"
#include "CanonicalWorldSettings.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

namespace Horo::Physics::Detail {
    AllocatorFunctions installedAllocatorFunctions;

    std::atomic<DiagnosticInbox *> &ActiveDiagnosticInbox() noexcept {
        static std::atomic<DiagnosticInbox *> inbox;
        return inbox;
    }

    CanonicalRuntime::~CanonicalRuntime() {
        ActiveDiagnosticInbox().store(nullptr);
        if (typesRegistered)
            JPH::UnregisterTypes();
        JPH::Factory::sInstance = nullptr;
        factory.reset();
        JPH::Allocate = nullptr;
        JPH::Reallocate = nullptr;
        JPH::Free = nullptr;
        JPH::AlignedAllocate = nullptr;
        JPH::AlignedFree = nullptr;
        installedAllocatorFunctions = {};
        JPH::Trace = priorTrace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = priorAssertFailed;
#endif
    }

    CanonicalWorld::CanonicalWorld(CanonicalRuntime &runtime, const std::uint32_t maximumBodies, const std::uint32_t maximumFixtures,
                                   const std::uint32_t maximumShapes, const std::uint32_t maximumConstraints)
        : owner(runtime), scene(maximumBodies, maximumShapes, maximumConstraints), query(maximumFixtures, maximumBodies) {
        ++owner.resources.worlds;
        query.fixtures.reserve(query.maximumFixtures);
        scene.shapes.reserve(scene.maximumShapes);
        scene.bodies.reserve(scene.maximumBodies);
        scene.constraints.reserve(scene.maximumConstraints);
    }

    CanonicalWorld::~CanonicalWorld() {
        if (native.system != nullptr) {
            for (const CanonicalSceneConstraintRecord &constraint : scene.constraints)
                native.system->RemoveConstraint(constraint.constraint.GetPtr());
            auto &bodyInterface = native.system->GetBodyInterface();
            for (const CanonicalSceneBodyRecord &body : scene.bodies) {
                bodyInterface.RemoveBody(body.nativeBody);
                bodyInterface.DestroyBody(body.nativeBody);
            }
            for (const CanonicalQueryFixtureRecord &fixture : query.fixtures) {
                bodyInterface.RemoveBody(fixture.nativeBody);
                bodyInterface.DestroyBody(fixture.nativeBody);
            }
        }
        scene.constraints.clear();
        scene.bodies.clear();
        scene.shapes.clear();
        query.fixtures.clear();
        const auto hadSystem = static_cast<std::uint32_t>(native.system != nullptr);
        const auto hadJobs = static_cast<std::uint32_t>(native.jobs != nullptr);
        const auto hadScratch = static_cast<std::uint32_t>(native.scratch != nullptr);
        native.system.reset();
        native.jobs.reset();
        native.scratch.reset();
        owner.resources.physicsSystems -= hadSystem;
        owner.resources.jobSystems -= hadJobs;
        owner.resources.scratchAllocators -= hadScratch;
        --owner.resources.worlds;
    }

    namespace {

        /** @brief Normalizes one Jolt trace call into a bounded validation record without retaining native memory. */
        void CaptureNativeTrace(const char *format, ...) noexcept {  // NOSONAR -- JPH::TraceFunction requires this C varargs ABI.
            DiagnosticInbox *inbox = ActiveDiagnosticInbox().load();
            if (inbox == nullptr || format == nullptr)
                return;
            thread_local std::array<char, MaximumPhysicsDiagnosticMessageBytes + 1> message{};
            message.fill('\0');
            std::va_list arguments;
            va_start(arguments, format);
            const int formatted = std::vsnprintf(message.data(), message.size(), format, arguments);  // NOSONAR -- native format ABI.
            va_end(arguments);
            if (formatted < 0)
                inbox->Submit(CanonicalDiagnosticKind::Validation, "Native solver validation message could not be formatted.");
            else
                inbox->Submit(CanonicalDiagnosticKind::Validation, message.data());
        }

#ifdef JPH_ENABLE_ASSERTS
        /** @brief Converts a Jolt assertion into fatal bounded evidence; owner lifecycle performs the transition. */
        bool CaptureNativeAssertion(const char *expression, const char *message, const char *, const JPH::uint) noexcept {
            DiagnosticInbox *inbox = ActiveDiagnosticInbox().load();
            if (inbox == nullptr)
                return false;
            std::string_view evidence{"Native solver assertion"};
            if (message != nullptr && message[0] != '\0')
                evidence = message;
            else if (expression != nullptr)
                evidence = expression;
            inbox->Submit(CanonicalDiagnosticKind::Assertion, evidence);
            return false;
        }
#endif

        /** @brief Native allocation cannot unwind through no-exception Jolt frames; fail closed instead of dereferencing null. */
        void *RequireNativeAllocation(void *memory) noexcept {
            if (memory == nullptr)
                std::abort();
            return memory;
        }

        /** @brief Preserves native allocation semantics with a defined process-fatal exhaustion path. */
        void *CheckedAllocate(const std::size_t size) {
            return RequireNativeAllocation(installedAllocatorFunctions.allocate(std::max(size, std::size_t{1})));
        }

        /** @brief Delegates reallocation without allowing null to escape into no-exception native code. */
        void *CheckedReallocate(void *memory, const std::size_t oldSize, const std::size_t newSize) {
            return RequireNativeAllocation(installedAllocatorFunctions.reallocate(memory, oldSize, std::max(newSize, std::size_t{1})));
        }

        /** @brief Retains platform alignment and fails closed on exhaustion. */
        void *CheckedAlignedAllocate(const std::size_t size, const std::size_t alignment) {
            return RequireNativeAllocation(installedAllocatorFunctions.alignedAllocate(std::max(size, std::size_t{1}), alignment));
        }

        /** @brief Captures platform allocation functions and installs bounded allocation-failure behavior explicitly. */
        void InstallAllocators() {
            JPH::RegisterDefaultAllocator();
            installedAllocatorFunctions = {JPH::Allocate, JPH::Reallocate, JPH::AlignedAllocate};
            JPH::Allocate = CheckedAllocate;
            JPH::Reallocate = CheckedReallocate;
            JPH::AlignedAllocate = CheckedAlignedAllocate;
        }

        /** @brief Restricts process-global callback routing to one joined native step. */
        class DiagnosticRoute final {
        public:
            explicit DiagnosticRoute(DiagnosticInbox &inbox) noexcept : inbox_(&inbox) {
                DiagnosticInbox *expected = nullptr;
                admitted_ = ActiveDiagnosticInbox().compare_exchange_strong(expected, inbox_);
            }

            DiagnosticRoute(const DiagnosticRoute &) = delete;
            DiagnosticRoute &operator=(const DiagnosticRoute &) = delete;
            DiagnosticRoute(DiagnosticRoute &&) = delete;
            DiagnosticRoute &operator=(DiagnosticRoute &&) = delete;

            ~DiagnosticRoute() {
                if (admitted_)
                    ActiveDiagnosticInbox().store(nullptr);
            }

            [[nodiscard]] bool Admitted() const noexcept {
                return admitted_;
            }

        private:
            DiagnosticInbox *inbox_{};
            bool admitted_{};
        };

        /** @brief Rejects foreign native ownership rather than replacing global hooks or factories. */
        bool NativeGlobalsAreUnowned() noexcept {
            const std::array globalsUnowned{
                JPH::Factory::sInstance == nullptr, JPH::Allocate == nullptr,    JPH::Reallocate == nullptr, JPH::Free == nullptr,
                JPH::AlignedAllocate == nullptr,    JPH::AlignedFree == nullptr,
            };
            return std::ranges::all_of(globalsUnowned, std::identity{});
        }

    }  // namespace

    /** @copydoc CreateCanonicalRuntime */
    Result<CanonicalRuntimeHandle> CreateCanonicalRuntime(const CanonicalFailurePoint failurePoint) {
        if (!IsCanonicalSolverBuildCompatible())
            return Result<CanonicalRuntimeHandle>::Failure(
                MakeError(PhysicsErrors::ProfileUnsupported, "Canonical solver ABI is incompatible."));
        if (!NativeGlobalsAreUnowned())
            return Result<CanonicalRuntimeHandle>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Jolt process globals are already owned by another runtime."));

        auto runtime = std::make_unique<CanonicalRuntime>();
        runtime->priorTrace = JPH::Trace;
        JPH::Trace = CaptureNativeTrace;
#ifdef JPH_ENABLE_ASSERTS
        runtime->priorAssertFailed = JPH::AssertFailed;
        JPH::AssertFailed = CaptureNativeAssertion;
#endif
        InstallAllocators();
        if (failurePoint == CanonicalFailurePoint::AllocatorRegistered)
            return Result<CanonicalRuntimeHandle>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Allocator registration stage."));
        runtime->factory = std::make_unique<JPH::Factory>();
        JPH::Factory::sInstance = runtime->factory.get();
        if (failurePoint == CanonicalFailurePoint::FactoryCreated)
            return Result<CanonicalRuntimeHandle>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Factory creation stage."));
        JPH::RegisterTypes();
        runtime->typesRegistered = true;
        if (failurePoint == CanonicalFailurePoint::TypesRegistered)
            return Result<CanonicalRuntimeHandle>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Type registration stage."));
        return Result<CanonicalRuntimeHandle>::Success({runtime.release()});
    }

    /** @copydoc DestroyCanonicalRuntime */
    void DestroyCanonicalRuntime(const CanonicalRuntimeHandle runtime) noexcept {
        const std::unique_ptr<CanonicalRuntime> ownedRuntime{static_cast<CanonicalRuntime *>(runtime.value)};
    }

    /** @copydoc CreateCanonicalWorld */
    Result<CanonicalWorldHandle> CreateCanonicalWorld(const CanonicalRuntimeHandle runtime, const PhysicsWorldSettings &settings,
                                                      const CanonicalFailurePoint failurePoint) {
        if (runtime.value == nullptr)
            return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (settings.Values().world.capacity.maximumBodies == 0)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Canonical broad-phase initialization requires non-zero body capacity."));
        if (settings.Values().nonFinitePolicy != PhysicsNonFinitePolicy::FailWorld)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Body quarantine requires a qualified safe-point retirement path."));
        const auto translated = TranslateCanonicalWorldSettings(settings);
        if (translated.HasError())
            return Result<CanonicalWorldHandle>::Failure(translated.ErrorValue());

        const auto &values = translated.Value();
        auto world = std::make_unique<CanonicalWorld>(*static_cast<CanonicalRuntime *>(runtime.value), values.maximumBodies,
                                                      settings.Values().budgets.maximumShapes, settings.Values().budgets.maximumShapes,
                                                      settings.Values().world.capacity.maximumConstraints);
        world->native.scratch = std::make_unique<JPH::TempAllocatorImpl>(static_cast<std::size_t>(values.scratchBytes));
        ++world->owner.resources.scratchAllocators;
        if (failurePoint == CanonicalFailurePoint::ScratchCreated)
            return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Scratch creation stage."));
        world->native.jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        ++world->owner.resources.jobSystems;
        if (failurePoint == CanonicalFailurePoint::JobsCreated)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::InitializationFailed, "Serial job-dispatch creation stage."));
        world->native.system = std::make_unique<JPH::PhysicsSystem>();
        ++world->owner.resources.physicsSystems;
        world->native.system->Init(values.maximumBodies, 1, values.maximumBodyPairs, values.maximumContactConstraints,
                                   world->broadPhaseLayers, world->objectVsBroadPhase, world->objectPairs);
        world->native.system->SetPhysicsSettings(values.solver);
        world->native.system->SetGravity(values.gravity);
        if (failurePoint == CanonicalFailurePoint::SystemInitialized)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::InitializationFailed, "Native world initialization stage."));
        return Result<CanonicalWorldHandle>::Success(CanonicalWorldHandle{world.release()});
    }

    /** @copydoc DestroyCanonicalWorld */
    void DestroyCanonicalWorld(const CanonicalWorldHandle world) noexcept {
        const std::unique_ptr<CanonicalWorld> ownedWorld{static_cast<CanonicalWorld *>(world.value)};
    }

    /** @copydoc StepCanonicalWorld */
    Result<CanonicalStepOutcome> StepCanonicalWorld(const CanonicalWorldHandle world, const float fixedDeltaSeconds) {
        if (world.value == nullptr)
            return Result<CanonicalStepOutcome>::Failure(MakeError(PhysicsErrors::InvalidState));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (const DiagnosticRoute route{canonical.diagnostics}; !route.Admitted()) {
            return Result<CanonicalStepOutcome>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Another canonical solver callback generation is still active."));
        } else {
            const auto updateError =
                canonical.native.system->Update(fixedDeltaSeconds, 1, canonical.native.scratch.get(), canonical.native.jobs.get());
            std::optional<Error> diagnostic = canonical.diagnostics.Drain();
            if (diagnostic.has_value() && (diagnostic->code.Value() == PhysicsErrors::SolverAssertionFailed.code.Value() ||
                                           diagnostic->code.Value() == PhysicsErrors::SolverFatalCondition.code.Value()))
                return Result<CanonicalStepOutcome>::Failure(std::move(*diagnostic));
            if (updateError != JPH::EPhysicsUpdateError::None)
                return Result<CanonicalStepOutcome>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "Canonical fixed tick exhausted required contact or pair storage."));
            return Result<CanonicalStepOutcome>::Success({.diagnostic = std::move(diagnostic)});
        }
    }

    /** @copydoc SubmitCanonicalDiagnosticForTesting */
    void SubmitCanonicalDiagnosticForTesting(const CanonicalWorldHandle world, const CanonicalDiagnosticKind kind,
                                             const std::string_view message) noexcept {
        if (world.value != nullptr)
            static_cast<CanonicalWorld *>(world.value)->diagnostics.Submit(kind, message);
    }

    /** @copydoc InvokeCanonicalDiagnosticCallbackForTesting */
    void InvokeCanonicalDiagnosticCallbackForTesting(const CanonicalWorldHandle world, const CanonicalDiagnosticKind kind,
                                                     const std::string_view message) {
        using enum CanonicalDiagnosticKind;
        if (world.value == nullptr)
            return;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (const DiagnosticRoute route{canonical.diagnostics}; !route.Admitted()) {
            return;
        } else {
            const std::string ownedMessage{message};
            if (kind == Validation) {
                JPH::Trace("%s", ownedMessage.c_str());
                return;
            }
#ifdef JPH_ENABLE_ASSERTS
            if (kind == Assertion) {
                JPH::AssertFailed("injected_solver_assertion", ownedMessage.c_str(), "", 0);
                return;
            }
#endif
            canonical.diagnostics.Submit(kind, message);
        }
    }

    /** @copydoc InspectCanonicalResources */
    CanonicalResourceCounts InspectCanonicalResources(const CanonicalRuntimeHandle runtime) noexcept {
        return runtime.value == nullptr ? CanonicalResourceCounts{} : static_cast<const CanonicalRuntime *>(runtime.value)->resources;
    }
}  // namespace Horo::Physics::Detail
