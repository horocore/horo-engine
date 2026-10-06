#include "AllocationProbe.h"
#include "ReplicationCaptureTestSupport.h"

#include <thread>

namespace Horo::Network {
    using namespace CaptureTestSupport;

    TEST_CASE("Committed capture publishes once and duplicate hints create no history", "[network][capture]") {
        Fixture fixture;
        REQUIRE(fixture.Capture(1, 3.0).published == 1);
        const auto first = fixture.Pin();
        REQUIRE(first->SimulationTick() == 1);
        REQUIRE(first->CommitRevision() > 0);
        REQUIRE(fixture.capture->MarkDirty(Object().object).HasValue());
        REQUIRE(fixture.capture->MarkDirty(Object().object).HasValue());
        REQUIRE(fixture.Capture(2, 3.0).unchanged == 1);
        REQUIRE(fixture.Pin() == first);
        REQUIRE(first->PublicationRevision() == 1);
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 2), 2).HasError());
        REQUIRE(fixture.codec->encodeCalls == 0);
        REQUIRE(fixture.owner->begins == fixture.owner->ends);
    }

    TEST_CASE("Lost hints reconcile through a stable bounded rotating cursor", "[network][capture]") {
        auto lifecycle = Lifecycle();
        auto codec = std::make_shared<CountingCodec>();
        auto registry = Registry(codec);
        std::array<std::shared_ptr<Owner>, 4> owners;
        std::array<ReplicationCaptureTarget, 4> targets;
        for (std::size_t index{}; index < targets.size(); ++index) {
            owners[index] = std::make_shared<Owner>();
            targets[index] = {Object(index + 1), owners[index]};
            REQUIRE(lifecycle.RegisterObject(World().scene, World().session, targets[index].object).HasValue());
        }
        ReplicationCaptureLimits limits;
        limits.maximumTargetsPerTick = 1;
        auto capture = std::move(ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets, limits)).Value();
        for (std::uint64_t tick = 1; tick <= 8; ++tick) {
            for (auto &owner : owners)
                owner->Commit(tick, tick > 4 ? 7.0 : 2.0);
            const auto result = capture->CaptureAtCommit(Read(lifecycle, tick), tick);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().considered == 1);
            REQUIRE(result.Value().deferred == 3);
        }
        for (const auto &target : targets)
            REQUIRE(std::get<double>(capture->Latest(target.object.object).Value()->Fields()[0].value) == 7.0);
        for (const auto &owner : owners)
            REQUIRE(owner->captures == 2);
    }

    TEST_CASE("Owner failures and uncommitted transactions preserve the prior immutable pin", "[network][capture]") {
        Fixture fixture;
        REQUIRE(fixture.Capture(1, 4.0).published == 1);
        const auto baseline = fixture.Pin();
        fixture.owner->fail = true;
        REQUIRE(fixture.Capture(2, 8.0).failed == 1);
        REQUIRE(fixture.Pin() == baseline);
        fixture.owner->fail = false;
        fixture.owner->committed = false;
        REQUIRE(fixture.Capture(3, 9.0).failed == 1);
        REQUIRE(fixture.Pin() == baseline);
        REQUIRE(std::get<double>(baseline->Fields()[0].value) == 4.0);
        REQUIRE(fixture.owner->ends == 2);
    }

    TEST_CASE("Held snapshot pins exhaust finite pools without partial publication", "[network][capture]") {
        ReplicationCaptureLimits limits;
        limits.snapshotSlotsPerTarget = 2;
        Fixture fixture{limits};
        REQUIRE(fixture.Capture(1, 1.0).published == 1);
        auto first = fixture.Pin();
        REQUIRE(fixture.Capture(2, 2.0).published == 1);
        auto second = fixture.Pin();
        REQUIRE(fixture.Capture(3, 3.0).failed == 1);
        REQUIRE(fixture.Pin() == second);
        REQUIRE(std::get<double>(first->Fields()[0].value) == 1.0);
        first.reset();
        REQUIRE(fixture.Capture(4, 4.0).published == 1);
        REQUIRE(std::get<double>(second->Fields()[0].value) == 2.0);
    }

    TEST_CASE("Current success paths allocate and reclaim no storage per tick", "[network][capture][allocation]") {
        Fixture fixture;
        fixture.Capture(1, 1.0);
        const auto initial = fixture.Pin();
        const auto before = Tests::AllocationProbe::Count();
        const auto freed = Tests::AllocationProbe::FreeCount();
        bool passed = true;
        for (std::uint64_t tick = 2; tick <= 64; ++tick) {
            fixture.owner->Commit(tick, static_cast<double>(tick));
            const auto world =
                fixture.lifecycle.AcquireCaptureRead({World().scene, World().session, Runtime::RuntimePhase::NetworkFlush, tick, {}});
            if (world.HasError()) {
                passed = false;
                break;
            }
            const auto report = fixture.capture->CaptureAtCommit(world.Value(), tick);
            if (report.HasError() || report.Value().published != 1) {
                passed = false;
                break;
            }
        }
        const auto after = Tests::AllocationProbe::Count();
        const auto reclaimed = Tests::AllocationProbe::FreeCount();
        REQUIRE(passed);
        REQUIRE(after == before);
        REQUIRE(reclaimed == freed);
        REQUIRE(initial->IsCurrent());
        REQUIRE(fixture.codec->encodeCalls == 0);
    }

    TEST_CASE("Mapping retirement invalidates old reads and snapshot eligibility immediately", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1);
        auto read = Read(fixture.lifecycle);
        const auto pin = fixture.Pin();
        REQUIRE(fixture.lifecycle.RetireObject(World().scene, World().session, Object().object).HasValue());
        REQUIRE_FALSE(read.IsCurrent());
        REQUIRE(read.Resolve(Object().object).HasError());
        REQUIRE_FALSE(pin->IsCurrent());
        REQUIRE(fixture.capture->Latest(Object().object).HasError());
        REQUIRE(fixture.lifecycle.RegisterObject(World().scene, World().session, Object(1, 2)).HasValue());
        REQUIRE(fixture.Capture(2).failed == 1);
        REQUIRE_FALSE(pin->IsCurrent());
    }

    TEST_CASE("Cancellation after owner capture publishes nothing and closes its read scope", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1);
        const auto prior = fixture.Pin();
        CancellationSource cancellation;
        fixture.owner->context = &cancellation;
        fixture.owner->onCapture = [](void *context) {
            static_cast<CancellationSource *>(context)->RequestCancellation();
        };
        REQUIRE(fixture.Capture(2, 5.0, cancellation.Token()).failed == 1);
        REQUIRE(fixture.Pin() == prior);
        REQUIRE_FALSE(fixture.owner->reading);
        REQUIRE(fixture.owner->begins == fixture.owner->ends);
    }

    TEST_CASE("Codec callbacks cannot publish after reentrant mapping replacement", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1);
        const auto prior = fixture.Pin();
        fixture.codec->context = &fixture.lifecycle;
        fixture.codec->onCompare = [](void *context) {
            auto &world = *static_cast<ReplicationWorldLifecycle *>(context);
            static_cast<void>(world.RetireObject(World().scene, World().session, Object().object));
        };
        REQUIRE(fixture.Capture(2, 4.0).failed == 1);
        REQUIRE_FALSE(prior->IsCurrent());
        REQUIRE(fixture.capture->Latest(Object().object).HasError());
        REQUIRE_FALSE(fixture.owner->reading);
    }

    TEST_CASE("Shutdown revokes snapshot pins and closes work before owner module release", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1, 2.0);
        auto pin = fixture.Pin();
        std::weak_ptr<const ICommittedReplicationSource> owner = fixture.owner;
        fixture.owner.reset();
        fixture.capture->Shutdown();
        fixture.capture->Shutdown();
        REQUIRE_FALSE(pin->IsCurrent());
        REQUIRE(fixture.capture->MarkDirty(Object().object).HasError());
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 2), 2).HasError());
        REQUIRE_FALSE(owner.expired());
        REQUIRE_FALSE(fixture.capture->CanReclaim());
        REQUIRE(std::get<double>(pin->Fields()[0].value) == 2.0);
        pin.reset();
        REQUIRE(fixture.capture->CanReclaim());
        fixture.capture.reset();  // Host quiescent reclamation releases owner pins.
        REQUIRE(owner.expired());
    }

    TEST_CASE("Capture rejects phase capability affinity and impossible per-target budgets", "[network][capture]") {
        Fixture fixture;
        auto work = ReplicationWorldWorkRequest{World().scene, World().session, Runtime::RuntimePhase::FixedUpdate, 1, {}};
        REQUIRE(fixture.lifecycle.AcquireCaptureRead(work).HasError());
        bool affinityRejected{};
        const auto ownerRead = Read(fixture.lifecycle);
        std::thread thread{[&] {
            affinityRejected = ownerRead.Resolve(Object().object).HasError();
        }};
        // Avoid cross-thread lifecycle access: only the immutable read is transferable.
        thread.join();
        REQUIRE(affinityRejected);
        ReplicationCaptureLimits limits;
        limits.maximumBytesPerTick = 1;
        const std::array targets{ReplicationCaptureTarget{Object(), fixture.owner}};
        REQUIRE(ReplicationStateCapture::Prepare(Read(fixture.lifecycle), fixture.registry, targets, limits).HasError());
    }

    TEST_CASE("Wrong missing duplicate and foreign fields reject a whole candidate", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1, 4.0);
        const auto baseline = fixture.Pin();
        const std::array faults{WriterFault::Missing, WriterFault::Duplicate, WriterFault::Foreign, WriterFault::WrongKind};
        std::uint64_t tick{2};
        for (const auto fault : faults) {
            fixture.owner->fault = fault;
            REQUIRE(fixture.Capture(tick++, 9.0).failed == 1);
            REQUIRE(fixture.Pin() == baseline);
            REQUIRE_FALSE(fixture.owner->reading);
        }
    }

    TEST_CASE("World replacement revokes every old source pin before another capture attempt", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1);
        const auto pin = fixture.Pin();
        const auto oldRead = Read(fixture.lifecycle);
        const auto replacement = World(2);
        REQUIRE(fixture.lifecycle.Stage(replacement).HasValue());
        REQUIRE(fixture.lifecycle.CommitAtSafePoint(replacement.scene, replacement.session).HasValue());
        REQUIRE_FALSE(pin->IsCurrent());
        REQUIRE_FALSE(oldRead.IsCurrent());
        REQUIRE(oldRead.Resolve(Object().object).HasError());
        REQUIRE(fixture.capture->Latest(Object().object).HasError());
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 2), 2).HasError());
    }

    TEST_CASE("Pinned codec metadata survives mutable contribution metadata and owner lifetime", "[network][capture]") {
        Fixture fixture;
        const auto metadata = fixture.registry->DescriptorFor(SchemaId(10), FieldIdValue(1));
        REQUIRE(metadata.HasValue());
        fixture.codec->descriptor.valueKind = ReplicationValueKind::UnsignedInteger;
        REQUIRE(metadata.Value()->valueKind == ReplicationValueKind::FloatingPoint);
        REQUIRE(fixture.registry->DescriptorFor(SchemaId(999), FieldIdValue(1)).HasError());
        REQUIRE(fixture.registry->DescriptorFor(SchemaId(10), FieldIdValue(999)).HasError());
        std::weak_ptr<const IReplicationFieldSerializer> codec = fixture.codec;
        fixture.codec.reset();
        fixture.registry.reset();
        REQUIRE_FALSE(codec.expired());
        REQUIRE(fixture.Capture(1).published == 1);
        fixture.capture->Shutdown();
        REQUIRE(fixture.capture->CanReclaim());
        fixture.capture.reset();
        REQUIRE(codec.expired());
    }

    TEST_CASE("Owner read generation changes inside capture cannot publish a stale candidate", "[network][capture]") {
        Fixture fixture;
        fixture.Capture(1);
        const auto prior = fixture.Pin();
        fixture.owner->context = fixture.owner.get();
        fixture.owner->onCapture = [](void *context) {
            static_cast<Owner *>(context)->committed = false;
        };
        REQUIRE(fixture.Capture(2, 8.0).failed == 1);
        REQUIRE(fixture.Pin() == prior);
        REQUIRE_FALSE(fixture.owner->reading);
    }

    TEST_CASE("Shutdown and external pin release do not reclaim pool storage on the hot path", "[network][capture][allocation]") {
        Fixture fixture;
        fixture.Capture(1);
        auto pin = fixture.Pin();
        const auto allocations = Tests::AllocationProbe::Count();
        const auto frees = Tests::AllocationProbe::FreeCount();
        fixture.capture->Shutdown();
        pin.reset();
        const auto after = Tests::AllocationProbe::Count();
        const auto reclaimed = Tests::AllocationProbe::FreeCount();
        REQUIRE(allocations == after);
        REQUIRE(frees == reclaimed);
        REQUIRE(fixture.capture->CanReclaim());
    }

    TEST_CASE("Pause denies new capture admission while preserving already admitted unchanged-world reads",
              "[network][capture][lifecycle]") {
        Fixture fixture;
        fixture.Capture(1, 1.0);
        const auto prior = fixture.Pin();
        const auto admitted = Read(fixture.lifecycle, 2);
        fixture.owner->Commit(2, 2.0);
        REQUIRE(fixture.lifecycle.Pause(World().scene, World().session).HasValue());
        const auto newRead =
            fixture.lifecycle.AcquireCaptureRead({World().scene, World().session, Runtime::RuntimePhase::NetworkFlush, 2, {}});
        REQUIRE(newRead.HasError());
        REQUIRE(admitted.IsCurrent());
        REQUIRE(prior->IsCurrent());
        REQUIRE(fixture.capture->CaptureAtCommit(admitted, 2).Value().published == 1);
        REQUIRE(prior->IsCurrent());
        REQUIRE(fixture.lifecycle.Resume(World().scene, World().session).HasValue());
        REQUIRE(fixture.Capture(3, 3.0).published == 1);
    }
}  // namespace Horo::Network

