#include "Horo/Physics/PhysicsDebugSnapshot.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] PhysicsWorldId World(const std::uint64_t value = 913) {
            return PhysicsWorldId::Create(value).Value();
        }

        [[nodiscard]] PhysicsDebugBudget Budget() {
            PhysicsDebugBudget budget;
            budget.maximumPayloadBytes = MaximumPhysicsDebugPayloadBytes;
            for (auto &category : budget.categories)
                category = {.maximumRecords = MaximumPhysicsDebugRecords, .maximumPayloadBytes = MaximumPhysicsDebugPayloadBytes};
            return budget;
        }

        [[nodiscard]] PhysicsDebugSource Source(const PhysicsWorldId world = World(), const std::uint64_t tick = 1,
                                                const std::uint64_t revision = 1) {
            return {.world = world, .simulationTick = tick, .publicationRevision = revision};
        }

        [[nodiscard]] PhysicsPublishedTick Published(const std::uint64_t tick = 1, const std::uint64_t revision = 1) {
            return {.completedTick = tick, .publicationRevision = revision};
        }

        [[nodiscard]] std::array<PhysicsDebugRecord, PhysicsDebugCategoryCount> Records() {
            const BodyHandle body{World(), {1, 1}};
            const BodyHandle second{World(), {2, 1}};
            const ShapeHandle shape{World(), {3, 1}};
            const ConstraintHandle constraint{World(), {4, 1}};
            PhysicsEventRecord event;
            event.simulationTick = 1;
            event.pair.first.body = body;
            event.pair.second.body = second;
            PhysicsQueryHit hit;
            hit.body = body;
            hit.shape = shape;
            return {PhysicsDebugBody{body},
                    PhysicsDebugShape{shape, body},
                    PhysicsDebugContact{event},
                    PhysicsDebugConstraint{constraint, body, second},
                    PhysicsDebugBroadphase{body, second},
                    PhysicsDebugQuery{hit},
                    PhysicsDebugPipeline{1, 2, 3}};
        }

#if HORO_TEST_PHYSICS_NATIVE
        /** @brief Checks the native projection's supported categories and copied identities. */
        void RequireProjectedEvidence(const PhysicsDebugSnapshot &snapshot, const BodyHandle body, const ShapeHandle shape,
                                      const ConstraintHandle constraint, const PhysicsPublishedTick &published) {
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Body).captured == 1);
            REQUIRE(std::get<PhysicsDebugBody>(snapshot.Records(PhysicsDebugCategory::Body)[0]).body == body);
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Shape).captured == 1);
            REQUIRE(std::get<PhysicsDebugShape>(snapshot.Records(PhysicsDebugCategory::Shape)[0]).shape == shape);
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Constraint).captured == 1);
            REQUIRE(std::get<PhysicsDebugConstraint>(snapshot.Records(PhysicsDebugCategory::Constraint)[0]).constraint == constraint);
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Contact).availability == PhysicsDebugAvailability::Available);
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Broadphase).availability == PhysicsDebugAvailability::Unavailable);
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Query).availability == PhysicsDebugAvailability::Unavailable);
            REQUIRE(snapshot.Evidence(PhysicsDebugCategory::Pipeline).captured == 1);
            REQUIRE(std::get<PhysicsDebugPipeline>(snapshot.Records(PhysicsDebugCategory::Pipeline)[0]).eventCount == published.eventCount);
        }

        /** @brief Confirms that per-category zero limits filter native records without losing evidence. */
        void RequireBoundedProjection(const PhysicsWorld &world) {
            auto limited = Budget();
            limited.categories[static_cast<std::size_t>(PhysicsDebugCategory::Body)] = {};
            limited.categories[static_cast<std::size_t>(PhysicsDebugCategory::Constraint)] = {};
            const auto bounded = world.CaptureDebugSnapshot(limited);
            REQUIRE(bounded.HasValue());
            REQUIRE(bounded.Value()->Evidence(PhysicsDebugCategory::Body).captured == 0);
            REQUIRE(bounded.Value()->Evidence(PhysicsDebugCategory::Body).truncated == 1);
            REQUIRE(bounded.Value()->Evidence(PhysicsDebugCategory::Constraint).truncated == 1);
        }
