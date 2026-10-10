#include "SaveSlotLifecycleTestSupport.h"

using namespace Horo::Runtime::SaveSlotLifecycleTest;

TEST_CASE("Slot lifecycle import publishes new ownership and generation and survives reopen", "[runtime][save][lifecycle]") {
    Fixture fixture;
    const auto original = MakeArchive().bytes;
    const auto imported = fixture.Import(10, original);
    REQUIRE(imported.entry);
    CHECK(imported.entry->publication.slot == Id<SaveGameSlotId>(10));
    CHECK(imported.entry->publication.generation != Id<SlotGenerationId>(2));
    CHECK_FALSE(imported.cleanupDeferred);
    const auto exported = fixture.ExportBytes(10);
    auto archive = SaveArchiveReader{}.Read(exported);
    REQUIRE(archive.HasValue());
    CHECK(archive.Value().Header().slotGeneration == imported.entry->publication.generation);
    CHECK(archive.Value().Header().user == fixture.policy.destination.user);
    CHECK(archive.Value().Header().profile == fixture.policy.destination.profile);
    CHECK(archive.Value().Manifest() == SaveArchiveReader{}.Read(original).Value().Manifest());
    fixture.Reopen();
    CHECK(fixture.ExportBytes(10) == exported);
    CHECK(fixture.Index().entries == std::vector{*imported.entry});
    CHECK_FALSE(std::filesystem::exists(fixture.Slots() / ".lifecycle.journal"));
    CHECK(fixture.host.leases == 0);
}

TEST_CASE("Lifecycle copy preserves source and replaces destination as one new generation", "[runtime][save][lifecycle]") {
    Fixture fixture;
    fixture.Import(10);
    const auto oldDestination = fixture.Import(11);
    const auto originalSource = fixture.ExportBytes(10);
    const auto source = fixture.Target(10);
    auto copied = fixture.owner->Execute(fixture.Copy(10, 11));
    REQUIRE(copied.HasValue());
    REQUIRE(copied.Value().entry);
    CHECK(copied.Value().entry->publication.generation != source.generation);
    CHECK(copied.Value().entry->publication.generation != oldDestination.entry->publication.generation);
    CHECK(fixture.ExportBytes(10) == originalSource);
    const auto destination = ReadOwned(fixture.ExportBytes(11));
    CHECK(destination.Header().parentGeneration == oldDestination.entry->publication.generation);
    CHECK(destination.Manifest() == SaveArchiveReader{}.Read(originalSource).Value().Manifest());
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(oldDestination.entry->publication.generation)));
    fixture.Reopen();
    CHECK(fixture.Target(11).generation == copied.Value().entry->publication.generation);
}

TEST_CASE("Lifecycle rename is presentation-only and labels cannot become filesystem paths", "[runtime][save][lifecycle]") {
    Fixture fixture;
    const auto imported = fixture.Import(10);
    const auto original = fixture.ExportBytes(10);
    auto renamed = fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Rename,
                                           .source = fixture.Target(10),
                                           .display = {"../../outside/長い名前 ü", "Duplicate labels are metadata"}});
    REQUIRE(renamed.HasValue());
    CHECK(renamed.Value().entry->publication == imported.entry->publication);
    CHECK(fixture.ExportBytes(10) == original);
    CHECK_FALSE(std::filesystem::exists(fixture.root.CanonicalPath().parent_path() / "outside"));
    fixture.Reopen();
    CHECK(fixture.Index().entries.front().display.displayName == "../../outside/長い名前 ü");
}

TEST_CASE("Soft delete retains restorable bytes across restart and permanent delete purges only selected generation",
          "[runtime][save][lifecycle]") {
    Fixture fixture;
    const auto imported = fixture.Import(10);
    const auto sourceBytes = fixture.ExportBytes(10);
    auto deleted = fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10)});
    REQUIRE(deleted.HasValue());
    CHECK(fixture.Index().entries.empty());
    CHECK(DiskBytes(fixture.Generation(imported.entry->publication.generation)) == sourceBytes);
    fixture.Reopen();
    REQUIRE(fixture.owner->ListDeleted(fixture.Access()).Value().entries.size() == 1);
    auto restored = fixture.owner->Execute({.kind = SaveSlotLifecycleKind::RestoreDeleted, .source = fixture.Target(10, true)});
    REQUIRE(restored.HasValue());
    CHECK(fixture.ExportBytes(10) == sourceBytes);
    REQUIRE(fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10)}).HasValue());
    auto purged = fixture.owner->Execute(
        {.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10, true), .deleteMode = SaveSlotDeleteMode::Permanent});
    REQUIRE(purged.HasValue());
    CHECK_FALSE(purged.Value().cleanupDeferred);
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(imported.entry->publication.generation)));
    fixture.Reopen();
    CHECK(fixture.owner->ListDeleted(fixture.Access()).Value().entries.empty());
    CHECK(fixture.Index().entries.empty());
}

