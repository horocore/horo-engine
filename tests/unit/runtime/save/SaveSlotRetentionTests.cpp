#include "SaveSlotRetentionTestSupport.h"

#include <algorithm>

namespace Horo::Runtime {
    using namespace SaveSlotRetentionTest;

    TEST_CASE("Retention publishes bounded rotation and backup evidence through the real catalog", "[runtime][save][retention]") {
        RetentionFixture fixture;
        const auto first = fixture.Commit(1, 100).entry.value();
        fixture.Commit(2, 200);
        fixture.Commit(3, 300);
        const auto fourth = fixture.Commit(4, 400);
        REQUIRE(fourth.retention.size() == 1);
        CHECK(fourth.retention[0].entry == first);
        CHECK(fourth.retention[0].reason == SaveSlotRetentionReason::Capacity);
        fixture.native.Reopen();
        const auto snapshot = fixture.Snapshot();
        REQUIRE(snapshot.selected.size() == 3);
        REQUIRE(snapshot.retained.size() == 1);
        CHECK(snapshot.lastCommitMilliseconds == 400);
        CHECK(snapshot.retained[0].backup);
        auto backup = fixture.native.owner->ReadBackup(fixture.RetainedTarget(snapshot.retained[0]));
        REQUIRE(backup.HasValue());
        auto archive = ReadOwned(*backup.Value().bytes);
        CHECK(archive.Header().slotGeneration == first.publication.generation);
        CHECK(archive.Directory().Entries().size() > 0);
    }

    TEST_CASE("Retention cannot prune manual slots or pinned autosaves", "[runtime][save][retention]") {
        RetentionFixture fixture;
        const auto manual = fixture.Commit(9, 10, SaveSlotKind::Manual).entry;
        const auto pinned = fixture.Commit(1, 20).entry;
        REQUIRE(fixture.native.owner->SetPinned(fixture.native.Target(1), true).HasValue());
        fixture.Commit(2, 30);
        fixture.Commit(3, 40);
        fixture.Commit(4, 50);
        fixture.native.Reopen();
        const auto index = fixture.native.Index();
        CHECK(std::ranges::find(index.entries, *manual) != index.entries.end());
        CHECK(std::ranges::find(index.entries, *pinned) != index.entries.end());
        const auto snapshot = fixture.Snapshot();
        REQUIRE(snapshot.retained.size() == 1);
        CHECK(snapshot.retained[0].entry.publication.slot == Id<SaveGameSlotId>(2));
    }