#endif
    }  // namespace

    TEST_CASE("Physics debug snapshot copies every category and survives source destruction", "[physics][debug]") {
        static_assert(std::is_same_v<decltype(std::declval<PhysicsDebugSnapshot>().Records(PhysicsDebugCategory::Body)),
                                     std::span<const PhysicsDebugRecord>>);
        std::shared_ptr<const PhysicsDebugSnapshot> retained;
        {
            auto records = Records();
            auto source = Source();
            for (std::size_t index = 0; index < records.size(); ++index)
                source.categories[index] = {.availability = PhysicsDebugAvailability::Available,
                                            .records = std::span<const PhysicsDebugRecord>{&records[index], 1}};
            const auto captured = CapturePhysicsDebugSnapshot(source, Published(), Budget());
            REQUIRE(captured.HasValue());
            retained = captured.Value();
            records[0] = PhysicsDebugBody{BodyHandle{World(), {99, 1}}};
        }
        REQUIRE(retained->Matches(World(), Published()));
        REQUIRE_FALSE(retained->Matches(World(914), Published()));
        REQUIRE_FALSE(retained->Matches(World(), Published(2, 2)));
        REQUIRE(retained->World() == World());
        REQUIRE(retained->SimulationTick() == 1);
        REQUIRE(retained->PublicationRevision() == 1);
        for (std::size_t index = 0; index < PhysicsDebugCategoryCount; ++index) {
            const auto category = static_cast<PhysicsDebugCategory>(index);
            REQUIRE(retained->Evidence(category).availability == PhysicsDebugAvailability::Available);
            REQUIRE(retained->Evidence(category).captured == 1);
            REQUIRE(retained->Evidence(category).truncated == 0);
            REQUIRE(retained->Records(category).size() == 1);
        }
        REQUIRE(std::get<PhysicsDebugBody>(retained->Records(PhysicsDebugCategory::Body)[0]).body.slot.index == 1);
    }

    TEST_CASE("Physics debug budgets separate capture truncation from producer drops", "[physics][debug]") {
        const BodyHandle body{World(), {1, 1}};
        const std::array<PhysicsDebugRecord, 3> bodies{PhysicsDebugBody{body}, PhysicsDebugBody{body}, PhysicsDebugBody{body}};
        auto source = Source();
        source.categories[0] = {.availability = PhysicsDebugAvailability::Available, .records = bodies, .droppedBeforeCapture = 4};
        source.categories[1] = {.availability = PhysicsDebugAvailability::Unavailable};
        auto budget = Budget();
        budget.categories[0].maximumRecords = 2;
        budget.categories[0].maximumPayloadBytes = sizeof(PhysicsDebugRecord);
        const auto captured = CapturePhysicsDebugSnapshot(source, Published(), budget);
        REQUIRE(captured.HasValue());
        const auto evidence = captured.Value()->Evidence(PhysicsDebugCategory::Body);
        REQUIRE(evidence.captured == 1);
        REQUIRE(evidence.truncated == 2);
        REQUIRE(evidence.dropped == 4);
        REQUIRE(evidence.payloadBytes == sizeof(PhysicsDebugRecord));
        REQUIRE(captured.Value()->Evidence(PhysicsDebugCategory::Shape).availability == PhysicsDebugAvailability::Unavailable);

        budget.categories[0] = {};
        const auto filtered = CapturePhysicsDebugSnapshot(source, Published(), budget);
        REQUIRE(filtered.HasValue());
        REQUIRE(filtered.Value()->Evidence(PhysicsDebugCategory::Body).captured == 0);
        REQUIRE(filtered.Value()->Evidence(PhysicsDebugCategory::Body).truncated == 3);

        budget = Budget();
        budget.maximumPayloadBytes = sizeof(PhysicsDebugRecord);
        const auto globallyBounded = CapturePhysicsDebugSnapshot(source, Published(), budget);
        REQUIRE(globallyBounded.HasValue());
        REQUIRE(globallyBounded.Value()->Evidence(PhysicsDebugCategory::Body).truncated == 2);
        REQUIRE(globallyBounded.Value()->PayloadBytes() == sizeof(PhysicsDebugRecord));
    }

    TEST_CASE("Every Physics debug category reports filtered and producer-lost records", "[physics][debug]") {
        const auto records = Records();
        auto source = Source();
        auto budget = Budget();
        for (std::size_t index = 0; index < PhysicsDebugCategoryCount; ++index) {
            source.categories[index] = {.availability = PhysicsDebugAvailability::Available,
                                        .records = std::span<const PhysicsDebugRecord>{&records[index], 1},
                                        .droppedBeforeCapture = 2};
            budget.categories[index] = {};
        }
        const auto captured = CapturePhysicsDebugSnapshot(source, Published(), budget);
        REQUIRE(captured.HasValue());
        for (std::size_t index = 0; index < PhysicsDebugCategoryCount; ++index) {
            const auto evidence = captured.Value()->Evidence(static_cast<PhysicsDebugCategory>(index));
            REQUIRE(evidence.availability == PhysicsDebugAvailability::Available);
            REQUIRE(evidence.captured == 0);
            REQUIRE(evidence.truncated == 1);
            REQUIRE(evidence.dropped == 2);
        }
    }

    TEST_CASE("Physics debug rejects stale or malformed evidence before publication", "[physics][debug]") {
        auto source = Source();
        const auto budget = Budget();
        const auto stale = CapturePhysicsDebugSnapshot(source, Published(2, 2), budget);
        REQUIRE(stale.HasError());
        REQUIRE(stale.ErrorValue().code.Value() == PhysicsErrors::QuerySnapshotStale.code.Value());
        const std::array<PhysicsDebugRecord, 1> wrong{PhysicsDebugPipeline{}};
        source.categories[0] = {.availability = PhysicsDebugAvailability::Available, .records = wrong};
        const auto malformed = CapturePhysicsDebugSnapshot(source, Published(), budget);
        REQUIRE(malformed.HasError());
        REQUIRE(malformed.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        const std::array<PhysicsDebugRecord, 1> foreign{PhysicsDebugBody{BodyHandle{World(914), {1, 1}}}};
        source.categories[0].records = foreign;
        REQUIRE(CapturePhysicsDebugSnapshot(source, Published(), budget).ErrorValue().code.Value() ==
                PhysicsErrors::DescriptorInvalid.code.Value());
        auto oversized = budget;
        oversized.maximumPayloadBytes = MaximumPhysicsDebugPayloadBytes + 1;
        REQUIRE(CapturePhysicsDebugSnapshot(Source(), Published(), oversized).ErrorValue().code.Value() ==
                PhysicsErrors::CapacityExceeded.code.Value());
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Physics world gates debug capture by owner, tick and lifecycle while retained values survive",
              "[physics][debug][lifecycle]") {
        auto runtime = std::move(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical)).Value();
        auto world = std::move(runtime->PrepareWorld(Test::SmallWorldSettings())).Value();
        REQUIRE(world->Activate(World()).HasValue());
        REQUIRE(world->CaptureDebugSnapshot(Budget()).ErrorValue().code.Value() == PhysicsErrors::QuerySnapshotStale.code.Value());
        const auto shape = world->CreateSceneShape(PhysicsBoxShape{});
        REQUIRE(shape.HasValue());
        PhysicsBodyDescriptor bodyDescriptor;
        bodyDescriptor.shape = shape.Value();
        bodyDescriptor.motion = PhysicsMotionType::Static;
        bodyDescriptor.mass = PhysicsNoMass{};
        const auto body = world->CreateSceneBody({bodyDescriptor, false});
        REQUIRE(body.HasValue());
        PhysicsConstraintDescriptor constraintDescriptor;
        constraintDescriptor.first = {body.Value(), {}};
        constraintDescriptor.second = PhysicsWorldAnchor{};
        constraintDescriptor.parameters = PhysicsFixedConstraint{};
        const auto constraint = world->CreateSceneConstraint(constraintDescriptor);
        REQUIRE(constraint.HasValue());
        const auto delta = Duration::FromNanoseconds(16'666'667);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = delta}).HasValue());
        const auto published = world->PublishedTick();
        bool offThreadRejected{};
        std::thread foreign([&] {
            const auto result = world->CaptureDebugSnapshot(Budget());
            offThreadRejected =
                result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(offThreadRejected);
        auto retained = world->CaptureDebugSnapshot(Budget()).Value();
        REQUIRE(retained->Matches(World(), published));
        RequireProjectedEvidence(*retained, body.Value(), shape.Value(), constraint.Value(), published);
        RequireBoundedProjection(*world);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 1, .fixedDelta = delta}).HasValue());
        REQUIRE_FALSE(retained->Matches(World(), world->PublishedTick()));
        REQUIRE(world->UnloadScene().HasValue());
        REQUIRE(world->CaptureDebugSnapshot(Budget()).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        world.reset();
        runtime.reset();
        REQUIRE(retained->Records(PhysicsDebugCategory::Pipeline).size() == 1);
        REQUIRE(retained->SimulationTick() == 1);
    }
#endif
}  // namespace Horo::Physics
