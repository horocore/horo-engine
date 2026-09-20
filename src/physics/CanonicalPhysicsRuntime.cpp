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
#include <Jolt/Physics/Collision/ShapeCast.h>
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
#include <ranges>
#include <string>
#include <tuple>
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

        /** @brief Native fixture plus copied Horo identity/filter evidence for query projection. */
        struct CanonicalQueryFixtureRecord final {
            PhysicsQueryFixture fixture;
            PhysicsQueryFixtureDescriptor descriptor;
            JPH::BodyID nativeBody;
            JPH::Ref<JPH::Shape> shape;
        };

        /** @brief Fixed-capacity native collector; callbacks cannot allocate or mutate world structure. */
        template <typename Collector> class FixedQueryCollector final : public Collector {
        public:
            using ResultType = typename Collector::ResultType;

            FixedQueryCollector(ResultType *storage, const PhysicsQueryCollection collection) : values(storage), collection_(collection) {}

            void AddHit(const ResultType &hit) override {
                if (collection_ == PhysicsQueryCollection::Any) {
                    if (count == 0)
                        values[count++] = hit;
                    this->ForceEarlyOut();
                    return;
                }
                if (count < MaximumPhysicsQueryHits)
                    values[count++] = hit;
                else
                    overflow = true;
            }

            ResultType *values{};
            std::size_t count{};
            bool overflow{};

            PhysicsQueryCollection collection_;
        };

        /** @brief Per-world native ownership in dependency order; reverse member destruction releases the system first. */
        struct CanonicalWorld final {
            static constexpr std::size_t InvalidFixtureIndex = std::numeric_limits<std::size_t>::max();

            CanonicalWorld(CanonicalRuntime &runtime, const std::uint32_t maximumBodies, const std::uint32_t maximumFixtures)
                : owner(runtime), maximumFixtures(maximumFixtures), nativeFixtureIndices(maximumBodies, InvalidFixtureIndex) {
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
            DiagnosticInbox diagnostics;
            std::uint32_t maximumFixtures{};
            std::vector<CanonicalQueryFixtureRecord> fixtures;
            std::vector<std::size_t> nativeFixtureIndices;
            std::uint32_t nextFixtureSlot{};
            std::uint32_t nextFixtureGeneration{1};
            std::uint64_t querySchemaGeneration{1};
            std::array<JPH::CastRayCollector::ResultType, MaximumPhysicsQueryHits> rayQueryResults{};
            std::array<JPH::CollidePointCollector::ResultType, MaximumPhysicsQueryHits> pointQueryResults{};
            std::array<JPH::CollideShapeCollector::ResultType, MaximumPhysicsQueryHits> overlapQueryResults{};
            std::array<JPH::CastShapeCollector::ResultType, MaximumPhysicsQueryHits> sweepQueryResults{};
            std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> queryCandidates{};
        };

        /** @brief Reusable fixed-capacity collectors for one owner-thread query execution. */
        struct CanonicalQueryCollectors final {
            CanonicalQueryCollectors(CanonicalWorld &world, const PhysicsQueryCollection collection)
                : ray(world.rayQueryResults.data(), collection), point(world.pointQueryResults.data(), collection),
                  overlap(world.overlapQueryResults.data(), collection), sweep(world.sweepQueryResults.data(), collection) {}

            FixedQueryCollector<JPH::CastRayCollector> ray;
            FixedQueryCollector<JPH::CollidePointCollector> point;
            FixedQueryCollector<JPH::CollideShapeCollector> overlap;
            FixedQueryCollector<JPH::CastShapeCollector> sweep;
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

        [[nodiscard]] JPH::Vec3 ToNative(const Math::Vec3 value) noexcept {
            return {value.x, value.y, value.z};
        }

        [[nodiscard]] JPH::Quat ToNative(const Math::Quaternion value) noexcept {
            return {value.x, value.y, value.z, value.w};
        }

        [[nodiscard]] Result<JPH::Ref<JPH::Shape>> CreateNativeShape(const PhysicsShapeDescriptor &descriptor) {
            JPH::ShapeSettings::ShapeResult created;
            std::visit([&created]<typename Shape>(const Shape &shape) {
                using ShapeType = std::decay_t<Shape>;
                if constexpr (std::is_same_v<ShapeType, PhysicsBoxShape>)
                    created = JPH::BoxShapeSettings(ToNative(shape.halfExtentsMeters)).Create();
                else if constexpr (std::is_same_v<ShapeType, PhysicsSphereShape>)
                    created = JPH::SphereShapeSettings(shape.radiusMeters).Create();
                else if constexpr (std::is_same_v<ShapeType, PhysicsCapsuleShape>)
                    created = JPH::CapsuleShapeSettings(shape.cylindricalHalfHeightMeters, shape.radiusMeters).Create();
                else {
                    static_assert(std::is_same_v<ShapeType, PhysicsStaticPlaneShape>);
                    created = JPH::PlaneShapeSettings(JPH::Plane(ToNative(shape.normal), shape.signedDistanceMeters)).Create();
                }
            }, descriptor);
            if (created.HasError())
                return Result<JPH::Ref<JPH::Shape>>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the admitted analytic query shape."));
            return Result<JPH::Ref<JPH::Shape>>::Success(created.Get());
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const BodyHandle body) {
            const auto found = std::ranges::find_if(world.fixtures, [body](const auto &fixture) {
                return fixture.fixture.body == body;
            });
            return found == world.fixtures.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const ShapeHandle shape) {
            const auto found = std::ranges::find_if(world.fixtures, [shape](const auto &fixture) {
                return fixture.fixture.shape == shape;
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

        [[nodiscard]] bool Admits(const CanonicalQueryFixtureRecord &fixture, const PhysicsQueryDescriptor &descriptor) noexcept {
            if (fixture.descriptor.channel != descriptor.filter.channel ||
                fixture.descriptor.response == PhysicsQueryFixtureResponse::Ignore ||
                (fixture.descriptor.trigger && descriptor.filter.triggers == PhysicsQueryTriggerPolicy::Exclude))
                return false;
            if (descriptor.filter.requiredLayer.has_value() && fixture.descriptor.layer != *descriptor.filter.requiredLayer)
                return false;
            if (descriptor.filter.requiredProfile.has_value() && fixture.descriptor.profile != *descriptor.filter.requiredProfile)
                return false;
            if (descriptor.filter.excludedBody.has_value() && fixture.fixture.body == *descriptor.filter.excludedBody)
                return false;
            return true;
        }

        /** @brief Applies the stable Horo query filter before native collectors can short-circuit. */
        class QueryBodyFilter final : public JPH::BodyFilter {
        public:
            QueryBodyFilter(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor)
                : world_(world), descriptor_(descriptor) {}

            [[nodiscard]] bool ShouldCollide(const JPH::BodyID &body) const override {
                const auto *fixture = FindFixture(world_, body);
                return fixture != nullptr && Admits(*fixture, descriptor_);
            }

        private:
            const CanonicalWorld &world_;
            const PhysicsQueryDescriptor &descriptor_;
        };

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindSourceShape(const CanonicalWorld &world,
                                                                         const PhysicsQueryGeometry &geometry) {
            if (const auto *sweep = std::get_if<PhysicsSweepQuery>(&geometry))
                return FindFixture(world, sweep->shape);
            if (const auto *overlap = std::get_if<PhysicsOverlapQuery>(&geometry))
                return FindFixture(world, overlap->shape);
            return nullptr;
        }

        [[nodiscard]] PhysicsQueryResponse ToResponse(const PhysicsQueryFixtureResponse response) noexcept {
            return response == PhysicsQueryFixtureResponse::Overlap ? PhysicsQueryResponse::Overlap : PhysicsQueryResponse::Block;
        }

        [[nodiscard]] std::optional<Math::Vec3> RayNormal(const CanonicalWorld &world, const JPH::BodyID body,
                                                          const JPH::SubShapeID &subshape, const Math::Vec3 position) {
            JPH::BodyLockRead lock(world.system->GetBodyLockInterface(), body);
            if (!lock.Succeeded())
                return std::nullopt;
            const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(subshape, {position.x, position.y, position.z});
            const Math::Vec3 result{normal.GetX(), normal.GetY(), normal.GetZ()};
            return Math::IsFinite(result) ? std::optional<Math::Vec3>{result} : std::nullopt;
        }

        void CollectRayQuery(const CanonicalWorld &world, const PhysicsRayQuery &query, CanonicalQueryCollectors &collectors,
                             const QueryBodyFilter &bodyFilter) {
            world.system->GetNarrowPhaseQuery().CastRay({JPH::RVec3(query.origin.x, query.origin.y, query.origin.z),
                                                         ToNative(query.direction * query.maximumDistanceMeters)},
                                                        JPH::RayCastSettings{}, collectors.ray, {}, {}, bodyFilter);
        }

        void CollectPointQuery(const CanonicalWorld &world, const PhysicsPointQuery &query, CanonicalQueryCollectors &collectors,
                               const QueryBodyFilter &bodyFilter) {
            world.system->GetNarrowPhaseQuery().CollidePoint({query.point.x, query.point.y, query.point.z}, collectors.point, {}, {},
                                                             bodyFilter);
        }

        void CollectOverlapQuery(const CanonicalWorld &world, const PhysicsOverlapQuery &query, const CanonicalQueryFixtureRecord &source,
                                 CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const JPH::RMat44 transform =
                JPH::RMat44::sRotationTranslation(ToNative(query.pose.rotation),
                                                  JPH::RVec3(query.pose.translation.x, query.pose.translation.y, query.pose.translation.z));
            world.system->GetNarrowPhaseQuery().CollideShape(source.shape, JPH::Vec3::sOne(), transform, JPH::CollideShapeSettings{},
                                                             JPH::RVec3::sZero(), collectors.overlap, {}, {}, bodyFilter);
        }

        void CollectSweepQuery(const CanonicalWorld &world, const PhysicsSweepQuery &query, const CanonicalQueryFixtureRecord &source,
                               CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const JPH::RMat44 transform =
                JPH::RMat44::sRotationTranslation(ToNative(query.pose.rotation),
                                                  JPH::RVec3(query.pose.translation.x, query.pose.translation.y, query.pose.translation.z));
            const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(source.shape, JPH::Vec3::sOne(), transform,
                                                                              ToNative(query.direction * query.maximumDistanceMeters));
            world.system->GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{}, JPH::RVec3::sZero(), collectors.sweep, {}, {},
                                                          bodyFilter);
        }

        /** @brief Executes one validated native query into the world-owned fixed collectors. */
        [[nodiscard]] Result<void> CollectCanonicalQuery(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                         CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const auto *source = FindSourceShape(world, descriptor.geometry);
            if ((std::holds_alternative<PhysicsSweepQuery>(descriptor.geometry) ||
                 std::holds_alternative<PhysicsOverlapQuery>(descriptor.geometry)) &&
                source == nullptr)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));

            std::visit([&]<typename Query>(const Query &query) {
                if constexpr (std::is_same_v<Query, PhysicsRayQuery>) {
                    CollectRayQuery(world, query, collectors, bodyFilter);
                } else if constexpr (std::is_same_v<Query, PhysicsPointQuery>) {
                    CollectPointQuery(world, query, collectors, bodyFilter);
                } else if constexpr (std::is_same_v<Query, PhysicsOverlapQuery>) {
                    CollectOverlapQuery(world, query, *source, collectors, bodyFilter);
                } else {
                    static_assert(std::is_same_v<Query, PhysicsSweepQuery>);
                    CollectSweepQuery(world, query, *source, collectors, bodyFilter);
                }
            }, descriptor.geometry);
            if (collectors.ray.overflow || collectors.point.overflow || collectors.overlap.overflow || collectors.sweep.overflow)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "Canonical query exceeded the fixed native hit bound."));
            return Result<void>::Success();
        }

        [[nodiscard]] std::optional<Math::Vec3> ContactNormal(const JPH::Vec3 axis) {
            if (axis.LengthSq() <= 1.0e-12F)
                return std::nullopt;
            const JPH::Vec3 normalized = axis.Normalized();
            return Math::Vec3{-normalized.GetX(), -normalized.GetY(), -normalized.GetZ()};
        }

        struct CanonicalHitEvidence final {
            JPH::BodyID body;
            Math::Vec3 position;
            std::optional<Math::Vec3> normal;
            float distance{};
        };

        /** @brief Copies one native result into stable Horo query evidence. */
        [[nodiscard]] Result<void> AppendCanonicalHit(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                      std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                      std::size_t &candidateCount, const CanonicalHitEvidence &evidence) {
            const auto *fixture = FindFixture(world, evidence.body);
            if (fixture == nullptr || !Admits(*fixture, descriptor))
                return Result<void>::Success();
            if (candidateCount == candidates.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            PhysicsQueryHit hit{.body = fixture->fixture.body,
                                .shape = fixture->fixture.shape,
                                .subshape = fixture->descriptor.subshape,
                                .material = fixture->descriptor.material,
                                .layer = fixture->descriptor.layer,
                                .profile = fixture->descriptor.profile,
                                .channel = fixture->descriptor.channel,
                                .filterSchemaGeneration = world.querySchemaGeneration,
                                .response = ToResponse(fixture->descriptor.response),
                                .position = evidence.position,
                                .normal = evidence.normal,
                                .distanceMeters = evidence.distance};
            if (const Result<void> valid = ValidatePhysicsQueryHit(hit, descriptor); valid.HasError())
                return valid;
            candidates[candidateCount++] = std::move(hit);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendRayQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                      const PhysicsRayQuery &ray,
                                                      const FixedQueryCollector<JPH::CastRayCollector> &collector,
                                                      std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                      std::size_t &candidateCount) {
            for (std::size_t index = 0; index < collector.count; ++index) {
                const auto &hit = collector.values[index];
                const Math::Vec3 position = ray.origin + ray.direction * (ray.maximumDistanceMeters * hit.mFraction);
                if (const Result<void> appended = AppendCanonicalHit(world, descriptor, candidates, candidateCount,
                                                                     {.body = hit.mBodyID,
                                                                      .position = position,
                                                                      .normal = RayNormal(world, hit.mBodyID, hit.mSubShapeID2, position),
                                                                      .distance = ray.maximumDistanceMeters * hit.mFraction});
                    appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendPointQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                        const PhysicsPointQuery &point,
                                                        const FixedQueryCollector<JPH::CollidePointCollector> &collector,
                                                        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                        std::size_t &candidateCount) {
            for (std::size_t index = 0; index < collector.count; ++index) {
                const auto &hit = collector.values[index];
                if (const Result<void> appended =
                        AppendCanonicalHit(world, descriptor, candidates, candidateCount,
                                           {.body = hit.mBodyID, .position = point.point, .normal = std::nullopt, .distance = 0.0F});
                    appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        template <typename Collector, typename DistanceFunction>
        [[nodiscard]] Result<void> AppendContactQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                          const FixedQueryCollector<Collector> &collector,
                                                          std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                          std::size_t &candidateCount, const DistanceFunction &distanceFunction) {
            for (std::size_t index = 0; index < collector.count; ++index) {
                const auto &hit = collector.values[index];
                const Math::Vec3 position{hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()};
                if (const Result<void> appended = AppendCanonicalHit(world, descriptor, candidates, candidateCount,
                                                                     {.body = hit.mBodyID2,
                                                                      .position = position,
                                                                      .normal = ContactNormal(hit.mPenetrationAxis),
                                                                      .distance = distanceFunction(hit)});
                    appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        /** @brief Projects native query collectors into deterministic, bounded Horo hit storage. */
        [[nodiscard]] Result<void> ProjectCanonicalQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                             const CanonicalQueryCollectors &collectors,
                                                             std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                             std::size_t &candidateCount) {
            if (const auto *ray = std::get_if<PhysicsRayQuery>(&descriptor.geometry))
                return AppendRayQueryHits(world, descriptor, *ray, collectors.ray, candidates, candidateCount);
            if (const auto *point = std::get_if<PhysicsPointQuery>(&descriptor.geometry))
                return AppendPointQueryHits(world, descriptor, *point, collectors.point, candidates, candidateCount);
            if (std::holds_alternative<PhysicsOverlapQuery>(descriptor.geometry))
                return AppendContactQueryHits(world, descriptor, collectors.overlap, candidates, candidateCount, [](const auto &) {
                    return 0.0F;
                });
            const auto &sweep = std::get<PhysicsSweepQuery>(descriptor.geometry);
            return AppendContactQueryHits(world, descriptor, collectors.sweep, candidates, candidateCount, [&sweep](const auto &hit) {
                return hit.mFraction * sweep.maximumDistanceMeters;
            });
        }

        /** @brief Applies collection policy, caller-storage truncation and result metadata. */
        [[nodiscard]] Result<PhysicsQueryResult> FinalizeCanonicalQuery(const PhysicsQueryDescriptor &descriptor,
                                                                        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                                        const std::size_t candidateCount,
                                                                        const std::span<PhysicsQueryHit> hits,
                                                                        const std::uint64_t schemaGeneration) {
            std::ranges::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(candidateCount), PhysicsQueryHitLess);
            std::size_t selectedCount = candidateCount;
            if (descriptor.collection == PhysicsQueryCollection::Closest || descriptor.collection == PhysicsQueryCollection::Any)
                selectedCount = std::min<std::size_t>(selectedCount, 1);
            else if (descriptor.collection == PhysicsQueryCollection::ThroughFirstBlock) {
                const auto end = candidates.begin() + static_cast<std::ptrdiff_t>(candidateCount);
                const auto firstBlock = std::ranges::find_if(candidates.begin(), end, [](const PhysicsQueryHit &hit) {
                    return hit.response == PhysicsQueryResponse::Block;
                });
                selectedCount =
                    firstBlock == end ? candidateCount : static_cast<std::size_t>(std::distance(candidates.begin(), firstBlock) + 1);
            }
            if (selectedCount > descriptor.maximumHitCount)
                return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const std::size_t copied = std::min(selectedCount, hits.size());
            std::copy_n(candidates.begin(), copied, hits.begin());
            PhysicsQueryResult result{.hitCount = static_cast<std::uint32_t>(copied),
                                      .truncated = copied < selectedCount,
                                      .filterSchemaGeneration = schemaGeneration,
                                      .broadphaseSnapshotGeneration = schemaGeneration};
            if (const Result<void> valid = ValidatePhysicsQueryResult(result, descriptor); valid.HasError())
                return Result<PhysicsQueryResult>::Failure(valid.ErrorValue());
            return Result<PhysicsQueryResult>::Success(result);
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
        if (failurePoint == CanonicalFailurePoint::SystemInitialized)
            return Result<CanonicalWorldHandle>::Failure(
                MakeError(PhysicsErrors::InitializationFailed, "Native world initialization stage."));
        return Result<CanonicalWorldHandle>::Success({world.release()});
    }

    /** @copydoc CreateCanonicalQueryFixture */
    Result<PhysicsQueryFixture> CreateCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsQueryFixtureDescriptor &fixture) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.nextFixtureSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.nextFixtureGeneration == std::numeric_limits<std::uint32_t>::max() ||
            canonical.fixtures.size() >= canonical.maximumFixtures)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const auto nativeShape = CreateNativeShape(fixture.shape);
        if (nativeShape.HasError())
            return Result<PhysicsQueryFixture>::Failure(nativeShape.ErrorValue());

        const std::uint32_t slot = canonical.nextFixtureSlot++;
        const std::uint32_t generation = canonical.nextFixtureGeneration++;
        const PhysicsQueryFixture identity{.body = {owner, {slot, generation}}, .shape = {owner, {slot, generation}}};
        JPH::BodyCreationSettings settings(nativeShape.Value().GetPtr(),
                                           JPH::RVec3(fixture.pose.translation.x, fixture.pose.translation.y, fixture.pose.translation.z),
                                           ToNative(fixture.pose.rotation), JPH::EMotionType::Static, JPH::ObjectLayer{0});
        settings.mIsSensor = fixture.trigger;
        const JPH::BodyID nativeBody = canonical.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (nativeBody.IsInvalid())
            return Result<PhysicsQueryFixture>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the query fixture body admission."));
        if (nativeBody.GetIndex() >= canonical.nativeFixtureIndices.size()) {
            canonical.system->GetBodyInterface().RemoveBody(nativeBody);
            canonical.system->GetBodyInterface().DestroyBody(nativeBody);
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
        canonical.fixtures.push_back({.fixture = identity, .descriptor = fixture, .nativeBody = nativeBody, .shape = nativeShape.Value()});
        canonical.nativeFixtureIndices[nativeBody.GetIndex()] = canonical.fixtures.size() - 1;
        ++canonical.querySchemaGeneration;
        return Result<PhysicsQueryFixture>::Success(identity);
    }

    /** @copydoc DestroyCanonicalQueryFixture */
    Result<void> DestroyCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsQueryFixture &fixture) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (const auto owner = ValidatePhysicsHandleOwner(fixture.body, fixture.body.world); owner.HasError())
            return owner;
        if (fixture.body.world.Value() != fixture.shape.world.Value() || fixture.body.slot.index != fixture.shape.slot.index ||
            fixture.body.slot.generation != fixture.shape.slot.generation)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Fixture body and shape identities must be paired."));
        const auto found = std::ranges::find_if(canonical.fixtures, [&fixture](const auto &entry) {
            return entry.fixture == fixture;
        });
        if (found == canonical.fixtures.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        canonical.system->GetBodyInterface().RemoveBody(found->nativeBody);
        canonical.system->GetBodyInterface().DestroyBody(found->nativeBody);
        const auto erasedIndex = static_cast<std::size_t>(std::distance(canonical.fixtures.begin(), found));
        canonical.nativeFixtureIndices[found->nativeBody.GetIndex()] = CanonicalWorld::InvalidFixtureIndex;
        canonical.fixtures.erase(found);
        if (erasedIndex < canonical.fixtures.size())
            canonical.nativeFixtureIndices[canonical.fixtures[erasedIndex].nativeBody.GetIndex()] = erasedIndex;
        ++canonical.querySchemaGeneration;
        return Result<void>::Success();
    }

    /** @copydoc ExecuteCanonicalQuery */
    Result<PhysicsQueryResult> ExecuteCanonicalQuery(const CanonicalWorldHandle world, const PhysicsQueryDescriptor &descriptor,
                                                     const std::span<PhysicsQueryHit> hits) {
        if (world.value == nullptr)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        CanonicalQueryCollectors collectors{canonical, descriptor.collection};
        const QueryBodyFilter bodyFilter{canonical, descriptor};
        if (const Result<void> collected = CollectCanonicalQuery(canonical, descriptor, collectors, bodyFilter); collected.HasError())
            return Result<PhysicsQueryResult>::Failure(collected.ErrorValue());

        auto &candidates = canonical.queryCandidates;
        std::size_t candidateCount{};
        if (const Result<void> projected = ProjectCanonicalQueryHits(canonical, descriptor, collectors, candidates, candidateCount);
            projected.HasError())
            return Result<PhysicsQueryResult>::Failure(projected.ErrorValue());

        return FinalizeCanonicalQuery(descriptor, candidates, candidateCount, hits, canonical.querySchemaGeneration);
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
