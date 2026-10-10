#include "SaveSlotLifecycleTestSupport.h"

using namespace Horo::Runtime::SaveSlotLifecycleTest;

namespace {
    struct SelectedState final {
        SaveSlotIndex index;
        std::vector<std::byte> source;
        std::vector<std::byte> destination;
        std::vector<std::byte> catalog;
    };

    SelectedState Before(const Fixture &fixture) {
        return {fixture.Index(), fixture.ExportBytes(10), fixture.ExportBytes(11), DiskBytes(fixture.Slots() / ".lifecycle.catalog")};
    }

    void Unchanged(const Fixture &fixture, const SelectedState &before) {
        CHECK(fixture.Index().revision == before.index.revision);
        CHECK(fixture.Index().entries == before.index.entries);
        CHECK(fixture.ExportBytes(10) == before.source);
        CHECK(fixture.ExportBytes(11) == before.destination);
        CHECK(DiskBytes(fixture.Slots() / ".lifecycle.catalog") == before.catalog);
        for (const auto &entry : before.index.entries)
            CHECK(std::filesystem::exists(fixture.Generation(entry.publication.generation)));
    }

    /** @brief Rejects one untrusted import while verifying both physical selected generations remain exact. */
    void RejectedImport(Fixture &fixture, const std::vector<std::byte> &bytes, const SelectedState &before) {
        auto failed = fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Import,
                                              .source = fixture.Target(11),
                                              .imported = {std::make_shared<const std::vector<std::byte>>(bytes)},
                                              .importSource = 0});
        REQUIRE(failed.HasError());
        fixture.host.semanticFailure = false;
        if (fixture.policy.archiveLimits.maximumArchiveBytes == 100) {
            fixture.owner.reset();
            fixture.policy.archiveLimits.maximumArchiveBytes = 4ULL << 30U;
            fixture.Open();
        }
        Unchanged(fixture, before);
    }

    void NoTemporaries(const Fixture &fixture) {
        for (const auto &entry : std::filesystem::directory_iterator(fixture.Slots()))
            CHECK_FALSE(entry.path().filename().string().ends_with(".temporary"));
    }
}  // namespace

TEST_CASE("Real lifecycle write disk-full flush and replacement failures preserve source destination and catalog",
          "[runtime][save][lifecycle][failure]") {
    const auto kind =
        GENERATE(SaveSlotLifecycleFileKind::Journal, SaveSlotLifecycleFileKind::Generation, SaveSlotLifecycleFileKind::Catalog);
    const auto stage = GENERATE(SaveSlotLifecycleIoStage::Write, SaveSlotLifecycleIoStage::WriteProgress,
                                SaveSlotLifecycleIoStage::FileSync, SaveSlotLifecycleIoStage::Replace);
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto next = Id<SlotGenerationId>(fixture.host.nextGeneration);
    fixture.InjectFailure(stage, kind);
    auto failed = fixture.owner->Execute(fixture.Copy(10, 11));
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() != SaveErrors::SlotCommitOutcomeUnknown.code.Value());
    Unchanged(fixture, before);
    NoTemporaries(fixture);
    fixture.Reopen();
    Unchanged(fixture, before);
    REQUIRE(fixture.owner->Reconcile(fixture.Access()).HasValue());
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    Unchanged(fixture, before);
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(next)));
    NoTemporaries(fixture);
}

TEST_CASE("Disk-full after actual partial native write never exposes a partial destination", "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    auto source = ReadOwned(MakeArchive().bytes);
    auto entry = source.Directory().Entries().front();
    std::vector<std::byte> data(256ULL << 10U, std::byte{7});
    entry.storedByteLength = data.size();
    entry.decodedByteLength = data.size();
    entry.decodedHash = ComputeSha256(data);
    const std::vector chunks{PreservedSaveChunk{entry, data}};
    const auto written = SaveArchiveContainerWriter::Write(source.Header(), source.Manifest(), chunks, V<ArchiveFormatVersion>(1));
    REQUIRE(written.HasValue());
    fixture.Import(10, {written.Value().Bytes().begin(), written.Value().Bytes().end()});
    fixture.Import(11);
    const auto before = Before(fixture);
    bool partialObserved = false;
    fixture.InjectFailure(SaveSlotLifecycleIoStage::WriteProgress, SaveSlotLifecycleFileKind::Generation);
    fixture.fault.action = [&](const auto stage, const auto kind) {
        if (stage != SaveSlotLifecycleIoStage::WriteProgress || kind != SaveSlotLifecycleFileKind::Generation)
            return;
        for (const auto &file : std::filesystem::directory_iterator(fixture.Slots())) {
            if (file.path().filename().string().ends_with(".temporary")) {
                const auto size = std::filesystem::file_size(file.path());
                partialObserved = size > 0 && size < before.source.size();
            }
        }
    };
    auto failed = fixture.owner->Execute(fixture.Copy(10, 11));
    REQUIRE(failed.HasError());
    CHECK(partialObserved);
    CHECK(failed.ErrorValue().code.Value() == SaveErrors::StorageDiskFull.code.Value());
    Unchanged(fixture, before);
    NoTemporaries(fixture);
    fixture.fault.action = {};
    fixture.Reopen();
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    Unchanged(fixture, before);
}

