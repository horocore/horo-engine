#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "PhysicsStateWorldTestSupport.h"
#include "PhysicsWorldInternal.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <thread>

namespace Horo::Physics::StateTest {
    TEST_CASE("Real Physics restore cancellation preserves last-good state and validation is the explicit same-fence claim",
              "[physics][native][state][cancel]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto checkpoint = fixture.Capture();
        fixture.Advance();
        const auto lastGood = fixture.world->PublishedTick();
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        Test::RequireError(fixture.world->PrepareStateRestore(checkpoint, cancelled.Token()), PhysicsErrors::StateCancelled);
        CHECK(SamePublication(fixture.world->PublishedTick(), lastGood));
        CancellationSource beforeClaim;
        std::optional<PhysicsStateRestoreCandidate> rejected{Take(fixture.world->PrepareStateRestore(checkpoint, beforeClaim.Token()))};
        beforeClaim.RequestCancellation();
        Test::RequireError(rejected->ValidatePublication(), PhysicsErrors::StateCancelled);
        CHECK(SamePublication(fixture.world->PublishedTick(), lastGood));
        rejected.reset();
        CancellationSource afterClaim;
        auto committed = Take(fixture.world->PrepareStateRestore(checkpoint, afterClaim.Token()));
        Check(committed.ValidatePublication());
        afterClaim.RequestCancellation();
        committed.Publish();
        CHECK(fixture.world->PublishedTick().completedTick == checkpoint->World().completedTick);
        CHECK(fixture.world->PublishedTick().publicationRevision == lastGood.publicationRevision + 1);
    }

    TEST_CASE("Real Physics restore publication rejects foreign threads and preserves owner admission",
              "[physics][native][state][affinity]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto checkpoint = fixture.Capture();
        fixture.Advance();
        const auto lastGood = fixture.world->PublishedTick();
        auto candidate = Take(fixture.world->PrepareStateRestore(checkpoint));
        std::optional<Result<void>> foreignResult;
        std::thread foreign([&] {
            foreignResult.emplace(candidate.ValidatePublication());
        });
        foreign.join();
        REQUIRE(foreignResult.has_value());
        Test::RequireError(*foreignResult, PhysicsErrors::ThreadAffinityViolation);
        CHECK(SamePublication(fixture.world->PublishedTick(), lastGood));
        Check(candidate.ValidatePublication());
        candidate.Publish();
        CHECK(fixture.world->PublishedTick().completedTick == checkpoint->World().completedTick);
        CHECK(fixture.world->PublishedTick().publicationRevision == lastGood.publicationRevision + 1);
    }

    TEST_CASE("Real Physics abandoned foreign restore handle cannot publish a recycled attempt and overlap is bounded",
              "[physics][native][state][lifetime]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto checkpoint = fixture.Capture();
        const auto unchanged = fixture.world->PublishedTick();
        auto moved = Take(fixture.world->PrepareStateRestore(checkpoint));
        std::thread foreign([owned = std::move(moved)]() mutable {
        });
        foreign.join();
        Test::RequireError(moved.ValidatePublication(), PhysicsErrors::InvalidState);
        auto current = Take(fixture.world->PrepareStateRestore(checkpoint));
        Test::RequireError(fixture.world->PrepareStateRestore(checkpoint), PhysicsErrors::CapacityExceeded);
        CHECK(SamePublication(fixture.world->PublishedTick(), unchanged));
        Check(current.ValidatePublication());
        current.Publish();
        CHECK(fixture.world->PublishedTick().publicationRevision == unchanged.publicationRevision + 1);
    }

    TEST_CASE("Real Physics restore rejects intervening publication and immutable topology changes without touching the current world",
              "[physics][native][state][stale]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto checkpoint = fixture.Capture();
        std::optional<PhysicsStateRestoreCandidate> old{Take(fixture.world->PrepareStateRestore(checkpoint))};
        fixture.Advance();
        const auto newer = fixture.world->PublishedTick();
        Test::RequireError(old->ValidatePublication(), PhysicsErrors::CapabilityStale);
        CHECK(SamePublication(fixture.world->PublishedTick(), newer));
        old.reset();
        Take(fixture.world->CreateSceneShape(PhysicsSphereShape{.25F}));
        const auto edited = fixture.world->PublishedTick();
        Test::RequireError(fixture.world->PrepareStateRestore(checkpoint), PhysicsErrors::CapabilityStale);
        CHECK(SamePublication(fixture.world->PublishedTick(), edited));
    }

