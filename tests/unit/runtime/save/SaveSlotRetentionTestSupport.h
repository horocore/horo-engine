#pragma once

#include "SaveSlotLifecycleTestSupport.h"

namespace Horo::Runtime::SaveSlotRetentionTest {
    using namespace SaveSlotLifecycleTest;

    /** @brief Native contained-storage fixture using the same authoritative lifecycle publication path. */
    struct RetentionFixture final {
        Fixture native;
        std::uint8_t next{180};

        RetentionFixture() {
            native.owner.reset();
            native.policy.retention.enabled = true;
            native.policy.retention.kinds[0].maximumSlots = 3;
            native.policy.retention.kinds[1].maximumSlots = 3;
            native.Open();
        }

        SaveStorageWrite Candidate(const std::uint8_t slot, const SaveSlotKind kind = SaveSlotKind::Auto) {
            auto target = native.Target(slot);
            auto source = ReadOwned(MakeArchive().bytes);
            auto header = source.Header();
            header.product = Namespace().product;
            header.environment = Namespace().environment;
            header.user = Id<LocalUserStorageId>(5);
            header.profile = Id<GameProfileId>(6);
            header.slot = target.address.slot;
            header.slotGeneration = Id<SlotGenerationId>(next++);
            header.parentGeneration = target.generation;
            // Archive presentation time deliberately differs from retention time/order.
            header.capturedAtUnixMilliseconds = 1;
            auto bytes = RewriteHeader(source, header);
            auto archive = ReadOwned(bytes);
            SaveSlotCatalogEntry entry{.publication = {.slot = header.slot,
                                                       .generation = header.slotGeneration,
                                                       .kind = kind,
                                                       .savedAtUnixMilliseconds = header.capturedAtUnixMilliseconds,
                                                       .playTimeNanoseconds = header.playTimeNanoseconds,
                                                       .baseScene = header.baseScene,
                                                       .productCompatibility = header.productCompatibility,
                                                       .saveSchema = archive.Manifest().saveSchemaVersion,
                                                       .projectBuildId = header.projectBuildId,
                                                       .canonicalState = archive.Manifest().canonicalState,
                                                       .archiveContent = archive.Integrity().archiveContent},
                                       .display = {"Retention ü"}};
            return {std::move(entry), {std::make_shared<const std::vector<std::byte>>(std::move(bytes))}};
        }

        SaveCloudMetadataScope CloudScope() const {
            return {Namespace(), Id<SaveCloudProviderId>(10), Id<SaveCloudAccountId>(11)};
        }

        SaveCloudRevisionSnapshot Cloud() const {
            auto index = native.Index();
            auto metadata = ReconcileSaveCloudRevisionMetadata(index, CloudScope(), {}, 1);
            REQUIRE(metadata.HasValue());
            auto candidate = std::move(metadata).Value();
            for (auto &record : candidate.records) {
                record.state = SaveCloudGenerationState::Clean;
                record.object = SaveCloudObjectRef{{std::byte{1}, std::byte{2}}, std::vector{std::byte{3}, std::byte{4}}};
            }
            auto snapshot = SaveCloudRevisionSnapshot::Create(index, CloudScope(), std::move(candidate));
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        SaveSlotLifecycleResult Commit(const std::uint8_t slot, const std::uint64_t clock, const SaveSlotKind kind = SaveSlotKind::Auto,
                                       const bool lowSpace = false) {
            const auto target = native.Target(slot);
            auto cloud = Cloud();
            auto result = native.owner->CommitSave(target, Candidate(slot, kind), clock, lowSpace, {}, &cloud);
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Committed"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        SaveSlotRetentionSnapshot Snapshot() const {
            auto value = native.owner->RetentionSnapshot(native.Access());
            REQUIRE(value.HasValue());
            return std::move(value).Value();
        }

        SaveSlotLifecycleTarget RetainedTarget(const SaveSlotRetentionRecord &record) const {
            return {{native.Access(), record.entry.publication.slot}, Snapshot().catalogRevision, record.entry.publication.generation};
        }
    };
}  // namespace Horo::Runtime::SaveSlotRetentionTest
