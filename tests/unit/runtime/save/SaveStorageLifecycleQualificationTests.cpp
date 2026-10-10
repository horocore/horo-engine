#include "SaveSlotLifecycleTestSupport.h"

#include <algorithm>

using namespace Horo::Runtime::SaveSlotLifecycleTest;

namespace {
    struct StageOccurrence final {
        SaveSlotLifecycleIoStage stage;
        SaveSlotLifecycleFileKind kind;
        std::size_t ordinal;
    };

    /** @brief Records every actual stage occurrence, including selection durability and cleanup acknowledgements. */
    std::vector<StageOccurrence> CopyStages() {
        Fixture fixture;
        fixture.Import(10);
        fixture.Import(11);
        std::map<std::pair<SaveSlotLifecycleIoStage, SaveSlotLifecycleFileKind>, std::size_t> counts;
        std::vector<StageOccurrence> stages;
        fixture.fault.action = [&](const auto stage, const auto kind) {
            stages.push_back({stage, kind, ++counts[{stage, kind}]});
        };
        REQUIRE(fixture.owner->Execute(fixture.Copy(10, 11)).HasValue());
        return stages;
    }

    /** @brief Revalidates a retained physical generation through the production archive reader. */
    void CheckGeneration(const Fixture &fixture, const SlotGenerationId generation, const std::vector<std::byte> &expected) {
        const auto bytes = DiskBytes(fixture.Generation(generation));
        CHECK(bytes == expected);
        const auto archive = ReadOwned(bytes);
        CHECK(archive.Header().slotGeneration == generation);
    }

    /** @brief Leaves foreign artifacts in the same directory to detect overly broad recovery cleanup. */
    void CheckForeign(const Fixture &fixture, const std::vector<std::byte> &bytes) {
        CHECK(DiskBytes(fixture.Slots() / ".another-owner.temporary") == bytes);
        CHECK(DiskBytes(fixture.Generation(Id<SlotGenerationId>(240))) == bytes);
    }

    /** @brief Immutable evidence captured before an overwrite attempt. */
    struct CopySnapshot final {
        SaveSlotIndex index;
        SaveSlotCatalogEntry destination;
        std::vector<std::byte> sourceBytes;
        std::vector<std::byte> destinationBytes;
    };

