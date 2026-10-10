#include "../../../support/AllocationProbe.h"
#include "SaveAllocationDiagnostics.h"
#include "SaveSlotRetentionTestSupport.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::Runtime {
    using namespace SaveSlotRetentionTest;

    namespace {
        /** @brief Edits a sealed private fixture catalog without bypassing the production decoder on reopen. */
        void RewriteCatalog(Fixture &fixture, const std::function<void(nlohmann::json &)> &edit) {
            auto bytes = DiskBytes(fixture.Slots() / ".lifecycle.catalog");
            const std::string text(reinterpret_cast<const char *>(bytes.data() + 32), bytes.size() - 32);
            auto value = nlohmann::json::parse(text);
            edit(value);
            const auto encoded = value.dump();
            const auto body = std::as_bytes(std::span{encoded.data(), encoded.size()});
            const auto digest = ComputeSha256(body);
            bytes.clear();
            for (const auto byte : digest.bytes)
                bytes.push_back(static_cast<std::byte>(byte));
            bytes.insert(bytes.end(), body.begin(), body.end());
            WriteBytes(fixture.Slots() / ".lifecycle.catalog", bytes);
        }

        /** @brief Measures every real publication observer occurrence for exhaustive single-fault injection. */
        std::size_t PublicationOccurrences() {
            RetentionFixture probe;
            probe.Commit(1, 10);
            probe.Commit(2, 20);
            probe.Commit(3, 30);
            std::size_t occurrences = 0;
            probe.native.fault.action = [&occurrences](const auto, const auto) {
                ++occurrences;
            };
            probe.Commit(4, 40);
            return occurrences;
        }

        /** @brief Reopens the selected rotation and proves reconciliation preserves the exact backup and binding release. */
        void CheckRecoveredRetention(RetentionFixture &fixture, const SaveSlotCatalogEntry &oldest) {
            fixture.native.fault.failure = {};
            SaveAllocationPhase("[save-allocation] retention-reopen-enter\n");
            fixture.native.Reopen();
            SaveAllocationPhase("[save-allocation] retention-reopen-returned\n");
            SaveAllocationPhase("[save-allocation] retention-reconcile-enter\n");
            REQUIRE(fixture.native.owner->Reconcile(fixture.native.Access()).HasValue());
            SaveAllocationPhase("[save-allocation] retention-reconcile-returned\n");
            const auto snapshot = fixture.Snapshot();
            CHECK(snapshot.lastCommitMilliseconds == 40);
            CHECK(snapshot.selected.size() == 3);
            REQUIRE(snapshot.retained.size() == 1);
            CHECK(snapshot.retained[0].entry == oldest);
            CHECK(snapshot.retained[0].backup);
            CHECK(fixture.native.host.leases == 0);
        }
    }  // namespace

    TEST_CASE("Every real retention publication I/O occurrence preserves old or complete new rotation",
              "[runtime][save][retention][failure]") {
        const auto occurrences = PublicationOccurrences();
        REQUIRE(occurrences != 0);
        for (std::size_t failed = 0; failed < occurrences; ++failed) {
            INFO("Injected native occurrence " << failed);
            RetentionFixture fixture;
            const auto oldest = fixture.Commit(1, 10).entry.value();
            fixture.Commit(2, 20);
            fixture.Commit(3, 30);
            const auto before = fixture.Snapshot();
            std::size_t occurrence = 0;
            fixture.native.fault.action = [&fixture, &occurrence, failed](const auto stage, const auto kind) {
                CHECK(fixture.native.host.leases == 1);
                if (occurrence++ == failed)
                    fixture.native.InjectFailure(stage, kind);
            };
            auto result = fixture.native.owner->CommitSave(fixture.native.Target(4), fixture.Candidate(4), 40);
            CHECK(fixture.native.host.leases == 0);
            fixture.native.fault.action = {};
            const auto after = fixture.Snapshot();
            const bool published = after.catalogRevision != before.catalogRevision;
            if (!published) {
                REQUIRE(result.HasError());
                CHECK(after.lastCommitMilliseconds == 30);
                CHECK(after.retained.empty());
                CHECK(std::ranges::any_of(after.selected, [&oldest](const auto &row) {
                    return row.entry == oldest;
                }));
            } else {
                if (result.HasError())
                    CHECK(result.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
                CHECK(after.lastCommitMilliseconds == 40);
                REQUIRE(after.retained.size() == 1);
                CHECK(after.retained[0].entry == oldest);
                CHECK(after.retained[0].backup);
            }
            fixture.native.Reopen();
            auto reconciled = fixture.native.owner->Reconcile(fixture.native.Access());
            REQUIRE(reconciled.HasValue());
            CHECK_FALSE(reconciled.Value());
            CHECK(std::filesystem::exists(fixture.native.Generation(oldest.publication.generation)));
            CHECK(fixture.native.Index().entries.size() == 3);
        }
    }

    TEST_CASE("Cancelled retention save and semantic failure leave rotation unchanged", "[runtime][save][retention][failure]") {
        const bool cancelled = GENERATE(false, true);
        RetentionFixture fixture;
        fixture.Commit(1, 10);
        fixture.Commit(2, 20);
        fixture.Commit(3, 30);
        const auto before = fixture.Snapshot();
        CancellationSource source;
        if (cancelled)
            source.RequestCancellation();
        else
            fixture.native.host.semanticFailure = true;
        auto result = fixture.native.owner->CommitSave(fixture.native.Target(4), fixture.Candidate(4), 40, false, source.Token());
        REQUIRE(result.HasError());
        CHECK(fixture.native.host.leases == 0);
        CHECK(fixture.Snapshot().catalogRevision == before.catalogRevision);
        CHECK(fixture.Snapshot().retained.empty());
    }

    TEST_CASE("Retention allocation failure after selection preserves publication and binding ownership",
              "[runtime][save][retention][failure]") {
        const bool syncFailure = GENERATE(false, true);
        if (syncFailure)
            SaveAllocationPhase("[save-allocation] retention-sync-failure\n");
        else
            SaveAllocationPhase("[save-allocation] retention-sync-success\n");
        const SaveAllocationFixtureExit fixtureExit;
        RetentionFixture fixture;
        const auto oldest = fixture.Commit(1, 10).entry.value();
        fixture.Commit(2, 20);
        fixture.Commit(3, 30);
        const auto target = fixture.native.Target(4);
        auto candidate = fixture.Candidate(4);
        const auto cloud = fixture.Cloud();
        Error syncError = MakeError(SaveErrors::StoragePermanentIo);
        bool replaced{};
        std::optional<Horo::Tests::AllocationProbe::ScopedFailure> allocation;
        fixture.native.fault.failure = [&](const auto stage, const auto kind) {
            if (stage == SaveSlotLifecycleIoStage::Replace && kind == SaveSlotLifecycleFileKind::Catalog)
                replaced = true;
            if (replaced && !allocation && stage == SaveSlotLifecycleIoStage::DirectorySync && kind == SaveSlotLifecycleFileKind::Catalog) {
                SaveAllocationPhase("[save-allocation] retention-arm-enter\n");
                allocation.emplace(0, SaveAllocationInjected);
                SaveAllocationPhase("[save-allocation] retention-armed\n");
                if (syncFailure)
                    return Result<void>::Failure(std::move(syncError));
            }
            return Result<void>::Success();
        };
        SaveAllocationPhase("[save-allocation] retention-commit-enter\n");
        const auto result = fixture.native.owner->CommitSave(target, std::move(candidate), 40, false, {}, &cloud);
        SaveAllocationPhase("[save-allocation] retention-commit-returned\n");
        allocation.reset();
        SaveAllocationPhase("[save-allocation] retention-failure-reset\n");
        REQUIRE(replaced);
        CheckSaveAllocationPublication(result, syncFailure);
        CHECK(fixture.native.host.leases == 0);
        CHECK(std::filesystem::exists(fixture.native.Generation(oldest.publication.generation)));
        CheckRecoveredRetention(fixture, oldest);
        SaveAllocationPhase("[save-allocation] retention-fixture-cleanup-next\n");
    }

    TEST_CASE("Retention reads reject corrupted backups without modifying valid current publication",
              "[runtime][save][retention][failure]") {
        RetentionFixture fixture;
        for (std::uint8_t slot = 1; slot <= 4; ++slot)
            fixture.Commit(slot, slot * 10);
        const auto backup = fixture.Snapshot().retained[0];
        auto bytes = DiskBytes(fixture.native.Generation(backup.entry.publication.generation));
        bytes.back() ^= std::byte{1};
        WriteBytes(fixture.native.Generation(backup.entry.publication.generation), bytes);
        auto read = fixture.native.owner->ReadBackup(fixture.RetainedTarget(backup));
        REQUIRE(read.HasError());
        CHECK(fixture.native.host.leases == 0);
        CHECK(fixture.native.Index().entries.size() == 3);
        CHECK(DiskBytes(fixture.native.Generation(backup.entry.publication.generation)) == bytes);
    }

    TEST_CASE("Legacy lifecycle catalogs migrate with unknown retention order protected", "[runtime][save][retention][migration]") {
        RetentionFixture fixture;
        fixture.Commit(1, 10);
        fixture.Commit(2, 20);
        fixture.Commit(3, 30);
        RewriteCatalog(fixture.native, [](auto &value) {
            value[0] = 1U;
            value.erase(value.begin() + 5);
            for (auto &record : value[3])
                record.erase(record.begin() + 2, record.end());
            for (auto &record : value[4])
                record.erase(record.begin() + 2, record.end());
        });
        fixture.native.Reopen();
        CHECK(fixture.Snapshot().selected[0].sequence == 0);
        REQUIRE(fixture.native.owner->SetPinned(fixture.native.Target(1), true).HasValue());
        auto result = fixture.native.owner->CommitSave(fixture.native.Target(4), fixture.Candidate(4), 40);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == SaveErrors::StorageQuotaExceeded.code.Value());
        CHECK(fixture.native.Index().entries.size() == 3);
        CHECK(fixture.Snapshot().lastCommitMilliseconds == 0);
    }

    TEST_CASE("Malformed retention metadata cannot authorize cleanup", "[runtime][save][retention][migration]") {
        const int variant = GENERATE(0, 1, 2, 3);
        RetentionFixture fixture;
        fixture.Commit(1, 10);
        fixture.Commit(2, 20);
        const auto first = fixture.native.Index().entries[0].publication.generation;
        RewriteCatalog(fixture.native, [variant](auto &value) {
            if (variant == 0)
                value[3][0][2] = value[3][1][2];
            if (variant == 1)
                value[3][0][3] = 9999U;
            if (variant == 2)
                value[3][0][4] = "pinned";
            if (variant == 3)
                value[0] = 99U;
        });
        auto snapshot = fixture.native.owner->RetentionSnapshot(fixture.native.Access());
        REQUIRE(snapshot.HasError());
        REQUIRE(fixture.native.owner->Reconcile(fixture.native.Access()).HasError());
        CHECK(std::filesystem::exists(fixture.native.Generation(first)));
    }

    TEST_CASE("Retention policy rejects unsafe capacities before opening storage", "[runtime][save][retention][policy]") {
        const int variant = GENERATE(0, 1, 2, 3, 4);
        RetentionFixture fixture;
        fixture.native.owner.reset();
        auto &limits = fixture.native.policy.retention.kinds[0];
        if (variant == 0)
            limits.maximumSlots = 1;
        if (variant == 1)
            limits.lowSpaceSlots = 1;
        if (variant == 2)
            limits.minimumSlots = 0;
        if (variant == 3)
            limits.backupGenerations = 0;
        if (variant == 4)
            limits.backupGenerations = 65;
        auto opened = SaveSlotLifecycle::Open(fixture.native.root, fixture.native.policy, fixture.native.host);
        REQUIRE(opened.HasError());
        CHECK(opened.ErrorValue().code.Value() == SaveErrors::StoragePolicyInvalid.code.Value());
    }
}  // namespace Horo::Runtime
