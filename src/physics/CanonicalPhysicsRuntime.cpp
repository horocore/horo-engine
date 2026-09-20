#include "CanonicalPhysicsRuntime.h"

#include "CanonicalPhysicsRuntimeDiagnostics.h"
#include "CanonicalPhysicsRuntimeQuery.h"
#include "CanonicalSolver.h"
#include "CanonicalWorldSettings.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

namespace Horo::Physics::Detail {
    namespace {
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
                ResetAllocators();
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

        struct CanonicalWorld;
        class ContactCaptureRoute;

        /** @brief Copies Jolt contact callbacks into Horo evidence without invoking consumers or locking native bodies. */
        class CanonicalContactListener final : public JPH::ContactListener {
        public:
            explicit CanonicalContactListener(CanonicalWorld &world) noexcept : world_(world) {}

            void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                JPH::ContactSettings &settings) override;
            void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                    JPH::ContactSettings &settings) override;

        private:
            void Emit(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                      const JPH::ContactSettings &settings) const noexcept;

            CanonicalWorld &world_;
        };

        /** @brief Per-world native ownership in dependency order; reverse member destruction releases the system first. */
        struct CanonicalWorld final {
            static constexpr std::size_t InvalidFixtureIndex = std::numeric_limits<std::size_t>::max();

            CanonicalWorld(CanonicalRuntime &runtime, const std::uint32_t maximumBodies, const std::uint32_t maximumFixtures)
                : owner(runtime), contactListener(*this), maximumFixtures(maximumFixtures),
                  nativeFixtureIndices(maximumBodies, InvalidFixtureIndex) {
                ++owner.resources.worlds;
                fixtures.reserve(maximumFixtures);
            }

            CanonicalWorld(const CanonicalWorld &) = delete;
            CanonicalWorld &operator=(const CanonicalWorld &) = delete;

            ~CanonicalWorld() {
                if (system != nullptr) {
                    auto &bodyInterface = system->GetBodyInterface();
                    for (const CanonicalQueryFixtureRecord &fixture : fixtures) {
                        bodyInterface.RemoveBody(fixture.nativeBody);
                        bodyInterface.DestroyBody(fixture.nativeBody);
                    }
                }
                fixtures.clear();
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
            CanonicalContactListener contactListener;
            DiagnosticInbox diagnostics;
            std::atomic<ContactCaptureRoute *> contactRoute{};
            std::uint32_t maximumFixtures{};
            std::vector<CanonicalQueryFixtureRecord> fixtures;
            std::vector<std::size_t> nativeFixtureIndices;
            std::uint32_t nextFixtureSlot{};
            std::uint32_t nextFixtureGeneration{1};
            std::uint64_t querySchemaGeneration{1};
            CanonicalQueryStorage queryStorage;
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

        /** @brief Limits native contact routing to the joined solver step and clears borrowed state on every exit path. */
        class ContactCaptureRoute final {
        public:
            ContactCaptureRoute(CanonicalWorld &world, const std::uint64_t simulationTick, const CanonicalContactSink sink) noexcept
                : world_(world), simulationTick_(simulationTick), sink_(sink) {
                world_.contactRoute.store(this, std::memory_order::seq_cst);
            }

            ContactCaptureRoute(const ContactCaptureRoute &) = delete;
            ContactCaptureRoute &operator=(const ContactCaptureRoute &) = delete;

            ~ContactCaptureRoute() {
                world_.contactRoute.store(nullptr, std::memory_order::seq_cst);
            }

            [[nodiscard]] const CanonicalContactSink &Sink() const noexcept {
                return sink_;
            }

            [[nodiscard]] std::uint64_t SimulationTick() const noexcept {
                return simulationTick_;
            }

        private:
            CanonicalWorld &world_;
            std::uint64_t simulationTick_{};
            CanonicalContactSink sink_;
        };

        /** @brief Rejects foreign native ownership rather than replacing global hooks or factories. */
        bool NativeGlobalsAreUnowned() noexcept {
            const std::array globalsUnowned{
                JPH::Factory::sInstance == nullptr, JPH::Allocate == nullptr,    JPH::Reallocate == nullptr, JPH::Free == nullptr,
                JPH::AlignedAllocate == nullptr,    JPH::AlignedFree == nullptr,
            };
            return std::ranges::all_of(globalsUnowned, std::identity{});
        }

        [[nodiscard]] JPH::Vec3 ToNative(const Math::Vec3 value) noexcept {
            return {value.x, value.y, value.z};
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const BodyHandle body) {
            const auto found = std::ranges::find_if(world.fixtures, [body](const auto &fixture) {
                return fixture.fixture.body == body;
            });
            return found == world.fixtures.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const JPH::BodyID body) {
            const std::size_t nativeIndex = body.GetIndex();
            if (nativeIndex >= world.nativeFixtureIndices.size())
                return nullptr;
            const std::size_t fixtureIndex = world.nativeFixtureIndices[nativeIndex];
            if (fixtureIndex == CanonicalWorld::InvalidFixtureIndex || fixtureIndex >= world.fixtures.size() ||
                world.fixtures[fixtureIndex].nativeBody != body)
                return nullptr;
            return &world.fixtures[fixtureIndex];
        }

        /** @brief Converts a native double/float position to the backend-neutral scene vector. */
        [[nodiscard]] Math::Vec3 ToScene(const JPH::RVec3 &value) noexcept {
            return {value.GetX(), value.GetY(), value.GetZ()};
        }

        /** @brief Copies one fixture's stable identity and current filter generation into event evidence. */
        [[nodiscard]] PhysicsEventEndpoint ToEventEndpoint(const CanonicalWorld &world,
                                                           const CanonicalQueryFixtureRecord &fixture) noexcept {
            return {.body = fixture.fixture.body,
                    .shape = fixture.fixture.shape,
                    .subshape = fixture.descriptor.subshape,
                    .layer = fixture.descriptor.layer,
                    .profile = fixture.descriptor.profile,
                    .filterSchemaGeneration = world.querySchemaGeneration};
        }

        /** @brief Converts optional query material evidence without retaining the descriptor or native shape. */
        [[nodiscard]] std::optional<PhysicsEventMaterial> ToEventMaterial(const std::optional<PhysicsQueryMaterial> &material) noexcept {
            if (!material.has_value())
                return std::nullopt;
            return PhysicsEventMaterial{.asset = material->asset, .assetGeneration = material->assetGeneration, .slot = material->slot};
        }

        /** @brief Chooses a deterministic lexicographically smallest copied point from one manifold. */
        [[nodiscard]] Math::Vec3 ContactPoint(const JPH::ContactManifold &manifold) noexcept {
            Math::Vec3 selected = ToScene(manifold.mBaseOffset);
            bool selectedPoint = false;
            for (JPH::uint index = 0; index < manifold.mRelativeContactPointsOn1.size(); ++index) {
                const Math::Vec3 candidate = ToScene(manifold.GetWorldSpaceContactPointOn1(index));
                if (!selectedPoint || std::tie(candidate.x, candidate.y, candidate.z) < std::tie(selected.x, selected.y, selected.z)) {
                    selected = candidate;
                    selectedPoint = true;
                }
            }
            return selected;
        }

        /** @copydoc CanonicalContactListener::OnContactAdded */
        void CanonicalContactListener::OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                                      JPH::ContactSettings &settings) {
            Emit(body1, body2, manifold, settings);
        }

        /** @copydoc CanonicalContactListener::OnContactPersisted */
        void CanonicalContactListener::OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2,
                                                          const JPH::ContactManifold &manifold, JPH::ContactSettings &settings) {
            Emit(body1, body2, manifold, settings);
        }

        /** @brief Copies one complete callback manifold before Jolt releases its locked callback view. */
        void CanonicalContactListener::Emit(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                            const JPH::ContactSettings &settings) const noexcept {
            const ContactCaptureRoute *route = world_.contactRoute.load(std::memory_order::seq_cst);
            if (route == nullptr || route->Sink().append == nullptr || route->Sink().context == nullptr)
                return;
            const auto *fixture1 = FindFixture(world_, body1.GetID());
            const auto *fixture2 = FindFixture(world_, body2.GetID());
            if (fixture1 == nullptr || fixture2 == nullptr)
                return;
            const PhysicsContactObservation observation{.simulationTick = route->SimulationTick(),
                                                        .first = ToEventEndpoint(world_, *fixture1),
                                                        .second = ToEventEndpoint(world_, *fixture2),
                                                        .firstMaterial = ToEventMaterial(fixture1->descriptor.material),
                                                        .secondMaterial = ToEventMaterial(fixture2->descriptor.material),
                                                        .contact = {.position = ContactPoint(manifold),
                                                                    .normal = {manifold.mWorldSpaceNormal.GetX(),
                                                                               manifold.mWorldSpaceNormal.GetY(),
                                                                               manifold.mWorldSpaceNormal.GetZ()},
                                                                    .penetrationDepthMeters = manifold.mPenetrationDepth,
                                                                    .normalImpulseNewtonSeconds = 0.0F},
                                                        .sensor = settings.mIsSensor};
            static_cast<void>(route->Sink().append(route->Sink().context, observation));
        }

        [[nodiscard]] CanonicalQueryAccess MakeQueryAccess(CanonicalWorld &world) {
            return {.system = *world.system,
                    .fixtures = world.fixtures,
                    .nativeFixtureIndices = world.nativeFixtureIndices,
                    .maximumFixtures = world.maximumFixtures,
                    .nextFixtureSlot = world.nextFixtureSlot,
                    .nextFixtureGeneration = world.nextFixtureGeneration,
                    .querySchemaGeneration = world.querySchemaGeneration,
                    .storage = world.queryStorage};
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
                                                      settings.Values().budgets.maximumShapes);
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
        world->system->SetContactListener(&world->contactListener);
        if (failurePoint == CanonicalFailurePoint::SystemInitialized)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::InitializationFailed, "Native world initialization stage."));
        return Result<CanonicalWorldHandle>::Success({world.release()});
    }

    /** @copydoc CreateCanonicalQueryFixture */
    Result<PhysicsQueryFixture> CreateCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsQueryFixtureDescriptor &fixture) {
        if (world.value == nullptr)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto access = MakeQueryAccess(canonical);
        return CreateCanonicalQueryFixtureFromAccess(access, owner, fixture);
    }

    /** @copydoc DestroyCanonicalQueryFixture */
    Result<void> DestroyCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsQueryFixture &fixture) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto access = MakeQueryAccess(canonical);
        return DestroyCanonicalQueryFixtureFromAccess(access, fixture);
    }

    /** @copydoc ExecuteCanonicalQuery */
    Result<PhysicsQueryResult> ExecuteCanonicalQuery(const CanonicalWorldHandle world, const PhysicsQueryDescriptor &descriptor,
                                                     const std::span<PhysicsQueryHit> hits) {
        if (world.value == nullptr)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto access = MakeQueryAccess(canonical);
        return ExecuteCanonicalQueryFromAccess(access, descriptor, hits);
    }

    /** @copydoc DestroyCanonicalWorld */
    void DestroyCanonicalWorld(const CanonicalWorldHandle world) noexcept {
        const std::unique_ptr<CanonicalWorld> ownedWorld{static_cast<CanonicalWorld *>(world.value)};
    }

    /** @copydoc StepCanonicalWorld */
    Result<CanonicalStepOutcome> StepCanonicalWorld(const CanonicalWorldHandle world, const float fixedDeltaSeconds,
                                                    const std::uint64_t simulationTick, const CanonicalContactSink contactSink) {
        if (world.value == nullptr)
            return Result<CanonicalStepOutcome>::Failure(MakeError(PhysicsErrors::InvalidState));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (const DiagnosticRoute route{canonical.diagnostics}; !route.Admitted()) {
            return Result<CanonicalStepOutcome>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Another canonical solver callback generation is still active."));
        } else {
            const ContactCaptureRoute contactRoute{canonical, simulationTick, contactSink};
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

    /** @copydoc InvokeCanonicalContactCallbackForTesting */
    bool InvokeCanonicalContactCallbackForTesting(const CanonicalWorldHandle world, const PhysicsQueryFixture &first,
                                                  const PhysicsQueryFixture &second, const std::uint64_t simulationTick, const bool sensor,
                                                  const bool persisted, const CanonicalContactSink contactSink) {
        if (world.value == nullptr || simulationTick == 0)
            return false;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto *firstFixture = FindFixture(canonical, first.body);
        const auto *secondFixture = FindFixture(canonical, second.body);
        if (firstFixture == nullptr || secondFixture == nullptr)
            return false;

        const ContactCaptureRoute route{canonical, simulationTick, contactSink};
        JPH::BodyLockRead firstLock(canonical.system->GetBodyLockInterface(), firstFixture->nativeBody);
        JPH::BodyLockRead secondLock(canonical.system->GetBodyLockInterface(), secondFixture->nativeBody);
        if (!firstLock.Succeeded() || !secondLock.Succeeded())
            return false;

        JPH::ContactManifold manifold;
        manifold.mBaseOffset = JPH::RVec3::sZero();
        manifold.mWorldSpaceNormal = JPH::Vec3::sAxisY();
        manifold.mPenetrationDepth = 0.1F;
        manifold.mRelativeContactPointsOn1.push_back(JPH::Vec3::sZero());
        manifold.mRelativeContactPointsOn2.push_back(JPH::Vec3::sZero());
        JPH::ContactSettings settings{};
        settings.mIsSensor = sensor;
        if (persisted)
            canonical.contactListener.OnContactPersisted(firstLock.GetBody(), secondLock.GetBody(), manifold, settings);
        else
            canonical.contactListener.OnContactAdded(firstLock.GetBody(), secondLock.GetBody(), manifold, settings);
        return true;
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