    /** @brief Checks unchanged selection or honest unknown/deferred publication against the pre-operation evidence. */
    void CheckCopyOutcome(const Fixture &fixture, const CopySnapshot &before, const Result<SaveSlotLifecycleResult> &attempted) {
        const auto selected = fixture.Index();
        if (selected.revision == before.index.revision) {
            REQUIRE(attempted.HasError());
            CHECK(selected.entries == before.index.entries);
            CheckGeneration(fixture, before.destination.publication.generation, before.destinationBytes);
        } else {
            CHECK(selected.revision == before.index.revision + 1);
            if (attempted.HasError()) {
                CHECK(attempted.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
                CheckGeneration(fixture, before.destination.publication.generation, before.destinationBytes);
            } else {
                CHECK(attempted.Value().cleanupDeferred);
            }
            const auto archive = ReadOwned(fixture.ExportBytes(11));
            CHECK(archive.Manifest() == ReadOwned(before.sourceBytes).Manifest());
            CHECK(archive.Header().parentGeneration == before.destination.publication.generation);
        }
    }
}  // namespace

TEST_CASE("Every real copy I/O occurrence preserves selected validity and limits restart cleanup to owned artifacts",
          "[runtime][save][qualification][lifecycle][failure]") {
    const auto stages = CopyStages();
    REQUIRE_FALSE(stages.empty());
    for (const auto failure : stages) {
        INFO("stage " << static_cast<int>(failure.stage) << " kind " << static_cast<int>(failure.kind) << " occurrence "
                      << failure.ordinal);
        Fixture fixture;
        const auto source = fixture.Import(10);
        const auto destination = fixture.Import(11);
        const auto originalSource = fixture.ExportBytes(10);
        const auto originalDestination = fixture.ExportBytes(11);
        const CopySnapshot before{fixture.Index(), *destination.entry, originalSource, originalDestination};
        const std::vector foreign{std::byte{21}, std::byte{22}};
        WriteBytes(fixture.Slots() / ".another-owner.temporary", foreign);
        WriteBytes(fixture.Generation(Id<SlotGenerationId>(240)), foreign);
        std::size_t occurrence = 0;
        fixture.fault.failStage = failure.stage;
        fixture.fault.failKind = failure.kind;
        fixture.fault.action = [&](const auto stage, const auto kind) {
            if (stage == failure.stage && kind == failure.kind && ++occurrence == failure.ordinal)
                fixture.fault.enabled = true;
        };
        const auto attempted = fixture.owner->Execute(fixture.Copy(10, 11));
        REQUIRE(occurrence >= failure.ordinal);
        CHECK_FALSE(fixture.fault.enabled);
        fixture.fault.action = {};
        const auto selected = fixture.Index();
        CheckGeneration(fixture, source.entry->publication.generation, originalSource);
        CheckForeign(fixture, foreign);
        CheckCopyOutcome(fixture, before, attempted);
        const auto selectedBytes = fixture.ExportBytes(11);
        fixture.Reopen();
        CHECK(fixture.Index().revision == selected.revision);
        CHECK(fixture.Index().entries == selected.entries);
        for (int retry = 0; retry < 2; ++retry) {
            auto reconciled = fixture.owner->Reconcile(fixture.Access());
            REQUIRE(reconciled.HasValue());
            CHECK_FALSE(reconciled.Value());
            CHECK(fixture.ExportBytes(10) == originalSource);
            CHECK(fixture.ExportBytes(11) == selectedBytes);
            CheckForeign(fixture, foreign);
        }
    }
}

TEST_CASE("Metadata mutation outcome unknown retains last-known-good bytes until restart establishes durability",
          "[runtime][save][qualification][lifecycle][failure]") {
    const auto kind = GENERATE(SaveSlotLifecycleKind::Rename, SaveSlotLifecycleKind::Delete);
    Fixture fixture;
    const auto imported = fixture.Import(10);
    fixture.Import(11);
    const auto original = fixture.ExportBytes(10);
    const auto unrelated = fixture.ExportBytes(11);
    const auto before = fixture.Index();
    bool replacing = false;
    fixture.fault.failStage = SaveSlotLifecycleIoStage::DirectorySync;
    fixture.fault.failKind = SaveSlotLifecycleFileKind::Catalog;
    fixture.fault.action = [&](const auto stage, const auto fileKind) {
        if (fileKind != SaveSlotLifecycleFileKind::Catalog)
            return;
        if (stage == SaveSlotLifecycleIoStage::Replace)
            replacing = true;
        if (stage == SaveSlotLifecycleIoStage::DirectorySync && replacing)
            fixture.fault.enabled = true;
    };
    SaveSlotLifecycleRequest request{.kind = kind, .source = fixture.Target(10)};
    if (kind == SaveSlotLifecycleKind::Rename)
        request.display = {"renamed ü"};
    else
        request.deleteMode = SaveSlotDeleteMode::Permanent;
    const auto failed = fixture.owner->Execute(request);
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
    fixture.fault.action = {};
    CheckGeneration(fixture, imported.entry->publication.generation, original);
    fixture.Reopen();
    CHECK(fixture.Index().revision == before.revision + 1);
    CHECK(fixture.ExportBytes(11) == unrelated);
    if (kind == SaveSlotLifecycleKind::Rename) {
        CHECK(fixture.ExportBytes(10) == original);
        CHECK(fixture.Index().entries.front().display.displayName == "renamed ü");
    } else {
        CHECK_FALSE(fixture.Target(10).generation);
    }
    for (int retry = 0; retry < 2; ++retry) {
        auto reconciled = fixture.owner->Reconcile(fixture.Access());
        REQUIRE(reconciled.HasValue());
        CHECK_FALSE(reconciled.Value());
        CHECK(fixture.Index().revision == before.revision + 1);
    }
    CHECK(std::filesystem::exists(NativeProbePath(fixture.Generation(imported.entry->publication.generation))) ==
          (kind == SaveSlotLifecycleKind::Rename));
}

TEST_CASE("External export is an independent complete archive while internal cleanup is deferred",
          "[runtime][save][qualification][lifecycle]") {
    Fixture fixture;
    fixture.Import(10, MakeArchiveWithUnknown(false).bytes);
    fixture.Import(11);
    fixture.InjectFailure(SaveSlotLifecycleIoStage::Remove, SaveSlotLifecycleFileKind::Journal);
    const auto copied = fixture.owner->Execute(fixture.Copy(10, 11));
    REQUIRE(copied.HasValue());
    REQUIRE(copied.Value().cleanupDeferred);
    REQUIRE(std::filesystem::exists(NativeProbePath(fixture.Slots() / ".lifecycle.journal")));
    const auto immutable = fixture.owner->Execute(fixture.Export(11));
    REQUIRE(immutable.HasValue());
    const auto snapshot = *immutable.Value().exported.bytes;
    auto external = Namespace();
    external.environment = Id<EnvironmentStorageId>(50);
    auto opened = SaveFilesystemStorage::Open(fixture.root, external);
    REQUIRE(opened.HasValue());
    auto destination = std::move(opened).Value();
    const auto slot = Id<SaveGameSlotId>(60);
    REQUIRE(fixture.owner->ExportTo(fixture.Export(11), destination, slot).HasValue());
    const auto read = destination.Read(slot, snapshot.size());
    REQUIRE(read.HasValue());
    CHECK(read.Value() == snapshot);
    const auto archive = ReadOwned(read.Value());
    const auto source = ReadOwned(fixture.ExportBytes(10));
    CHECK(archive.Manifest() == source.Manifest());
    REQUIRE(archive.Directory().Entries().size() == source.Directory().Entries().size());
    for (std::size_t index = 0; index < source.Directory().Entries().size(); ++index) {
        const auto &entry = source.Directory().Entries()[index];
        const auto &exported = archive.Directory().Entries()[index];
        auto expected = entry;
        expected.offset = exported.offset;
        CHECK(expected == exported);
        CHECK(std::ranges::equal(source.Payload().subspan(entry.offset, entry.storedByteLength),
                                 archive.Payload().subspan(exported.offset, exported.storedByteLength)));
    }
    fixture.Import(11);
    CHECK(*immutable.Value().exported.bytes == snapshot);
    CHECK(destination.Read(slot, snapshot.size()).Value() == snapshot);
}