TEST_CASE("Delete and rename prepublication filesystem failures preserve exact selected state", "[runtime][save][lifecycle][failure]") {
    const auto kind = GENERATE(SaveSlotLifecycleKind::Delete, SaveSlotLifecycleKind::Rename);
    const auto stage = GENERATE(SaveSlotLifecycleIoStage::Write, SaveSlotLifecycleIoStage::FileSync, SaveSlotLifecycleIoStage::Replace);
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    fixture.InjectFailure(stage, SaveSlotLifecycleFileKind::Catalog);
    SaveSlotLifecycleRequest request{.kind = kind, .source = fixture.Target(10)};
    if (kind == SaveSlotLifecycleKind::Rename)
        request.display = {"New name"};
    else
        request.deleteMode = SaveSlotDeleteMode::Permanent;
    REQUIRE(fixture.owner->Execute(request).HasError());
    Unchanged(fixture, before);
    fixture.Reopen();
    Unchanged(fixture, before);
}

TEST_CASE("Retirement failure stays committed and bounded idempotent cleanup survives restart", "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    const auto old = fixture.Import(11);
    const auto original = fixture.ExportBytes(10);
    fixture.InjectFailure(SaveSlotLifecycleIoStage::Remove, SaveSlotLifecycleFileKind::Generation);
    auto copied = fixture.owner->Execute(fixture.Copy(10, 11));
    REQUIRE(copied.HasValue());
    CHECK(copied.Value().cleanupDeferred);
    CHECK(std::filesystem::exists(fixture.Generation(old.entry->publication.generation)));
    fixture.Reopen();
    CHECK(fixture.Target(11).generation == copied.Value().entry->publication.generation);
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(old.entry->publication.generation)));
    CHECK(fixture.ExportBytes(10) == original);
}

TEST_CASE("Generation and journal post-replace sync failure leaves selected slots unchanged and recovery cleans exact staging",
          "[runtime][save][lifecycle][failure]") {
    const auto kind = GENERATE(SaveSlotLifecycleFileKind::Journal, SaveSlotLifecycleFileKind::Generation);
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto next = Id<SlotGenerationId>(fixture.host.nextGeneration);
    fixture.fault = {};
    fixture.InjectFailure(SaveSlotLifecycleIoStage::DirectorySync, kind);
    REQUIRE(fixture.owner->Execute(fixture.Copy(10, 11)).HasError());
    Unchanged(fixture, before);
    fixture.Reopen();
    REQUIRE(fixture.owner->Reconcile(fixture.Access()).HasValue());
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    Unchanged(fixture, before);
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(next)));
}

TEST_CASE("Catalog sync failure after real rename reports Unknown and restart selects complete new destination retaining old evidence",
          "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto next = Id<SlotGenerationId>(fixture.host.nextGeneration);
    fixture.InjectFailure(SaveSlotLifecycleIoStage::DirectorySync, SaveSlotLifecycleFileKind::Catalog);
    const auto failed = fixture.owner->Execute(fixture.Copy(10, 11));
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
    CHECK(fixture.ExportBytes(10) == before.source);
    CHECK(fixture.Target(11).generation == next);
    CHECK(DiskBytes(fixture.Generation(before.index.entries.back().publication.generation)) == before.destination);
    const auto selectedDestination = fixture.ExportBytes(11);
    fixture.Reopen();
    CHECK(fixture.ExportBytes(11) == selectedDestination);
    CHECK(fixture.ExportBytes(10) == before.source);
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(before.index.entries.back().publication.generation)));
    CHECK(fixture.ExportBytes(11) == selectedDestination);
}