    TEST_CASE("Real Physics snapshot slots reject capacity without overwriting old leases and recycle only after their readers drop",
              "[physics][native][state][slots]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure(2));
        auto first = fixture.Capture();
        auto second = fixture.Capture();
        const std::vector<std::byte> bytes(first->Bytes().begin(), first->Bytes().end());
        Test::RequireError(fixture.world->CaptureState(), PhysicsErrors::CapacityExceeded);
        auto copy = first;
        first = {};
        Test::RequireError(fixture.world->CaptureState(), PhysicsErrors::CapacityExceeded);
        copy = {};
        fixture.Advance();
        auto replacement = fixture.Capture();
        CHECK(replacement->World().completedTick != second->World().completedTick);
        CHECK(std::ranges::equal(second->Bytes(), bytes));
    }

    TEST_CASE("Real Physics static image admission rejects an unimplemented dynamic-module lifetime profile before any world exists",
              "[physics][native][state][compatibility]") {
        auto runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
        CHECK(runtime->Capability(PhysicsCapability::StateSnapshots) == PhysicsCapabilitySupport::Unavailable);
        CHECK(runtime->Capability(PhysicsCapability::AtomicStateRestore) == PhysicsCapabilitySupport::Unavailable);
        auto binding = ActualStaticBinding();
        binding.lifetime = PhysicsStateCodeLifetime::ModuleLease;
        Test::RequireError(runtime->BindStateSnapshotBuild(binding, StateRuntimeBudget(2, 128U * 1024U * 1024U)),
                           PhysicsErrors::CapabilityUnavailable);
        CHECK(runtime->State() == PhysicsRuntimeState::Ready);
        binding.lifetime = PhysicsStateCodeLifetime::ProcessStatic;
        Check(runtime->BindStateSnapshotBuild(binding, StateRuntimeBudget(2, 128U * 1024U * 1024U)));
        CHECK(runtime->Capability(PhysicsCapability::StateSnapshots) == PhysicsCapabilitySupport::Available);
        CHECK(runtime->Capability(PhysicsCapability::AtomicStateRestore) == PhysicsCapabilitySupport::Available);
        runtime->Shutdown();
        CHECK(runtime->Capability(PhysicsCapability::StateSnapshots) == PhysicsCapabilitySupport::Unavailable);
    }

    TEST_CASE("Real Physics quarantine changes retained topology and cannot silently resurrect an older checkpoint body",
              "[physics][native][state][quarantine]") {
        auto descriptor = StateWorldSettings().Values();
        descriptor.nonFinitePolicy = PhysicsNonFinitePolicy::QuarantineBody;
        WorldFixture fixture{Take(PhysicsWorldSettings::Capture(descriptor))};
        static_cast<void>(fixture.Configure());
        auto checkpoint = fixture.Capture();
        REQUIRE(PhysicsWorldContainmentTestAccess::Inject(*fixture.world, fixture.bodies[1].body, std::numeric_limits<float>::quiet_NaN()));
        fixture.Advance();
        const auto current = fixture.world->PublishedTick();
        CHECK(fixture.world->State() == PhysicsWorldState::ActiveSolver);
        Test::RequireError(fixture.world->PrepareStateRestore(checkpoint), PhysicsErrors::CapabilityStale);
        Test::RequireError(fixture.world->CaptureState(), PhysicsErrors::CapabilityStale);
        CHECK(SamePublication(fixture.world->PublishedTick(), current));
        CHECK(checkpoint->Solver().bodies.size() == 3);
    }

    TEST_CASE("Real Physics restore preserves receiving nonreuse authority after an equivalent transient topology roundtrip",
              "[physics][native][state][identity]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto checkpoint = fixture.Capture();
        PhysicsQueryFixtureDescriptor descriptor;
        descriptor.shape = PhysicsSphereShape{.25F};
        descriptor.pose.translation = {10, 10, 0};
        descriptor.layer = fixture.schema->Layers().front().id;
        descriptor.profile = fixture.schema->DefaultProfile();
        descriptor.channel = fixture.schema->QueryChannels().front().id;
        const auto retired = Take(fixture.world->CreateQueryFixture(descriptor));
        Check(fixture.world->DestroyQueryFixture(retired));
        fixture.Advance();
        auto candidate = Take(fixture.world->PrepareStateRestore(checkpoint));
        Check(candidate.ValidatePublication());
        candidate.Publish();
        const auto next = Take(fixture.world->CreateQueryFixture(descriptor));
        CHECK(next.body != retired.body);
        CHECK(next.shape != retired.shape);
        CHECK(next.body.slot.index > retired.body.slot.index);
        CHECK(next.body.slot.generation > retired.body.slot.generation);
    }
}  // namespace Horo::Physics::StateTest
