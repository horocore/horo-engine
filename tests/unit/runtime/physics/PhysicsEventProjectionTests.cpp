#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsEventProjection.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
#include <vector>

namespace Horo::Physics::Detail {
    namespace {
        constexpr std::array<std::uint8_t, 16> LayerBytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        constexpr std::array<std::uint8_t, 16> ProfileBytes{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

        [[nodiscard]] PhysicsWorldId World() {
            return PhysicsWorldId::Create(17).Value();
        }

        [[nodiscard]] PhysicsEventEndpoint Endpoint(const std::uint32_t index, const std::uint32_t generation = 1) {
            return {.body = {World(), {index, generation}},
                    .shape = {World(), {index, generation}},
                    .subshape = PhysicsShapeSubresourceId::FromValue(index),
                    .layer = CollisionLayerId::FromBytes(LayerBytes),
                    .profile = CollisionProfileId::FromBytes(ProfileBytes),
                    .filterSchemaGeneration = 1};
        }

        [[nodiscard]] PhysicsContactObservation Observation(const std::uint64_t tick, const std::uint32_t first, const std::uint32_t second,
                                                            const bool sensor = false) {
            return {.simulationTick = tick,
                    .first = Endpoint(first),
                    .second = Endpoint(second),
                    .contact = {.position = {static_cast<float>(first), 0.5F, static_cast<float>(second)},
                                .normal = {0.0F, 1.0F, 0.0F},
                                .penetrationDepthMeters = 0.02F,
                                .normalImpulseNewtonSeconds = 0.0F},
                    .sensor = sensor};
        }

        bool CaptureOne(PhysicsEventProjection &projection, const std::uint64_t tick, const std::uint32_t first, const std::uint32_t second,
                        const bool sensor = false) {
            projection.BeginTick(tick);
            return projection.TryCapture(Observation(tick, first, second, sensor));
        }

        bool CapturePair(PhysicsEventProjection &projection, const std::uint64_t tick, const std::uint32_t first,
                         const std::uint32_t second, const bool sensor = false) {
            projection.BeginTick(tick);
            return projection.TryCapture(Observation(tick, first, second, sensor)) &&
                   projection.TryCapture(Observation(tick, first + 2, second + 2, sensor));
        }

        void RequireCode(const Result<PhysicsEventProjectionResult> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Physics event projection emits one canonical solid lifecycle", "[physics][events][projection]") {
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);

        projection.BeginTick(1);
        auto first = Observation(1, 2, 1);
        first.contact.normal = {1.0F, 0.0F, 0.0F};
        REQUIRE(projection.TryCapture(first));
        auto completed = projection.CompleteTick(1);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 1);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::ContactBegin);
        REQUIRE(projection.PublishedEvents()[0].pair.first.body.slot.index == 1);
        REQUIRE((projection.PublishedEvents()[0].contact.normal == Math::Vec3{-1.0F, 0.0F, 0.0F}));