TEST_CASE("Cancellation after hidden generation prepare preserves selections and restart discards only owned staging",
          "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto next = Id<SlotGenerationId>(fixture.host.nextGeneration);
    CancellationSource cancellation;
    fixture.fault.action = [&](const auto stage, const auto kind) {
        if (stage == SaveSlotLifecycleIoStage::DirectorySync && kind == SaveSlotLifecycleFileKind::Generation)
            cancellation.RequestCancellation();
    };
    auto failed = fixture.owner->Execute(fixture.Copy(10, 11), cancellation.Token());
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == SaveErrors::OperationCancelled.code.Value());
    Unchanged(fixture, before);
    fixture.fault.action = {};
    fixture.Reopen();
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(next)));
    Unchanged(fixture, before);
}

TEST_CASE("Cancellation after catalog visibility cannot label a committed copy Cancelled", "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    CancellationSource cancellation;
    fixture.fault.action = [&](const auto stage, const auto kind) {
        if (stage == SaveSlotLifecycleIoStage::DirectorySync && kind == SaveSlotLifecycleFileKind::Catalog)
            cancellation.RequestCancellation();
    };
    auto copied = fixture.owner->Execute(fixture.Copy(10, 11), cancellation.Token());
    REQUIRE(copied.HasValue());
    CHECK(cancellation.Token().IsCancellationRequested());
    CHECK(fixture.Target(11).generation == copied.Value().entry->publication.generation);
}

TEST_CASE("Malformed incompatible wrong-scope and semantically invalid imports preserve both real slot generations",
          "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    auto bytes = MakeArchive().bytes;
    SECTION("Integrity corruption") {
        bytes.at(100) ^= std::byte{1};
    }
    SECTION("Trailing bytes") {
        bytes.push_back(std::byte{0});
    }
    SECTION("Truncated bytes") {
        bytes.resize(40);
    }
    SECTION("Unsupported required participant") {
        bytes = MakeArchiveWithUnknown(true).bytes;
    }
    SECTION("Unsupported future archive format") {
        auto fixtureBytes = MakeArchive();
        PutLittleEndian(fixtureBytes.bytes, 8, std::uint32_t{3});
        Rehash(fixtureBytes);
        bytes = std::move(fixtureBytes.bytes);
    }
    SECTION("Invalid zero archive format") {
        auto fixtureBytes = MakeArchive();
        PutLittleEndian(fixtureBytes.bytes, 8, std::uint32_t{0});
        Rehash(fixtureBytes);
        bytes = std::move(fixtureBytes.bytes);
    }
    SECTION("Valid archive in an unauthorized profile") {
        const auto archive = ReadOwned(bytes);
        auto header = archive.Header();
        header.profile = Id<GameProfileId>(90);
        bytes = RewriteHeader(archive, header);
    }
    SECTION("Valid archive with incompatible product version") {
        const auto archive = ReadOwned(bytes);
        auto header = archive.Header();
        header.productCompatibility = V<ProductSaveCompatibilityVersion>(99);
        bytes = RewriteHeader(archive, header);
    }
    RejectedImport(fixture, bytes, before);
}

TEST_CASE("Host size semantic and generation failures preserve both real selected generations", "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto bytes = MakeArchive().bytes;
    SECTION("Oversized input") {
        fixture.owner.reset();
        fixture.policy.archiveLimits.maximumArchiveBytes = 100;
        fixture.Open();
    }
    SECTION("Semantic rejection") {
        fixture.host.semanticFailure = true;
    }
    SECTION("Generation allocator failure") {
        fixture.host.allocationFailure = true;
    }
    RejectedImport(fixture, bytes, before);
}

TEST_CASE("Immutable generation collision cannot overwrite or garbage collect another artifact", "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto generation = Id<SlotGenerationId>(fixture.host.nextGeneration);
    const auto collision = fixture.Generation(generation);
    const std::vector foreign{std::byte{7}, std::byte{8}};
    SECTION("Existing identity") {
        WriteBytes(collision, foreign);
    }
    SECTION("Insertion races final atomic create") {
        fixture.fault.action = [&](const auto stage, const auto kind) {
            if (stage == SaveSlotLifecycleIoStage::Replace && kind == SaveSlotLifecycleFileKind::Generation)
                WriteBytes(collision, foreign);
        };
    }
    REQUIRE(fixture.owner->Execute(fixture.Copy(10, 11)).HasError());
    CHECK(DiskBytes(collision) == foreign);
    Unchanged(fixture, before);
    fixture.fault.action = {};
    fixture.Reopen();
    (void)fixture.owner->Reconcile(fixture.Access());
    CHECK(DiskBytes(collision) == foreign);
    Unchanged(fixture, before);
}