    TEST_CASE("Pinned capacity fails closed without consuming rotation or time", "[runtime][save][retention]") {
        RetentionFixture fixture;
        for (std::uint8_t slot = 1; slot <= 3; ++slot) {
            fixture.Commit(slot, slot);
            REQUIRE(fixture.native.owner->SetPinned(fixture.native.Target(slot), true).HasValue());
        }
        const auto before = fixture.Snapshot();
        auto result = fixture.native.owner->CommitSave(fixture.native.Target(4), fixture.Candidate(4), 100);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == SaveErrors::StorageQuotaExceeded.code.Value());
        const auto after = fixture.Snapshot();
        CHECK(after.catalogRevision == before.catalogRevision);
        CHECK(after.lastCommitMilliseconds == before.lastCommitMilliseconds);
        CHECK(after.retained.empty());
    }

    TEST_CASE("Age and low-space retirement use commit order with independent category budgets", "[runtime][save][retention]") {
        const bool lowSpace = GENERATE(false, true);
        RetentionFixture fixture;
        fixture.native.owner.reset();
        fixture.native.policy.retention.kinds[0].maximumAgeMilliseconds = lowSpace ? 0 : 100;
        fixture.native.Open();
        fixture.Commit(8, 1, SaveSlotKind::Checkpoint);
        fixture.Commit(1, 10);
        fixture.Commit(2, 20);
        const auto result = fixture.Commit(3, 200, SaveSlotKind::Auto, lowSpace);
        REQUIRE(!result.retention.empty());
        CHECK(result.retention[0].entry.publication.slot == Id<SaveGameSlotId>(1));
        CHECK(result.retention[0].reason == (lowSpace ? SaveSlotRetentionReason::LowSpace : SaveSlotRetentionReason::Age));
        const auto selected = fixture.Snapshot().selected;
        CHECK(std::ranges::any_of(selected, [](const auto &record) {
            return record.entry.publication.slot == Id<SaveGameSlotId>(8);
        }));
        CHECK(std::ranges::any_of(selected, [](const auto &record) {
            return record.entry.publication.slot == Id<SaveGameSlotId>(3);
        }));
    }

    TEST_CASE("Automatic replacement selects oldest eligible and never newest or another kind", "[runtime][save][retention]") {
        RetentionFixture fixture;
        fixture.Commit(1, 10);
        fixture.Commit(2, 20);
        auto newest = fixture.native.owner->CommitSave(fixture.native.Target(2), fixture.Candidate(2), 30);
        REQUIRE(newest.HasError());
        CHECK(newest.ErrorValue().code.Value() == SaveErrors::StoragePermissionDenied.code.Value());
        auto manual = fixture.native.owner->CommitSave(fixture.native.Target(1), fixture.Candidate(1, SaveSlotKind::Manual), 30);
        REQUIRE(manual.HasError());
        const auto replaced = fixture.Commit(1, 30);
        REQUIRE(replaced.retention.size() == 1);
        CHECK(replaced.retention[0].reason == SaveSlotRetentionReason::Replacement);
        CHECK(fixture.native.Index().entries.size() == 2);
    }

    TEST_CASE("Retention preserves explicit tombstones independently of backup limits", "[runtime][save][retention]") {
        RetentionFixture fixture;
        fixture.native.owner.reset();
        fixture.native.policy.retention.cloudTombstones = true;
        fixture.native.Open();
        for (std::uint8_t slot = 1; slot <= 5; ++slot)
            fixture.Commit(slot, slot * 10);
        fixture.native.Reopen();
        auto snapshot = fixture.Snapshot();
        REQUIRE(snapshot.retained.size() == 2);
        CHECK(snapshot.retained[0].pendingCloudDelete);
        CHECK_FALSE(snapshot.retained[0].backup);
        const auto old = snapshot.retained[0];
        CHECK(std::filesystem::exists(fixture.native.Generation(old.entry.publication.generation)));
        auto invalid = fixture.native.owner->AcknowledgeCloudDelete(fixture.RetainedTarget(old), fixture.CloudScope(), {});
        REQUIRE(invalid.HasError());
        auto acknowledged =
            fixture.native.owner->AcknowledgeCloudDelete(fixture.RetainedTarget(old), fixture.CloudScope(), Id<SaveCloudMutationId>(4));
        REQUIRE(acknowledged.HasValue());
        REQUIRE(fixture.native.owner->Reconcile(fixture.native.Access()).HasValue());
        CHECK_FALSE(std::filesystem::exists(fixture.native.Generation(old.entry.publication.generation)));
        fixture.native.Reopen();
        snapshot = fixture.Snapshot();
        REQUIRE(snapshot.retained.size() == 1);
        CHECK(snapshot.retained[0].pendingCloudDelete);
        CHECK(snapshot.retained[0].backup);
        REQUIRE(fixture.native.owner
                    ->AcknowledgeCloudDelete(fixture.RetainedTarget(snapshot.retained[0]), fixture.CloudScope(), Id<SaveCloudMutationId>(5))
                    .HasValue());
        REQUIRE(fixture.native.owner->Reconcile(fixture.native.Access()).HasValue());
        REQUIRE(fixture.native.owner->ReadBackup(fixture.RetainedTarget(fixture.Snapshot().retained[0])).HasValue());
    }

    TEST_CASE("Retention rejects stale consent regressing clocks and contradictory archives", "[runtime][save][retention]") {
        const int variant = GENERATE(0, 1, 2, 3, 4);
        RetentionFixture fixture;
        fixture.Commit(1, 100);
        auto target = fixture.native.Target(2);
        auto candidate = fixture.Candidate(2);
        std::uint64_t clock = 200;
        if (variant == 0)
            --target.catalogRevision;
        if (variant == 1)
            --target.address.namespaceAccess.expectedRevision;
        if (variant == 2)
            clock = 99;
        if (variant == 3)
            candidate.metadata.publication.generation = Id<SlotGenerationId>(20);
        if (variant == 4)
            candidate.metadata.publication.kind = static_cast<SaveSlotKind>(255);
        auto result = fixture.native.owner->CommitSave(target, std::move(candidate), clock);
        REQUIRE(result.HasError());
        CHECK(fixture.Snapshot().selected.size() == 1);
        CHECK(fixture.Snapshot().lastCommitMilliseconds == 100);
    }

    TEST_CASE("Retention pin and cloud acknowledgement enforce binding and catalog consent", "[runtime][save][retention]") {
        RetentionFixture fixture;
        fixture.Commit(1, 10);
        auto stale = fixture.native.Target(1);
        fixture.Commit(2, 20);
        REQUIRE(fixture.native.owner->SetPinned(stale, true).HasError());
        auto unavailable = fixture.native.Target(1);
        fixture.native.host.binding.state = SaveNamespaceBindingState::ProfileDeleting;
        REQUIRE(fixture.native.owner->SetPinned(unavailable, true).HasError());
        REQUIRE(fixture.native.owner->RetentionSnapshot(fixture.native.Access()).HasError());
    }

    TEST_CASE("Retention default policy and missing capabilities deny automatic publication", "[runtime][save][retention]") {
        const int variant = GENERATE(0, 1, 2);
        RetentionFixture fixture;
        fixture.native.owner.reset();
        if (variant == 0)
            fixture.native.policy.retention.enabled = false;
        if (variant == 1)
            fixture.native.policy.capabilities &= ~(1U << static_cast<unsigned>(SaveSlotLifecycleKind::Retention));
        if (variant == 2)
            fixture.native.policy.capabilities &= ~(1U << static_cast<unsigned>(SaveSlotLifecycleKind::PublishSave));
        fixture.native.Open();
        auto result = fixture.native.owner->CommitSave(fixture.native.Target(1), fixture.Candidate(1), 10);
        REQUIRE(result.HasError());
        CHECK(fixture.native.Index().entries.empty());
    }

    TEST_CASE("Pending cloud tombstone saturation rejects a save before any rotation publication", "[runtime][save][retention]") {
        RetentionFixture fixture;
        fixture.native.owner.reset();
        fixture.native.policy.maximumSlots = 4;
        fixture.native.policy.retention.cloudTombstones = true;
        fixture.native.Open();
        for (std::uint8_t slot = 1; slot <= 7; ++slot)
            fixture.Commit(slot, slot * 10);
        const auto before = fixture.Snapshot();
        REQUIRE(before.retained.size() == 4);
        auto cloud = fixture.Cloud();
        auto result = fixture.native.owner->CommitSave(fixture.native.Target(8), fixture.Candidate(8), 80, false, {}, &cloud);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == SaveErrors::StorageQuotaExceeded.code.Value());
        const auto after = fixture.Snapshot();
        CHECK(after.catalogRevision == before.catalogRevision);
        CHECK(after.lastCommitMilliseconds == before.lastCommitMilliseconds);
        CHECK(after.retained.size() == before.retained.size());
    }

    TEST_CASE("Unknown cloud acknowledgement preserves backup and reconciles durable explicit evidence", "[runtime][save][retention]") {
        RetentionFixture fixture;
        fixture.native.owner.reset();
        fixture.native.policy.retention.cloudTombstones = true;
        fixture.native.Open();
        for (std::uint8_t slot = 1; slot <= 4; ++slot)
            fixture.Commit(slot, slot * 10);
        const auto retained = fixture.Snapshot().retained[0];
        // The retained backup/cloud hold prevents preflight cleanup from republishing the catalog.
        std::size_t syncs = 0;
        fixture.native.fault.action = [&fixture, &syncs](const auto stage, const auto kind) {
            if (stage == SaveSlotLifecycleIoStage::DirectorySync && kind == SaveSlotLifecycleFileKind::Catalog && ++syncs == 1)
                fixture.native.InjectFailure(stage, kind);
        };
        auto acknowledged = fixture.native.owner->AcknowledgeCloudDelete(fixture.RetainedTarget(retained), fixture.CloudScope(),
                                                                         Id<SaveCloudMutationId>(7));
        fixture.native.fault.action = {};
        REQUIRE(syncs == 1);
        REQUIRE(acknowledged.HasError());
        CHECK(acknowledged.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
        fixture.native.Reopen();
        REQUIRE(fixture.native.owner->Reconcile(fixture.native.Access()).HasValue());
        const auto snapshot = fixture.Snapshot();
        REQUIRE(snapshot.retained.size() == 1);
        CHECK(snapshot.retained[0].backup);
        CHECK_FALSE(snapshot.retained[0].pendingCloudDelete);
        REQUIRE(fixture.native.owner->ReadBackup(fixture.RetainedTarget(snapshot.retained[0])).HasValue());
    }

    TEST_CASE("Cloud retirement requires exact current provider evidence and preserves CAS across restart",
              "[runtime][save][retention][cloud]") {
        const int variant = GENERATE(0, 1, 2);
        RetentionFixture fixture;
        fixture.native.owner.reset();
        fixture.native.policy.retention.cloudTombstones = true;
        fixture.native.Open();
        fixture.Commit(1, 10);
        fixture.Commit(2, 20);
        auto staleCloud = fixture.Cloud();
        fixture.Commit(3, 30);
        auto current = fixture.Cloud();
        const SaveCloudRevisionSnapshot *evidence = variant == 0 ? nullptr : &staleCloud;
        std::optional<SaveCloudRevisionSnapshot> foreign;
        if (variant == 2) {
            auto scope = fixture.CloudScope();
            scope.localNamespace.product = Id<ProductStorageId>(99);
            auto metadata = current.Metadata();
            metadata.scope = scope;
            auto created = SaveCloudRevisionSnapshot::Create(current.Index(), scope, std::move(metadata));
            REQUIRE(created.HasValue());
            foreign.emplace(std::move(created).Value());
            evidence = &*foreign;
        }
        const auto before = fixture.Snapshot();
        auto denied = fixture.native.owner->CommitSave(fixture.native.Target(4), fixture.Candidate(4), 40, false, {}, evidence);
        REQUIRE(denied.HasError());
        CHECK(fixture.Snapshot().catalogRevision == before.catalogRevision);
        CHECK(fixture.Snapshot().retained.empty());
        fixture.Commit(4, 40);
        fixture.native.Reopen();
        const auto retained = fixture.Snapshot().retained[0];
        REQUIRE(retained.cloud.has_value());
        CHECK(retained.cloud->scope == fixture.CloudScope());
        CHECK(retained.cloud->generation == current.Metadata().records[0]);
        auto oldConsent = fixture.RetainedTarget(retained);
        fixture.Commit(5, 50);
        REQUIRE(fixture.native.owner->AcknowledgeCloudDelete(oldConsent, fixture.CloudScope(), Id<SaveCloudMutationId>(8)).HasError());
        auto foreignScope = fixture.CloudScope();
        foreignScope.account = Id<SaveCloudAccountId>(99);
        REQUIRE(fixture.native.owner->AcknowledgeCloudDelete(fixture.RetainedTarget(retained), foreignScope, Id<SaveCloudMutationId>(8))
                    .HasError());
        CHECK(std::filesystem::exists(fixture.native.Generation(retained.entry.publication.generation)));
    }
}  // namespace Horo::Runtime
