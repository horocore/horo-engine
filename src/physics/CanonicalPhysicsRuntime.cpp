#include "CanonicalPhysicsRuntime.h"

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
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Native callback targets are installed by the sole composition owner and cleared after all worlds retire. */
        struct AllocatorFunctions final {
            JPH::AllocateFunction allocate{};
            JPH::ReallocateFunction reallocate{};
            JPH::AlignedAllocateFunction alignedAllocate{};
        };

        /** @brief Callbacks retained only while the explicit canonical process owner is alive. */
        AllocatorFunctions installedAllocatorFunctions;

        /** @brief Fixed non-blocking mailbox written by native callbacks and drained at the owner-thread safe point. */
        struct DiagnosticInbox final {
            /** @brief Appends a bounded owner-thread summary for messages rejected by callback contention. */
            static void AppendDroppedSummary(std::string &message, const std::uint32_t droppedCount) {
                if (droppedCount == 0)
                    return;
                std::array<char, 16> countText{};
                const auto [countEnd, error] = std::to_chars(countText.data(), countText.data() + countText.size(), droppedCount);
                if (error != std::errc{})
                    return;
                std::string suffix{" ["};
                suffix.append(countText.data(), static_cast<std::size_t>(countEnd - countText.data()));
                suffix.append(" additional messages dropped]");
                if (message.size() + suffix.size() > MaximumPhysicsDiagnosticMessageBytes)
                    message.resize(MaximumPhysicsDiagnosticMessageBytes - suffix.size());
                message.append(suffix);
            }

            /** @brief Maps one private normalized classification to a stable Horo error. */
            static Error MakeDiagnosticError(const CanonicalDiagnosticKind kind, std::string message) {
                using enum CanonicalDiagnosticKind;
                switch (kind) {
                    case Validation:
                        return MakeError(PhysicsErrors::SolverValidationMessage, std::move(message));
                    case Assertion:
                        return MakeError(PhysicsErrors::SolverAssertionFailed, std::move(message));
                    case Fatal:
                        return MakeError(PhysicsErrors::SolverFatalCondition, std::move(message));
                }
                return MakeError(PhysicsErrors::SolverFatalCondition, "Unknown canonical solver diagnostic classification.");
            }

            void RetainEmergency(const CanonicalDiagnosticKind kind) noexcept {
                const auto desired = static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) + std::uint8_t{1});
                std::uint8_t current = emergencyKind.load();
                while (current < desired && !emergencyKind.compare_exchange_weak(current, desired)) {
                    // A producer changed the priority; retry only while this condition is more severe.
                }
            }

            void Submit(const CanonicalDiagnosticKind kind, const std::string_view message) noexcept {
                if (lock.test_and_set()) {
                    dropped.fetch_add(1);
                    if (kind != CanonicalDiagnosticKind::Validation)
                        RetainEmergency(kind);
                    return;
                }
                if (!occupied || kind > retainedKind) {
                    retainedKind = kind;
                    const std::string_view evidence =
                        message.empty() ? std::string_view{"Native solver emitted an empty diagnostic message."} : message;
                    const std::size_t count = std::min(evidence.size(), text.size() - 1);
                    std::copy_n(evidence.data(), count, text.data());
                    text[count] = '\0';
                    size = count;
                    occupied = true;
                }
                lock.clear();
            }

            [[nodiscard]] std::optional<Error> Drain() {
                if (lock.test_and_set())
                    return MakeError(PhysicsErrors::SolverFatalCondition,
                                     "A native solver callback did not quiesce before the owner-thread drain boundary.");
                const std::uint8_t emergency = emergencyKind.exchange(0);
                if (!occupied && emergency == 0) {
                    lock.clear();
                    return std::nullopt;
                }
                CanonicalDiagnosticKind kind = occupied ? retainedKind : static_cast<CanonicalDiagnosticKind>(emergency - 1);
                std::string message = occupied ? std::string{text.data(), size}
                                               : std::string{"Native solver fatal evidence was bounded by callback contention."};
                if (occupied && emergency > static_cast<std::uint8_t>(kind) + 1) {
                    kind = static_cast<CanonicalDiagnosticKind>(emergency - 1);
                    message = "Native solver fatal evidence was bounded by callback contention.";
                }
                AppendDroppedSummary(message, dropped.exchange(0));
                occupied = false;
                size = 0;
                lock.clear();
                return MakeDiagnosticError(kind, std::move(message));
            }

            std::atomic_flag lock = ATOMIC_FLAG_INIT;
            std::atomic<std::uint32_t> dropped{};
            std::atomic<std::uint8_t> emergencyKind{};
            std::array<char, MaximumPhysicsDiagnosticMessageBytes + 1> text{};
            std::size_t size{};
            CanonicalDiagnosticKind retainedKind{CanonicalDiagnosticKind::Validation};
            bool occupied{};
        };

        /** @brief Returns process callback routing state owned by the one admitted canonical runtime. */
        std::atomic<DiagnosticInbox *> &ActiveDiagnosticInbox() noexcept {
            static std::atomic<DiagnosticInbox *> inbox;
            return inbox;
        }

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
            const std::string_view evidence = message != nullptr && message[0] != '\0' ? std::string_view{message}
                                              : expression == nullptr                  ? std::string_view{"Native solver assertion"}
                                                                                       : std::string_view{expression};
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

        /** @brief Process-owned Jolt registration; destruction occurs only after every native world retires. */
        struct CanonicalRuntime final {
            CanonicalRuntime() = default;
            CanonicalRuntime(const CanonicalRuntime &) = delete;
            CanonicalRuntime &operator=(const CanonicalRuntime &) = delete;

            ~CanonicalRuntime() {
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

            bool typesRegistered{};
            JPH::TraceFunction priorTrace{};
#ifdef JPH_ENABLE_ASSERTS
            JPH::AssertFailedFunction priorAssertFailed{};
#endif
            CanonicalResourceCounts resources;
            std::unique_ptr<JPH::Factory> factory;
        };

        /** @brief Temporary closed filter until the collision-profile ticket installs a validated table. */
        class ClosedBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
        public:
            [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
                return 1;
            }

            [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {
                return JPH::BroadPhaseLayer{0};
            }
        };

        /** @brief Rejects all pairs while body/filter capability remains unpublished. */
        class ClosedObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
        public:
            [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override {
                return false;
            }
        };

        /** @brief Rejects all object pairs while body/filter capability remains unpublished. */
        class ClosedObjectPairs final : public JPH::ObjectLayerPairFilter {
        public:
            [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override {
                return false;
            }
        };

        /** @brief Per-world native ownership in dependency order; reverse member destruction releases the system first. */
        struct CanonicalWorld final {
            explicit CanonicalWorld(CanonicalRuntime &runtime) : owner(runtime) {
                ++owner.resources.worlds;
            }

            CanonicalWorld(const CanonicalWorld &) = delete;
            CanonicalWorld &operator=(const CanonicalWorld &) = delete;

            ~CanonicalWorld() {
                const auto hadSystem = static_cast<std::uint32_t>(system != nullptr);
                const auto hadJobs = static_cast<std::uint32_t>(jobs != nullptr);
                const auto hadScratch = static_cast<std::uint32_t>(scratch != nullptr);
                system.reset();
                jobs.reset();
                scratch.reset();
                owner.resources.physicsSystems -= hadSystem;
                owner.resources.jobSystems -= hadJobs;
                owner.resources.scratchAllocators -= hadScratch;
                --owner.resources.worlds;
            }

            CanonicalRuntime &owner;
            ClosedBroadPhaseLayers broadPhaseLayers;
            ClosedObjectVsBroadPhase objectVsBroadPhase;
            ClosedObjectPairs objectPairs;
            std::unique_ptr<JPH::TempAllocatorImpl> scratch;
            std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
            std::unique_ptr<JPH::PhysicsSystem> system;
            DiagnosticInbox diagnostics;
        };

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
        auto world = std::make_unique<CanonicalWorld>(*static_cast<CanonicalRuntime *>(runtime.value));
        world->scratch = std::make_unique<JPH::TempAllocatorImpl>(static_cast<std::size_t>(values.scratchBytes));
        ++world->owner.resources.scratchAllocators;
        if (failurePoint == CanonicalFailurePoint::ScratchCreated)
            return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Scratch creation stage."));
        world->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        ++world->owner.resources.jobSystems;
        if (failurePoint == CanonicalFailurePoint::JobsCreated)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::InitializationFailed, "Serial job-dispatch creation stage."));
        world->system = std::make_unique<JPH::PhysicsSystem>();
        ++world->owner.resources.physicsSystems;
        world->system->Init(values.maximumBodies, 1, values.maximumBodyPairs, values.maximumContactConstraints, world->broadPhaseLayers,
                            world->objectVsBroadPhase, world->objectPairs);
        world->system->SetPhysicsSettings(values.solver);
        world->system->SetGravity(values.gravity);
        if (failurePoint == CanonicalFailurePoint::SystemInitialized)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::InitializationFailed, "Native world initialization stage."));
        return Result<CanonicalWorldHandle>::Success({world.release()});
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
            const auto updateError = canonical.system->Update(fixedDeltaSeconds, 1, canonical.scratch.get(), canonical.jobs.get());
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
