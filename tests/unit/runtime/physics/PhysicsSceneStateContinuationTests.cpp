#include "AllocationProbe.h"
#include "PhysicsSceneStateTestSupport.h"

#include <thread>

namespace Horo::Physics::StateTest {
    TEST_CASE("Actual empty pending Character owner adopts the restored Physics publication and advances N plus one",
              "[physics][native][state][scene][character]") {
        auto runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
        Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget(16, 1024U * 1024U * 1024U)));
        SceneOwnerFixture source{*runtime, 41};
        source.Publish();
        source.Advance(5);
        auto capture = source.PrepareCapture();
        PhysicsStateSnapshotLease checkpoint;
        Tests::AllocationProbe::Measurement measured;
        bool captured{};
        {
            Tests::AllocationProbe::ScopedMeasurement probe;
            auto result = capture->Capture();
            captured = result.HasValue();
            if (captured)
                checkpoint = std::move(result).Value();
            measured = probe.Snapshot();
        }
        REQUIRE(captured);
        CHECK(measured.requests == 0);
        REQUIRE(checkpoint->World().completedTick == 5);
        SceneOwnerFixture receiver{*runtime, 87, 29};
        auto restored = receiver.Restore(checkpoint->Bytes());
        Check(restored->ValidateAndArm());
        receiver.Publish();
        auto pair = Take(receiver.host.Borrow());
        CHECK(pair.physics.PublishedTick().completedTick == 5);
        CHECK(pair.character.PublishedTick().completedTick == 5);
        receiver.Advance();
        CHECK(pair.physics.PublishedTick().completedTick == 6);
        CHECK(pair.character.PublishedTick().completedTick == 6);
        auto recapture = receiver.PrepareCapture();
        auto current = Take(recapture->Capture());
        CHECK(std::ranges::equal(current->ConstructionBytes(), checkpoint->ConstructionBytes()));
    }

    TEST_CASE("Actual Scene pending target denies foreign scene IDs and owner lanes before touching a native world",
              "[physics][native][state][scene][affinity]") {
        auto runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
        Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget()));
        SceneOwnerFixture receiver{*runtime, 63};
        CHECK(receiver.host.PendingStateTarget({64}).HasError());
        const auto target = Take(receiver.host.PendingStateTarget({63}));
        bool denied{};
        std::thread worker([&] {
            denied = target.DecodeInput({}, 1024, 1024).HasError();
        });
        worker.join();
        CHECK(denied);
        Check(receiver.candidate->ValidatePublication());
        receiver.Publish();
        receiver.Advance();
        CHECK(Take(receiver.host.Borrow()).physics.PublishedTick().completedTick == 1);
    }

    TEST_CASE("Full persistent Physics owner keys deny changed UUID high bits despite identical local Scene descriptors",
              "[physics][native][state][scene][identity]") {
        auto runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
        Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget(16, 1024U * 1024U * 1024U)));
        SceneOwnerFixture source{*runtime, 9};
        source.Publish();
        source.Advance();
        auto captured = source.PrepareCapture();
        auto checkpoint = Take(captured->Capture());
        SceneOwnerFixture receiver{*runtime, 78};
        auto records =
            std::vector<Runtime::PersistentEntityRecord>(receiver.identities.Records().begin(), receiver.identities.Records().end());
        std::vector<Runtime::PersistentEntityRuntimeBinding> bindings;
        for (auto &record : records) {
            const auto entity = Take(receiver.identities.Resolve(record.identity, record.generation));
            auto bytes = record.identity.Bytes();
            bytes.front() ^= 0x40;
            record.identity = Take(Runtime::PersistentEntityId::FromBytes(bytes));
            bindings.push_back({record.identity, record.generation, entity});
        }
        receiver.identities = Take(Runtime::PersistentEntityIdentityMap::Create(receiver.view.RuntimeId(), records, bindings));
        const auto rejected = receiver.RestoreResult(checkpoint->Bytes());
        REQUIRE(rejected.HasError());
        Check(receiver.candidate->ValidatePublication());
        receiver.Publish();
        CHECK(Take(receiver.host.Borrow()).physics.PublishedTick().completedTick == 0);
    }

    TEST_CASE("Exact Scene capture readiness waits for a real paired tick and preserves lifetime and lane errors",
              "[physics][native][state][scene][readiness]") {
        auto runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
        Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget()));
        SceneOwnerFixture source{*runtime, 96};
        source.Publish();
        const auto target = Take(source.host.PublishedStateTarget());
        CHECK_FALSE(Take(target.CaptureReady()));
        source.Advance();
        CHECK(Take(target.CaptureReady()));
        bool affinity{};
        std::thread foreign([&] {
            const auto result = target.CaptureReady();
            affinity = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        CHECK(affinity);
        source.candidate->Shutdown();
        Test::RequireError(target.CaptureReady(), PhysicsErrors::CapabilityRevoked);
    }
}  // namespace Horo::Physics::StateTest