TEST_CASE("Unsupported recycle and denied permanent delete preserve the complete selected slot", "[runtime][save][lifecycle]") {
    Fixture fixture;
    const auto imported = fixture.Import(10);
    const auto original = fixture.ExportBytes(10);
    const auto catalog = DiskBytes(fixture.Slots() / ".lifecycle.catalog");
    auto recycle = fixture.owner->Execute(
        {.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10), .deleteMode = SaveSlotDeleteMode::PlatformRecycle});
    REQUIRE(recycle.HasError());
    CHECK(recycle.ErrorValue().code.Value() == SaveErrors::StorageCapabilityUnsupported.code.Value());
    CHECK(fixture.ExportBytes(10) == original);
    CHECK(DiskBytes(fixture.Slots() / ".lifecycle.catalog") == catalog);
    fixture.owner.reset();
    fixture.policy.allowPermanentDelete = false;
    fixture.Open();
    CHECK(fixture.owner
              ->Execute({.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10), .deleteMode = SaveSlotDeleteMode::Permanent})
              .HasError());
    CHECK(fixture.Target(10).generation == imported.entry->publication.generation);
}

TEST_CASE("Platform recycle failure is durable deferred cleanup and never falls back to deleting source bytes",
          "[runtime][save][lifecycle]") {
    Fixture fixture;
    fixture.host.recycleSupported = true;
    fixture.host.recycleFailure = true;
    const auto imported = fixture.Import(10);
    const auto original = fixture.ExportBytes(10);
    auto deleted = fixture.owner->Execute(
        {.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10), .deleteMode = SaveSlotDeleteMode::PlatformRecycle});
    REQUIRE(deleted.HasValue());
    CHECK(deleted.Value().cleanupDeferred);
    CHECK(DiskBytes(fixture.Generation(imported.entry->publication.generation)) == original);
    fixture.Reopen();
    CHECK(fixture.Index().entries.empty());
    CHECK(fixture.owner->Reconcile(fixture.Access()).Value());
    fixture.host.recycleFailure = false;
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK(fixture.host.recycled.at(imported.entry->publication.generation) == original);
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(imported.entry->publication.generation)));
    CHECK_FALSE(fixture.owner->Reconcile(fixture.Access()).Value());
    CHECK(fixture.host.recycled.size() == 1);
}

TEST_CASE("Capabilities are independent and stale generation catalog or profile consent cannot mutate slots",
          "[runtime][save][lifecycle]") {
    Fixture fixture;
    fixture.Import(10);
    fixture.Import(11);
    const auto oldCopy = fixture.Copy(10, 11);
    fixture.Import(11);
    CHECK(fixture.owner->Execute(oldCopy).HasError());
    auto copy = fixture.Copy(10, 11);
    copy.destination->address.namespaceAccess.expected.owner = UserProfileOwner{Id<LocalUserStorageId>(5), Id<GameProfileId>(70)};
    CHECK(fixture.owner->Execute(copy).HasError());
    copy = fixture.Copy(10, 11);
    ++fixture.host.binding.revision;
    CHECK(fixture.owner->Execute(copy).HasError());
    const auto before = DiskBytes(fixture.Slots() / ".lifecycle.catalog");
    fixture.owner.reset();
    fixture.policy.capabilities = std::uint16_t{1} << static_cast<std::uint8_t>(SaveSlotLifecycleKind::Export);
    fixture.Open();
    CHECK(fixture.owner->Execute(fixture.Copy(10, 11)).HasError());
    CHECK(fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Delete, .source = fixture.Target(10)}).HasError());
    REQUIRE(fixture.owner->Execute(fixture.Export(10)).HasValue());
    CHECK(DiskBytes(fixture.Slots() / ".lifecycle.catalog") == before);
}