        projection.BeginTick(2);
        REQUIRE(projection.TryCapture(Observation(2, 1, 2)));
        completed = projection.CompleteTick(2);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 1);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::ContactPersist);

        projection.BeginTick(3);
        completed = projection.CompleteTick(3);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 1);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::ContactEnd);
    }

    TEST_CASE("Physics event projection treats sensors as enter and exit without persistence", "[physics][events][projection]") {
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);

        REQUIRE(CaptureOne(projection, 1, 1, 2, true));
        REQUIRE(projection.CompleteTick(1).Value().publishedRecordCount == 1);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::TriggerEnter);

        REQUIRE(CaptureOne(projection, 2, 1, 2, true));
        REQUIRE(projection.CompleteTick(2).Value().publishedRecordCount == 0);
        REQUIRE(projection.PublishedEvents().empty());

        projection.BeginTick(3);
        REQUIRE(projection.CompleteTick(3).Value().publishedRecordCount == 1);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::TriggerExit);
    }

    TEST_CASE("Physics event projection rejects stale observations and reconciles interleaved pairs", "[physics][events][projection]") {
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);

        RequireCode(projection.CompleteTick(1), PhysicsErrors::DescriptorInvalid);
        projection.BeginTick(1);
        RequireCode(projection.CompleteTick(2), PhysicsErrors::DescriptorInvalid);
        projection.AbortTick();

        auto invalid = Observation(1, 1, 2);
        invalid.contact.normal.x = std::numeric_limits<float>::quiet_NaN();
        projection.BeginTick(1);
        REQUIRE_FALSE(projection.TryCapture(invalid));
        REQUIRE(projection.CompleteTick(1).Value().publishedRecordCount == 0);

        REQUIRE(CapturePair(projection, 2, 1, 2));
        REQUIRE(projection.CompleteTick(2).Value().publishedRecordCount == 2);

        projection.BeginTick(3);
        REQUIRE(projection.TryCapture(Observation(3, 2, 3)));
        const auto completed = projection.CompleteTick(3);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 3);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::ContactEnd);
        REQUIRE(projection.PublishedEvents()[1].kind == PhysicsEventKind::ContactBegin);
        REQUIRE(projection.PublishedEvents()[2].kind == PhysicsEventKind::ContactEnd);
    }

    TEST_CASE("Physics event projection emits a bounded transition when a pair changes sensor classification",
              "[physics][events][projection]") {
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);

        REQUIRE(CaptureOne(projection, 1, 1, 2, true));
        REQUIRE(projection.CompleteTick(1).Value().publishedRecordCount == 1);

        REQUIRE(CaptureOne(projection, 2, 1, 2, false));
        const auto completed = projection.CompleteTick(2);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 2);
        REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::TriggerExit);
        REQUIRE(projection.PublishedEvents()[1].kind == PhysicsEventKind::ContactBegin);
    }

    TEST_CASE("Physics event projection owns callback values and coalesces duplicate manifolds", "[physics][events][projection]") {
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);
        auto materialBytes = std::array<std::uint8_t, 16>{};
        materialBytes.back() = 9;

        auto observation = Observation(1, 1, 2);
        observation.firstMaterial = PhysicsEventMaterial{.asset = Assets::AssetId::FromBytes(materialBytes),
                                                         .assetGeneration = 4,
                                                         .slot = PhysicsMaterialSlotId::FromValue(7)};
        projection.BeginTick(1);
        REQUIRE(projection.TryCapture(observation));
        REQUIRE(projection.TryCapture(observation));
        observation.contact.position = {99.0F, 99.0F, 99.0F};

        const auto completed = projection.CompleteTick(1);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 1);
        REQUIRE((projection.PublishedEvents()[0].contact.position == Math::Vec3{1.0F, 0.5F, 2.0F}));
        REQUIRE(projection.PublishedEvents()[0].firstMaterial->assetGeneration == 4);
    }

    TEST_CASE("Physics event projection bounds concurrent callback capture and reports drops", "[physics][events][projection]") {
        PhysicsEventProjection projection(8, 16, PhysicsEventOverflowPolicy::DropNewest);
        projection.BeginTick(1);
        std::atomic_bool captureFailed{};
        std::vector<std::thread> callbacks;
        callbacks.reserve(8);
        for (std::uint32_t index = 0; index < 8; ++index)
            callbacks.emplace_back([&projection, &captureFailed, index] {
                if (!projection.TryCapture(Observation(1, index + 1, 100 + index)))
                    captureFailed.store(true, std::memory_order_release);
            });
        for (auto &callback : callbacks)
            callback.join();

        const auto completed = projection.CompleteTick(1);
        REQUIRE(completed.HasValue());
        REQUIRE_FALSE(captureFailed.load(std::memory_order_acquire));
        REQUIRE(completed.Value().publishedRecordCount == 8);
        REQUIRE(completed.Value().droppedRecordCount == 0);
    }

    TEST_CASE("Physics event projection applies drop-newest and fail-tick overflow policies", "[physics][events][projection]") {
        SECTION("drop newest retains a coherent bounded lifecycle") {
            PhysicsEventProjection projection(1, 4, PhysicsEventOverflowPolicy::DropNewest);
            REQUIRE(CapturePair(projection, 1, 1, 2));
            const auto completed = projection.CompleteTick(1);
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value().overflowed);
            REQUIRE(completed.Value().droppedRecordCount == 1);
            REQUIRE(completed.Value().publishedRecordCount == 1);
            REQUIRE(projection.DroppedRecordCount() == 1);

            REQUIRE(CapturePair(projection, 2, 1, 2));
            REQUIRE(projection.CompleteTick(2).Value().publishedRecordCount == 1);
            REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::ContactPersist);
        }

        SECTION("fail tick does not replace the prior publication") {
            PhysicsEventProjection projection(1, 1, PhysicsEventOverflowPolicy::FailTick);
            projection.BeginTick(1);
            REQUIRE(projection.TryCapture(Observation(1, 1, 2)));
            REQUIRE_FALSE(projection.TryCapture(Observation(1, 3, 4)));
            RequireCode(projection.CompleteTick(1), PhysicsErrors::CapacityExceeded);
            REQUIRE(projection.PublishedTick() == 0);
            REQUIRE(projection.PublishedEvents().empty());

            projection.BeginTick(2);
            REQUIRE(projection.TryCapture(Observation(2, 1, 2)));
            REQUIRE(projection.CompleteTick(2).HasValue());
            REQUIRE(projection.PublishedEvents()[0].kind == PhysicsEventKind::ContactBegin);
        }
    }
}  // namespace Horo::Physics::Detail