TEST_CASE("Replication debugger observes actual canonical commit success and failure reports", "[network][capture][debugger]") {
    using namespace Horo::Network;
    NetworkDebugger debugger;
    const NetworkDiagnosticSource source{1, 2, CaptureTestSupport::World().scene.value, 5};
    REQUIRE(debugger.Begin(source, true));
    CaptureTestSupport::Fixture fixture{{}, &debugger};
    REQUIRE(fixture.Capture(1, 4.0).published == 1);
    fixture.owner->fail = true;
    REQUIRE(fixture.Capture(2, 8.0).failed == 1);
    REQUIRE(debugger.Publish(source, 100));
    auto snapshot = debugger.Snapshot();
    REQUIRE(snapshot.replication.size == 2);
    REQUIRE(snapshot.replication.records[0].published == 1);
    REQUIRE(snapshot.replication.records[1].failed == 1);
    REQUIRE(snapshot.replication.records[1].considered == 1);
    REQUIRE(fixture.owner->begins == fixture.owner->ends);
    REQUIRE(fixture.Pin()->SimulationTick() == 1);
    debugger.Detach();
    REQUIRE(debugger.Begin({1, 3, source.scene, 6}, true));
    fixture.owner->fail = false;
    REQUIRE(fixture.Capture(3, 9.0).published == 1);
    REQUIRE(debugger.Publish(debugger.Source(), 101));
    REQUIRE(debugger.Snapshot().replication.size == 0);
}