TEST_CASE("Explicit cross-profile import reowns a new slot but cannot overwrite an existing destination", "[runtime][save][lifecycle]") {
    Fixture fixture;
    fixture.Import(10);
    const auto original = fixture.ExportBytes(10);
    fixture.owner.reset();
    auto foreign = fixture.policy.destination;
    foreign.name.owner = UserProfileOwner{Id<LocalUserStorageId>(5), Id<GameProfileId>(90)};
    foreign.profile = Id<GameProfileId>(90);
    fixture.policy.importSources.push_back(foreign);
    fixture.Open();
    auto archive = ReadOwned(MakeArchive().bytes);
    auto header = archive.Header();
    header.profile = foreign.profile;
    const auto foreignBytes = RewriteHeader(archive, header);
    auto request = SaveSlotLifecycleRequest{.kind = SaveSlotLifecycleKind::Import,
                                            .source = fixture.Target(10),
                                            .imported = {std::make_shared<const std::vector<std::byte>>(foreignBytes)},
                                            .importSource = 1};
    REQUIRE(fixture.owner->Execute(request).HasError());
    CHECK(fixture.ExportBytes(10) == original);
    const auto imported = fixture.Import(11, foreignBytes, 1);
    auto reowned = ReadOwned(fixture.ExportBytes(11));
    CHECK(reowned.Header().profile == fixture.policy.destination.profile);
    CHECK(reowned.Header().slotGeneration == imported.entry->publication.generation);
    CHECK(reowned.Header().slotGeneration != header.slotGeneration);
    CHECK(fixture.ExportBytes(10) == original);
}

TEST_CASE("Export owns an immutable independent snapshot and publishes only complete archive bytes", "[runtime][save][lifecycle]") {
    Fixture fixture;
    fixture.Import(10);
    const auto bytes = fixture.ExportBytes(10);
    auto snapshot = fixture.owner->Execute(fixture.Export(10)).Value();
    auto external = Namespace();
    external.environment = Id<EnvironmentStorageId>(50);
    auto opened = SaveFilesystemStorage::Open(fixture.root, external);
    REQUIRE(opened.HasValue());
    auto destination = std::move(opened).Value();
    REQUIRE(fixture.owner->ExportTo(fixture.Export(10), destination, Id<SaveGameSlotId>(60)).HasValue());
    CHECK(destination.Read(Id<SaveGameSlotId>(60), bytes.size()).Value() == bytes);
    fixture.Import(10, MakeArchive("1.2.4-preview.1").bytes);
    CHECK(*snapshot.exported.bytes == bytes);
    CHECK(fixture.ExportBytes(10) != bytes);
}

TEST_CASE("Lifecycle repack preserves unknown optional stored compressed records exactly", "[runtime][save][lifecycle]") {
    Fixture fixture;
    const auto original = MakeArchiveWithUnknown(false, std::byte{0xa5}, "1.2.3-preview.4", true).bytes;
    fixture.Import(10, original);
    REQUIRE(fixture.owner->Execute(fixture.Copy(10, 11)).HasValue());
    const auto admittedSource = SaveArchiveReader{}.Read(original).Value();
    const auto copiedBytes = fixture.ExportBytes(11);
    const auto admittedCopy = SaveArchiveReader{}.Read(copiedBytes).Value();
    CHECK(VerifyUnknownDataRoundTrip(admittedSource, admittedCopy, fixture.policy.compatibility, 1024).HasValue());
    CHECK(admittedCopy.Manifest() == admittedSource.Manifest());
}

namespace {
    /** @brief Deterministic signature-provider fixture; exercises authority sequencing without claiming real Ed25519 cryptography. */
    class SignatureVerifier final : public SaveArchiveSignatureProvider {
    public:
        bool SupportsEd25519() const noexcept override {
            return true;
        }

        Result<void> Verify(const std::array<std::uint8_t, 16> key, const std::span<const std::byte> scope,
                            const std::span<const std::byte> message, const std::span<const std::byte> signature) override {
            ++calls;
            CHECK(scope.size() == CanonicalSaveNamespaceKeyBytes);
            if (key.front() != 7 || signature.size() != 64 || message.size() < 32 ||
                !std::ranges::equal(signature.first(32), message.last(32)) || !std::ranges::equal(signature.last(32), message.last(32)))
                return Result<void>::Failure(MakeError(SaveErrors::ProtectionAuthenticationFailed));
            return Result<void>::Success();
        }

        std::size_t calls{};
    };

    std::vector<std::byte> Signed(std::vector<std::byte> bytes) {
        const auto payloadBytes = GetLittleEndian<std::uint64_t>(bytes, 16);
        PutLittleEndian(bytes, 24, std::uint32_t{SaveArchiveSignedTrailerByteLength});
        bytes.resize(SaveArchivePreambleByteLength + payloadBytes + SaveArchiveSignedTrailerByteLength);
        const auto preamble = std::span<const std::byte>{bytes}.first(SaveArchivePreambleByteLength);
        const auto payload = std::span<const std::byte>{bytes}.subspan(SaveArchivePreambleByteLength, payloadBytes);
        const auto hash = ComputeArchiveContentHash(preamble, payload);
        const auto trailer = SaveArchivePreambleByteLength + payloadBytes;
        PutBytes(bytes, trailer, hash.value.bytes);
        PutLittleEndian(bytes, trailer + 32, std::uint16_t{1});
        bytes.at(trailer + 34) = std::byte{7};
        PutLittleEndian(bytes, trailer + 50, std::uint16_t{64});
        PutBytes(bytes, trailer + 52, hash.value.bytes);
        PutBytes(bytes, trailer + 84, hash.value.bytes);
        return bytes;
    }