TEST_CASE("Corrupt catalog and recovery journal fail closed without repairing or deleting real evidence",
          "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    const auto imported = fixture.Import(10);
    const auto source = fixture.ExportBytes(10);
    SECTION("Corrupt catalog") {
        auto bytes = DiskBytes(fixture.Slots() / ".lifecycle.catalog");
        bytes.back() ^= std::byte{1};
        WriteBytes(fixture.Slots() / ".lifecycle.catalog", bytes);
        fixture.owner.reset();
        CHECK(SaveSlotLifecycle::Open(fixture.root, fixture.policy, fixture.host).HasError());
        CHECK(DiskBytes(fixture.Generation(imported.entry->publication.generation)) == source);
    }
    SECTION("Malformed journal") {
        WriteBytes(fixture.Slots() / ".lifecycle.journal", std::vector{std::byte{1}});
        fixture.Reopen();
        CHECK(fixture.owner->Reconcile(fixture.Access()).HasError());
        CHECK(fixture.ExportBytes(10) == source);
        CHECK(DiskBytes(fixture.Slots() / ".lifecycle.journal") == std::vector{std::byte{1}});
    }
}

TEST_CASE("Recovery refuses missing selected bytes and preserves retired last-known-good bytes on corruption",
          "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    const auto prior = fixture.Import(11);
    const auto oldGeneration = fixture.Generation(prior.entry->publication.generation);
    const auto oldBytes = DiskBytes(oldGeneration);
    SECTION("Missing selected generation without a journal") {
        std::filesystem::remove(oldGeneration);
        fixture.Reopen();
        CHECK(fixture.owner->Reconcile(fixture.Access()).HasError());
    }
    SECTION("Corruption after journal acknowledgement while retirement remains deferred") {
        fixture.InjectFailure(SaveSlotLifecycleIoStage::Remove, SaveSlotLifecycleFileKind::Generation);
        const auto copied = fixture.owner->Execute(fixture.Copy(10, 11));
        REQUIRE(copied.HasValue());
        REQUIRE(copied.Value().cleanupDeferred);
        CHECK_FALSE(std::filesystem::exists(fixture.Slots() / ".lifecycle.journal"));
        const auto selected = fixture.Generation(copied.Value().entry->publication.generation);
        auto damaged = DiskBytes(selected);
        damaged.at(100) ^= std::byte{1};
        WriteBytes(selected, damaged);
        const auto catalog = DiskBytes(fixture.Slots() / ".lifecycle.catalog");
        fixture.Reopen();
        CHECK(fixture.owner->Reconcile(fixture.Access()).HasError());
        CHECK(DiskBytes(oldGeneration) == oldBytes);
        CHECK(DiskBytes(selected) == damaged);
        CHECK(DiskBytes(fixture.Slots() / ".lifecycle.catalog") == catalog);
    }
}

TEST_CASE("Symlink hard-link and redirected catalog targets preserve outside destination and selected slots",
          "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto before = Before(fixture);
    const auto outside = fixture.temporary.Path() / "outside.horosave";
    const std::vector outsideBytes{std::byte{7}, std::byte{9}};
    WriteBytes(outside, outsideBytes);
    const auto generation = fixture.Generation(Id<SlotGenerationId>(fixture.host.nextGeneration));
    std::error_code error;
    SECTION("Symlink generation") {
        std::filesystem::create_symlink(outside, generation, error);
    }
    SECTION("Hard-linked generation") {
        std::filesystem::create_hard_link(outside, generation, error);
    }
    if (error) {
        SKIP("Filesystem does not permit creating the requested hostile link fixture");
    }
    REQUIRE(fixture.owner->Execute(fixture.Copy(10, 11)).HasError());
    CHECK(DiskBytes(outside) == outsideBytes);
    Unchanged(fixture, before);
}

