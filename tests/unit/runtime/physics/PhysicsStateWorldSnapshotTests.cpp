#include "AllocationProbe.h"
#include "PhysicsStateWorldTestSupport.h"

#include <algorithm>
#include <optional>

namespace Horo::Physics::StateTest {
    TEST_CASE("Real public Physics snapshot preserves inverse-ID pending event transcript and future continuation",
              "[physics][native][state][world][events]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto original = fixture.Capture();
        const auto settings = StateWorldSettings();
        const auto &values = settings.Values().world;
        CHECK(original->Solver().bodies.size() == 3);
        CHECK(original->Solver().gravity == std::array{values.gravity.x, values.gravity.y, values.gravity.z});
        CHECK(original->Solver().previousDelta == static_cast<float>(values.fixedDeltaSeconds));
        const auto before = ReadTranscript(*fixture.world);
        REQUIRE(before.count == 2);
        REQUIRE(original->World().publishedEvents.size() == before.count);
        CHECK(original->World().publishedEvents[0].ordinal == 1);
        CHECK(original->World().publishedEvents[1].ordinal == 0);
        const std::vector<std::byte> immutable(original->Bytes().begin(), original->Bytes().end());
        auto oldAccess = Take(fixture.world->IssueQueryEventCapability());
        fixture.Advance();
        const auto future = ReadTranscript(*fixture.world);
        auto expected = fixture.Capture();
        const auto revision = fixture.world->PublishedTick().publicationRevision;
        auto restore = Take(fixture.world->PrepareStateRestore(original));
        Check(restore.ValidatePublication());
        restore.Publish();
        CHECK(fixture.world->PublishedTick().completedTick == original->World().completedTick);
        CHECK(fixture.world->PublishedTick().publicationRevision == revision + 1);
        const auto restored = ReadTranscript(*fixture.world);
        REQUIRE(restored.count == before.count);
        CHECK(std::ranges::equal(std::span{restored.events}.first(restored.count), std::span{before.events}.first(before.count)));
        CHECK(std::ranges::equal(original->Bytes(), immutable));
        std::array<PhysicsEventRecord, 8> ignored;
        const auto published = fixture.world->PublishedTick();
        Test::RequireError(oldAccess.ReadEvents({oldAccess.Identity(), published.eventTick, published.publicationRevision, 8}, ignored),
                           PhysicsErrors::CapabilityStale);
        fixture.Advance();
        const auto continued = ReadTranscript(*fixture.world);
        REQUIRE(continued.count == future.count);
        CHECK(std::ranges::equal(std::span{continued.events}.first(continued.count), std::span{future.events}.first(future.count)));
        auto actual = fixture.Capture();
        CHECK(std::ranges::equal(actual->Bytes(), expected->Bytes()));
    }

    TEST_CASE("Real public Physics capture and complete prepared restore reuse their cold storage without C++ heap requests",
              "[physics][native][state][allocation]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        PhysicsStateSnapshotLease captured;
        bool captureSucceeded{};
        Tests::AllocationProbe::Measurement captureCost;
        {
            Tests::AllocationProbe::ScopedMeasurement measured;
            auto result = fixture.world->CaptureState();
            captureSucceeded = result.HasValue();
            if (captureSucceeded)
                captured = std::move(result).Value();
            captureCost = measured.Snapshot();
        }
        REQUIRE(captureSucceeded);
        CHECK(captureCost.requests == 0);
        fixture.Advance();
        bool prepared{}, validated{};
        std::optional<PhysicsStateRestoreCandidate> candidate;
        Tests::AllocationProbe::Measurement restoreCost;
        {
            Tests::AllocationProbe::ScopedMeasurement measured;
            auto result = fixture.world->PrepareStateRestore(captured);
            prepared = result.HasValue();
            if (prepared) {
                candidate.emplace(std::move(result).Value());
                validated = candidate->ValidatePublication().HasValue();
                if (validated)
                    candidate->Publish();
            }
            restoreCost = measured.Snapshot();
        }
        REQUIRE(prepared);
        REQUIRE(validated);
        CHECK(restoreCost.requests == 0);
    }

    TEST_CASE("Real public Physics canonical decode and late malformed input preserve held immutable values and the last-good world",
              "[physics][native][state][decode]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto original = fixture.Capture();
        const std::vector<std::byte> bytes(original->Bytes().begin(), original->Bytes().end());
        auto decoded = Take(fixture.world->DecodeStateSnapshot(bytes));
        CHECK(std::ranges::equal(decoded->Bytes(), bytes));
        const auto publication = fixture.world->PublishedTick();
        std::vector<std::byte> damaged = bytes;
        damaged.back() ^= std::byte{1};
        REQUIRE(fixture.world->DecodeStateSnapshot(std::span{damaged}.first(damaged.size() - 1)).HasError());
        CHECK(SamePublication(fixture.world->PublishedTick(), publication));
        CHECK(std::ranges::equal(original->Bytes(), bytes));
        damaged = bytes;
        damaged[44] ^= std::byte{1};  // Actual closed header's build identity; no alternate serializer or native payload.
        Test::RequireError(fixture.world->DecodeStateSnapshot(damaged), PhysicsErrors::ProfileUnsupported);
        CHECK(SamePublication(fixture.world->PublishedTick(), publication));
        auto restore = Take(fixture.world->PrepareStateRestore(decoded));
        Check(restore.ValidatePublication());
        restore.Publish();
    }

    TEST_CASE("Real public Physics cold allocation and retained-byte rejection leave the old admitted pool and world intact",
              "[physics][native][state][failure]") {
        WorldFixture fixture;
        const auto admittedBytes = fixture.Configure();
        auto previous = fixture.Capture();
        const std::vector<std::byte> bytes(previous->Bytes().begin(), previous->Bytes().end());
        const auto publication = fixture.world->PublishedTick();
        {
            Tests::AllocationProbe::ScopedFailure failure;
            Test::RequireError(fixture.world->ConfigureStateSnapshots(fixture.Configuration()), PhysicsErrors::CapacityExceeded);
        }
        auto small = fixture.Configuration();
        REQUIRE(admittedBytes > 1);
        small.maximumRetainedBytes = admittedBytes - 1;
        Test::RequireError(fixture.world->ConfigureStateSnapshots(small), PhysicsErrors::CapacityExceeded);
        CHECK(SamePublication(fixture.world->PublishedTick(), publication));
        CHECK(std::ranges::equal(previous->Bytes(), bytes));
        auto stillReady = fixture.Capture();
        CHECK(std::ranges::equal(stillReady->Bytes(), bytes));
    }
}  // namespace Horo::Physics::StateTest