    /** @brief Proves required-signature denial precedes semantics and any catalog mutation. */
    void RejectUnsigned(Fixture &fixture, const std::vector<std::byte> &bytes) {
        const auto unsignedSource = std::make_shared<const std::vector<std::byte>>(bytes);
        auto denied = fixture.owner->Execute(
            {.kind = SaveSlotLifecycleKind::Import, .source = fixture.Target(10), .imported = {unsignedSource}, .importSource = 0});
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == SaveErrors::SignatureRequired.code.Value());
        CHECK(fixture.host.semanticCalls == 0);
        CHECK(fixture.Index().entries.empty());
    }

    /** @brief Composes required shipping signatures with verifier lifetime owned by the calling fixture scope. */
    void ConfigureSignatures(Fixture &fixture, SignatureVerifier &verifier) {
        fixture.owner.reset();
        fixture.policy.signature = SaveSignaturePolicy::Required;
        fixture.policy.profile = SaveSlotLifecycleProfile::Shipping;
        fixture.host.verifier = &verifier;
        fixture.host.signer = [](std::vector<std::byte> bytes) {
            return Result<std::vector<std::byte>>::Success(Signed(std::move(bytes)));
        };
        fixture.Open();
    }
}  // namespace

TEST_CASE("Required signature policy verifies untrusted source then signs and reverifies new publication",
          "[runtime][save][lifecycle][security]") {
    SignatureVerifier verifier;
    Fixture fixture;
    ConfigureSignatures(fixture, verifier);
    const auto versionTwo = GENERATE(false, true);
    const auto input = versionTwo ? MakeArchiveWithUnknown(false, std::byte{0xa5}, "1.2.3-preview.4", true).bytes : MakeArchive().bytes;
    RejectUnsigned(fixture, input);
    const auto signedSource = Signed(input);
    fixture.Import(10, signedSource);
    const auto sourceBytes = fixture.ExportBytes(10);
    CHECK(fixture.host.signCalls == 1);
    CHECK(ReadOwned(sourceBytes).Signature().algorithm == SaveArchiveSignatureAlgorithm::Ed25519);
    CHECK(sourceBytes != signedSource);
    const auto sourceEntry = fixture.Index().entries.front();
    SECTION("Unavailable authorized signer") {
        fixture.host.signer = {};
        CHECK(fixture.owner->Execute(fixture.Copy(10, 11)).HasError());
        CHECK(fixture.Index().entries == std::vector{sourceEntry});
    }
    SECTION("Signer cannot return unsigned output") {
        fixture.host.signer = [](std::vector<std::byte> bytes) {
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        };
        auto failed = fixture.owner->Execute(fixture.Copy(10, 11));
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == SaveErrors::SignatureRequired.code.Value());
        CHECK(fixture.Index().entries == std::vector{sourceEntry});
    }
    SECTION("Invalid present source signature cannot reach semantics") {
        auto invalid = signedSource;
        invalid.back() ^= std::byte{1};
        const auto calls = fixture.host.semanticCalls;
        auto failed = fixture.owner->Execute({.kind = SaveSlotLifecycleKind::Import,
                                              .source = fixture.Target(11),
                                              .imported = {std::make_shared<const std::vector<std::byte>>(invalid)},
                                              .importSource = 0});
        REQUIRE(failed.HasError());
        CHECK(fixture.host.semanticCalls == calls);
        CHECK(fixture.Index().entries == std::vector{sourceEntry});
    }
    CHECK(fixture.ExportBytes(10) == sourceBytes);
    fixture.owner.reset();  // The injected verifier outlives every accepted operation and owner.
}

TEST_CASE("Server and malformed host scope policy fail closed before filesystem ownership", "[runtime][save][lifecycle][security]") {
    Fixture fixture;
    auto policy = fixture.policy;
    policy.profile = SaveSlotLifecycleProfile::Server;
    CHECK(SaveSlotLifecycle::Open(fixture.root, policy, fixture.host).HasError());
    policy = fixture.policy;
    policy.destination.profile = Id<GameProfileId>(77);
    CHECK(SaveSlotLifecycle::Open(fixture.root, policy, fixture.host).HasError());
    policy = fixture.policy;
    policy.maximumSlots = 0;
    CHECK(SaveSlotLifecycle::Open(fixture.root, policy, fixture.host).HasError());
    policy = fixture.policy;
    policy.capabilities = std::numeric_limits<std::uint16_t>::max();
    CHECK(SaveSlotLifecycle::Open(fixture.root, policy, fixture.host).HasError());
}