TEST_CASE("Failed external export preserves source and existing destination bytes", "[runtime][save][lifecycle][failure]") {
    Fixture fixture;
    fixture.Import(10);
    const auto source = fixture.ExportBytes(10);
    auto external = Namespace();
    external.environment = Id<EnvironmentStorageId>(50);
    auto opened = SaveFilesystemStorage::Open(fixture.root, external);
    REQUIRE(opened.HasValue());
    auto destination = std::move(opened).Value();
    const auto slot = Id<SaveGameSlotId>(60);
    const std::vector old{std::byte{99}};
    REQUIRE(destination.Replace(slot, old).HasValue());
    const auto directory = NativeProbePath(fixture.root.CanonicalPath() / external.environment.ToString() / "profile" /
                                           (Id<LocalUserStorageId>(5).ToString() + "_" + Id<GameProfileId>(6).ToString()) / "slots");
    const auto saved = directory / (slot.ToString() + ".horosave");
    const auto outside = fixture.temporary.Path() / "export-target";
    WriteBytes(outside, old);
    std::filesystem::remove(saved);
    std::error_code error;
    std::filesystem::create_hard_link(outside, saved, error);
    if (error)
        SKIP("Filesystem does not support hard-link export failure fixture");
    REQUIRE(fixture.owner->ExportTo(fixture.Export(10), destination, slot).HasError());
    CHECK(DiskBytes(saved) == old);
    CHECK(DiskBytes(outside) == old);
    CHECK(fixture.ExportBytes(10) == source);
}

TEST_CASE("Postvisibility typed observer failures remain Unknown and preserve the prior generation", "[save][lifecycle]") {
    const bool allocation = GENERATE(false, true);
    Fixture fixture;
    fixture.Import(10);
    const auto previous = fixture.Import(11);
    const auto oldBytes = DiskBytes(fixture.Generation(previous.entry->publication.generation));
    fixture.fault.failure = [allocation](const SaveSlotLifecycleIoStage stage, const SaveSlotLifecycleFileKind kind) {
        if (stage == SaveSlotLifecycleIoStage::DirectorySync && kind == SaveSlotLifecycleFileKind::Catalog)
            return Result<void>::Failure(MakeError(allocation ? SaveErrors::StorageAllocationFailed : SaveErrors::StoragePermanentIo));
        return Result<void>::Success();
    };
    const auto result =
        fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Copy, .source = fixture.Target(10), .destination = fixture.Target(11)});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
    CHECK(fixture.host.leases == 0);
    CHECK(DiskBytes(fixture.Generation(previous.entry->publication.generation)) == oldBytes);
    fixture.fault.failure = {};
    fixture.Reopen();
    CHECK(fixture.Target(11).generation != previous.entry->publication.generation);
    CHECK_FALSE(fixture.ExportBytes(11).empty());
    CHECK(DiskBytes(fixture.Generation(previous.entry->publication.generation)) == oldBytes);
    const auto reconciled = fixture.owner->Reconcile(fixture.Access());
    REQUIRE(reconciled.HasValue());
    CHECK_FALSE(reconciled.Value());
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(previous.entry->publication.generation)));
}

TEST_CASE("Typed recycle provider failures remain committed with recoverable retained bytes", "[save][lifecycle]") {
    const bool allocation = GENERATE(false, true);
    Fixture fixture;
    fixture.host.recycleSupported = true;
    const auto previous = fixture.Import(10);
    const auto oldBytes = DiskBytes(fixture.Generation(previous.entry->publication.generation));
    fixture.host.onRecycle = [allocation] {
        return Result<void>::Failure(MakeError(allocation ? SaveErrors::StorageAllocationFailed : SaveErrors::StoragePermanentIo));
    };
    const auto result = fixture.owner->Execute(
        {.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10), .deleteMode = SaveSlotDeleteMode::PlatformRecycle});
    REQUIRE(result.HasValue());
    CHECK(result.Value().cleanupDeferred);
    CHECK(fixture.Index().entries.empty());
    CHECK(fixture.host.leases == 0);
    CHECK(DiskBytes(fixture.Generation(previous.entry->publication.generation)) == oldBytes);
    fixture.host.onRecycle = {};
    fixture.Reopen();
    const auto reconciled = fixture.owner->Reconcile(fixture.Access());
    REQUIRE(reconciled.HasValue());
    CHECK_FALSE(reconciled.Value());
    CHECK(fixture.host.recycled.at(previous.entry->publication.generation) == oldBytes);
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(previous.entry->publication.generation)));
}
