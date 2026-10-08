#include "Horo/Runtime/Save/SaveThumbnailArchive.h"
#include "SaveArchiveReaderTestHelpers.h"

#include <future>
#include <mutex>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace Test;
        using namespace ArchiveReaderTest;

        /** @brief Produces detached logical input and terminal thumbnail evidence using production capture. */
        [[nodiscard]] SavePresentationArchiveInput Input(const bool captured = true, const std::uint32_t version = 1,
                                                         const SaveThumbnailPolicy policy = SaveThumbnailPolicy::Optional) {
            auto fixture = MakeArchiveWithUnknown(false);
            auto archive = SaveArchiveReader{}.Read(fixture.bytes).Value();
            SavePresentationArchiveInput input{.header = archive.Header(),
                                               .manifest = archive.Manifest(),
                                               .version = V<ArchiveFormatVersion>(version),
                                               .publication = Entry(1, 2).publication,
                                               .display = {.displayName = "Kayıt"}};
            input.publication.baseScene = input.header.baseScene;
            input.publication.canonicalState = input.manifest.canonicalState;
            for (const auto &entry : archive.Directory().Entries()) {
                const auto bytes =
                    archive.Payload().subspan(static_cast<std::size_t>(entry.offset), static_cast<std::size_t>(entry.storedByteLength));
                input.chunks.push_back({entry, {bytes.begin(), bytes.end()}});
            }
            SaveThumbnailRequest request{.slot = input.header.slot,
                                         .generation = input.header.slotGeneration,
                                         .thumbnail = Id<SaveThumbnailId>(6),
                                         .policy = policy,
                                         .source = {.runtime = 1, .scene = 2, .view = 3, .frame = 10}};
            SaveThumbnailCapture capture;
            const auto serial =
                capture.Request(request, captured ? SaveThumbnailAvailability::Available : SaveThumbnailAvailability::Headless, {}).Value();
            if (captured) {
                REQUIRE(capture
                            .Complete({.requestSerial = serial,
                                       .slot = request.slot,
                                       .generation = request.generation,
                                       .thumbnail = request.thumbnail,
                                       .source = {.runtime = 1, .scene = 2, .view = 3, .frame = 12},
                                       .width = 320,
                                       .height = 180,
                                       .encoded = {std::byte{1}, std::byte{2}, std::byte{3}}},
                                      {}, request.source)
                            .Value());
            }
            input.capture = capture.Snapshot();
            return input;
        }

        /** @brief Matches archive ownership to the existing namespace binding contract. */
        [[nodiscard]] SaveStorageAddress PresentationAddress() {
            const auto header = Header();
            return {.namespaceAccess = {.expected = {.product = header.product,
                                                     .environment = header.environment,
                                                     .owner = UserProfileOwner{header.user, header.profile}},
                                        .expectedRevision = 7},
                    .slot = header.slot};
        }

        /** @brief Qualified test provider retains the exact atomic write presented to it. */
        class Provider final : public ISaveStorageProvider {
        public:
            [[nodiscard]] SaveStorageCapabilities Capabilities() const noexcept override {
                return SaveStorageCapabilities::All();
            }

            [[nodiscard]] Result<SaveStorageValue> Execute(const SaveStorageRequest &request, const CancellationToken &) override {
                std::lock_guard lock(mutex_);
                write_ = request.write;
                thread_ = std::this_thread::get_id();
                return Result<SaveStorageValue>::Success(std::monostate{});
            }

            [[nodiscard]] std::optional<SaveStorageWrite> Write() const {
                std::lock_guard lock(mutex_);
                return write_;
            }

            [[nodiscard]] std::thread::id Thread() const {
                std::lock_guard lock(mutex_);
                return thread_;
            }

        private:
            mutable std::mutex mutex_;
            std::optional<SaveStorageWrite> write_;
            std::thread::id thread_;
        };

        /** @brief Waits only in test code for finite hosted worker completion. */
        [[nodiscard]] SaveOperationSnapshot Await(const SaveStorageOperation &operation) {
            for (std::size_t i = 0; i < 2'000; ++i) {
                const auto snapshot = operation.Snapshot();
                REQUIRE(snapshot);
                if (snapshot->IsTerminal())
                    return *snapshot;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("presentation save did not complete");
        }

        TEST_CASE("Presentation raw chunks round trip in unchanged v1 and v2 framing without changing logical state",
                  "[unit][save][thumbnail][archive]") {
            for (const auto version : {1U, 2U}) {
                const auto input = Input(true, version);
                const auto prepared = PrepareSavePresentationWrite(input);
                REQUIRE(prepared.HasValue());
                REQUIRE(prepared.Value().metadata.publication.canonicalState == input.manifest.canonicalState);
                REQUIRE(prepared.Value().metadata.publication.thumbnail == Id<SaveThumbnailId>(6));
                auto archive = SaveArchiveReader{}.Read(prepared.Value().archive.bytes);
                REQUIRE(archive.HasValue());
                REQUIRE(archive.Value().Preamble().archiveFormatVersion.Value() == version);
                REQUIRE(archive.Value().Manifest().canonicalState == input.manifest.canonicalState);
                auto thumbnail = ReadSaveThumbnail(archive.Value(), prepared.Value().metadata.publication);
                REQUIRE(thumbnail.HasValue());
                REQUIRE(thumbnail.Value());
                REQUIRE((*thumbnail.Value())->Source().frame == 12);
                REQUIRE(std::ranges::equal((*thumbnail.Value())->Bytes(), input.capture.artifact->Bytes()));
                auto policy = UnknownPolicy();
                policy.archiveVersions.direct.maximum = V<ArchiveFormatVersion>(2);
                const auto unknown = archive.Value().InspectUnknownData(policy, 1U << 20U);
                REQUIRE(unknown.HasValue());
                REQUIRE(std::ranges::count(unknown.Value().preserved, SaveThumbnailArchiveOwner(), [](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner;
                }) == 2);
            }
        }

        TEST_CASE("Optional presentation yields archive capacity to valid logical state", "[unit][save][thumbnail][archive]") {
            for (const auto version : {1U, 2U}) {
                const auto logical = PrepareSavePresentationWrite(Input(false, version)).Value();
                for (const auto policy : {SaveThumbnailPolicy::Optional, SaveThumbnailPolicy::Required}) {
                    for (const auto byteBudget : {false, true}) {
                        auto input = Input(true, version, policy);
                        if (byteBudget)
                            input.limits.maximumArchiveBytes = logical.archive.bytes->size();
                        else
                            input.limits.maximumEntries = input.chunks.size() + 2;
                        const auto prepared = PrepareSavePresentationWrite(input);
                        if (policy == SaveThumbnailPolicy::Required) {
                            REQUIRE(prepared.HasError());
                            continue;
                        }
                        REQUIRE(prepared.HasValue());
                        REQUIRE_FALSE(prepared.Value().metadata.publication.thumbnail);
                        REQUIRE(prepared.Value().metadata.publication.canonicalState == input.manifest.canonicalState);
                        REQUIRE(prepared.Value().metadata.publication.archiveContent == logical.metadata.publication.archiveContent);
                        const auto archive = SaveArchiveReader{}.Read(prepared.Value().archive.bytes);
                        REQUIRE(archive.HasValue());
                        const auto thumbnail = ReadSaveThumbnail(archive.Value(), prepared.Value().metadata.publication);
                        REQUIRE(thumbnail.HasValue());
                        REQUIRE_FALSE(thumbnail.Value());
                        if (byteBudget)
                            --input.limits.maximumArchiveBytes;
                        else
                            --input.limits.maximumEntries;
                        REQUIRE(PrepareSavePresentationWrite(input).HasError());
                    }
                }
            }
        }

        TEST_CASE("Optional presentation yields storage byte capacity before the commit gate", "[unit][save][thumbnail][storage]") {
            const auto logical = PrepareSavePresentationWrite(Input(false)).Value();
            for (const auto policy : {SaveThumbnailPolicy::Optional, SaveThumbnailPolicy::Required}) {
                JobSystem jobs({.workerCount = 1});
                auto provider = std::make_shared<Provider>();
                SaveStorageAdapter adapter(jobs, provider, {.maximumArchiveBytes = logical.archive.bytes->size()});
                const auto admitted = adapter.SubmitPresentation(1, PresentationAddress(), Input(true, 1, policy));
                REQUIRE(admitted.HasValue());
                const auto terminal = Await(admitted.Value());
                if (policy == SaveThumbnailPolicy::Required) {
                    REQUIRE(terminal.state == SaveOperationState::Failed);
                    REQUIRE(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
                    REQUIRE_FALSE(provider->Write());
                    continue;
                }
                REQUIRE(terminal.state == SaveOperationState::Completed);
                REQUIRE(terminal.commit == SaveOperationCommitOutcome::Committed);
                const auto write = provider->Write();
                REQUIRE(write);
                REQUIRE_FALSE(write->metadata.publication.thumbnail);
                REQUIRE(write->metadata.publication.archiveContent == logical.metadata.publication.archiveContent);
                REQUIRE(write->archive.bytes->size() <= logical.archive.bytes->size());
                const auto archive = SaveArchiveReader{}.Read(write->archive.bytes);
                REQUIRE(archive.HasValue());
                const auto thumbnail = ReadSaveThumbnail(archive.Value(), write->metadata.publication);
                REQUIRE(thumbnail.HasValue());
                REQUIRE_FALSE(thumbnail.Value());
            }
        }

        TEST_CASE("Presentation owner and globally reserved record collisions fail before construction",
                  "[unit][save][thumbnail][archive]") {
            for (const auto variant : {0, 1, 2, 3}) {
                auto input = Input();
                if (variant == 0)
                    input.manifest.participants.front().participant = SaveThumbnailArchiveOwner();
                if (variant == 1)
                    input.manifest.participants.front().chunks.front() = SaveThumbnailMetadataRecord();
                if (variant == 2)
                    input.chunks.front().entry.owner = SaveThumbnailArchiveOwner();
                if (variant == 3)
                    input.chunks.front().entry.record = SaveThumbnailImageRecord();
                REQUIRE(PrepareSavePresentationWrite(input).HasError());
            }
        }

        TEST_CASE("Stored thumbnail cannot be extracted for a newer generation or wrong content reference",
                  "[unit][save][thumbnail][archive]") {
            const auto prepared = PrepareSavePresentationWrite(Input()).Value();
            const auto archive = SaveArchiveReader{}.Read(prepared.archive.bytes).Value();
            auto publication = prepared.metadata.publication;
            publication.generation = Id<SlotGenerationId>(7);
            REQUIRE(ReadSaveThumbnail(archive, publication).ErrorValue().code.Value() == SaveErrors::ThumbnailStale.code.Value());
            publication = prepared.metadata.publication;
            publication.thumbnail = Id<SaveThumbnailId>(7);
            REQUIRE(ReadSaveThumbnail(archive, publication).HasError());
            publication = prepared.metadata.publication;
            publication.archiveContent.value.bytes.front() ^= 1U;
            REQUIRE(ReadSaveThumbnail(archive, publication).HasError());
            REQUIRE(ReadSaveThumbnail(archive, prepared.metadata.publication, {.maximumEncodedBytes = 2}).HasError());
            REQUIRE(ReadSaveThumbnail(archive, prepared.metadata.publication, {.maximumDimension = 100}).HasError());
        }

        TEST_CASE("Malformed optional presentation metadata is admitted as opaque data but cannot be displayed",
                  "[unit][save][thumbnail][archive]") {
            const auto prepared = PrepareSavePresentationWrite(Input()).Value();
            const auto original = SaveArchiveReader{}.Read(prepared.archive.bytes).Value();
            for (const auto variant : {0, 1, 2, 3, 4}) {
                std::vector<PreservedSaveChunk> chunks;
                for (const auto &entry : original.Directory().Entries()) {
                    const auto bytes = original.Payload().subspan(static_cast<std::size_t>(entry.offset),
                                                                  static_cast<std::size_t>(entry.storedByteLength));
                    chunks.push_back({entry, {bytes.begin(), bytes.end()}});
                }
                auto &metadata = *std::ranges::find(chunks, SaveThumbnailMetadataRecord(), [](const PreservedSaveChunk &chunk) {
                    return chunk.entry.record;
                });
                if (variant == 0)
                    metadata.storedBytes.front() = std::byte{0};
                if (variant == 1)
                    metadata.storedBytes.pop_back();
                if (variant == 2)
                    std::fill_n(metadata.storedBytes.begin() + 56, 8, std::byte{0});
                if (variant == 3)
                    std::fill_n(metadata.storedBytes.begin() + 88, 4, std::byte{0});
                if (variant == 4)
                    metadata.storedBytes[96] = std::byte{4};
                metadata.entry.storedByteLength = metadata.storedBytes.size();
                metadata.entry.decodedByteLength = metadata.storedBytes.size();
                metadata.entry.decodedHash = ComputeSha256(metadata.storedBytes);
                const auto finalized =
                    SaveArchiveContainerWriter::Write(original.Header(), original.Manifest(), chunks, V<ArchiveFormatVersion>(1));
                REQUIRE(finalized.HasValue());
                const auto archive = SaveArchiveReader{}.Read(finalized.Value().Archive().bytes);
                REQUIRE(archive.HasValue());
                auto publication = prepared.metadata.publication;
                publication.archiveContent = archive.Value().Integrity().archiveContent;
                REQUIRE(ReadSaveThumbnail(archive.Value(), publication).HasError());
            }
        }

        TEST_CASE("Legacy optional-owner policy preserves presentation across repack and rejects unknown required schema",
                  "[unit][save][thumbnail][archive]") {
            const auto prepared = PrepareSavePresentationWrite(Input()).Value();
            const auto original = SaveArchiveReader{}.Read(prepared.archive.bytes).Value();
            std::vector<PreservedSaveChunk> chunks;
            for (const auto &entry : original.Directory().Entries()) {
                const auto bytes =
                    original.Payload().subspan(static_cast<std::size_t>(entry.offset), static_cast<std::size_t>(entry.storedByteLength));
                chunks.push_back({entry, {bytes.begin(), bytes.end()}});
            }
            const auto repacked =
                SaveArchiveContainerWriter::Write(original.Header(), original.Manifest(), chunks, V<ArchiveFormatVersion>(1));
            REQUIRE(repacked.HasValue());
            const auto destination = SaveArchiveReader{}.Read(repacked.Value().Archive().bytes).Value();
            auto policy = UnknownPolicy();
            REQUIRE(VerifyUnknownDataRoundTrip(original, destination, policy, 1U << 20U).HasValue());
            REQUIRE(ReadSaveThumbnail(destination, prepared.metadata.publication).Value());
            auto manifest = original.Manifest();
            auto owner = std::ranges::find(manifest.participants, SaveThumbnailArchiveOwner(), &SaveManifestParticipant::participant);
            owner->schemaVersion = V<ParticipantSchemaVersion>(2);
            auto future = SaveArchiveContainerWriter::Write(original.Header(), manifest, chunks, V<ArchiveFormatVersion>(1));
            REQUIRE(future.HasValue());
            const auto futureArchive = SaveArchiveReader{}.Read(future.Value().Archive().bytes).Value();
            auto publication = prepared.metadata.publication;
            publication.archiveContent = futureArchive.Integrity().archiveContent;
            REQUIRE(futureArchive.InspectUnknownData(policy, 1U << 20U).HasValue());
            REQUIRE(ReadSaveThumbnail(futureArchive, publication).HasError());
            owner->required = true;
            auto required = SaveArchiveContainerWriter::Write(original.Header(), manifest, chunks, V<ArchiveFormatVersion>(1));
            REQUIRE(required.HasValue());
            REQUIRE(SaveArchiveReader{}.Read(required.Value().Archive().bytes).Value().InspectUnknownData(policy, 1U << 20U).HasError());
        }

        TEST_CASE("Optional capture failure publishes a valid save through asynchronous storage", "[unit][save][thumbnail][storage]") {
            JobSystem jobs({.workerCount = 1});
            auto provider = std::make_shared<Provider>();
            SaveStorageAdapter adapter(jobs, provider);
            const auto input = Input(false);
            const auto admitted = adapter.SubmitPresentation(1, PresentationAddress(), input);
            REQUIRE(admitted.HasValue());
            const auto terminal = Await(admitted.Value());
            REQUIRE(terminal.state == SaveOperationState::Completed);
            REQUIRE(terminal.commit == SaveOperationCommitOutcome::Committed);
            const auto write = provider->Write();
            REQUIRE(write);
            REQUIRE_FALSE(write->metadata.publication.thumbnail);
            auto archive = SaveArchiveReader{}.Read(write->archive.bytes);
            REQUIRE(archive.HasValue());
            const auto thumbnail = ReadSaveThumbnail(archive.Value(), write->metadata.publication);
            REQUIRE(thumbnail.HasValue());
            REQUIRE_FALSE(thumbnail.Value());
            REQUIRE(provider->Thread() != std::this_thread::get_id());
        }

        TEST_CASE("Presentation save returns while its single worker is blocked and commits exact captured bytes later",
                  "[unit][save][thumbnail][storage]") {
            JobSystem jobs({.workerCount = 1});
            auto started = std::make_shared<std::promise<void>>();
            std::promise<void> release;
            const auto released = release.get_future().share();
            auto startedFuture = started->get_future();
            auto blocked = jobs.SubmitResult({}, [started, released](const CancellationToken &) {
                started->set_value();
                (void)released.wait_for(std::chrono::seconds{10});
                return Result<void>::Success();
            });
            REQUIRE(blocked.HasValue());
            REQUIRE(startedFuture.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
            auto provider = std::make_shared<Provider>();
            SaveStorageAdapter adapter(jobs, provider);
            const auto admitted = adapter.SubmitPresentation(1, PresentationAddress(), Input());
            REQUIRE(admitted.HasValue());
            REQUIRE_FALSE(provider->Write());
            release.set_value();
            REQUIRE(Await(admitted.Value()).state == SaveOperationState::Completed);
            const auto write = provider->Write();
            REQUIRE(write);
            const auto archive = SaveArchiveReader{}.Read(write->archive.bytes);
            REQUIRE(archive.HasValue());
            REQUIRE(ReadSaveThumbnail(archive.Value(), write->metadata.publication).Value());
        }

        TEST_CASE("Presentation submission revalidates product environment and user profile before queuing",
                  "[unit][save][thumbnail][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<Provider>();
            SaveStorageAdapter adapter(jobs, provider);
            for (const auto variant : {0, 1, 2, 3}) {
                auto address = PresentationAddress();
                if (variant == 0)
                    address.namespaceAccess.expected.product = Id<ProductStorageId>(8);
                if (variant == 1)
                    address.namespaceAccess.expected.environment = Id<EnvironmentStorageId>(8);
                auto &owner = std::get<UserProfileOwner>(address.namespaceAccess.expected.owner);
                if (variant == 2)
                    owner.user = Id<LocalUserStorageId>(8);
                if (variant == 3)
                    owner.profile = Id<GameProfileId>(8);
                REQUIRE(adapter.SubmitPresentation(1, address, Input()).HasError());
            }
            REQUIRE_FALSE(provider->Write());
        }

        TEST_CASE("Presentation preparation observes deadline and storage budget before provider commit",
                  "[unit][save][thumbnail][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<Provider>();
            SaveStorageAdapter adapter(jobs, provider);
            const auto expired = adapter.SubmitPresentation(1, PresentationAddress(), Input(), {},
                                                            std::chrono::steady_clock::now() - std::chrono::seconds{1});
            REQUIRE(expired.HasValue());
            REQUIRE(Await(expired.Value()).state == SaveOperationState::Cancelled);
            SaveStorageAdapter bounded(jobs, provider, {.maximumArchiveBytes = 1});
            const auto oversized = bounded.SubmitPresentation(2, PresentationAddress(), Input());
            REQUIRE(oversized.HasValue());
            const auto terminal = Await(oversized.Value());
            REQUIRE(terminal.state == SaveOperationState::Failed);
            REQUIRE(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
            REQUIRE_FALSE(provider->Write());
        }

        TEST_CASE("Required thumbnail failure and preparation errors never reach storage commit", "[unit][save][thumbnail][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<Provider>();
            SaveStorageAdapter adapter(jobs, provider);
            auto input = Input(false);
            input.capture.request->policy = SaveThumbnailPolicy::Required;
            input.capture.state = SaveThumbnailCaptureState::Failed;
            REQUIRE(adapter.SubmitPresentation(1, PresentationAddress(), input).HasError());
            REQUIRE_FALSE(provider->Write());
            input = Input();
            input.header.slotGeneration = Id<SlotGenerationId>(7);
            const auto admitted = adapter.SubmitPresentation(2, PresentationAddress(), input);
            REQUIRE(admitted.HasValue());
            const auto terminal = Await(admitted.Value());
            REQUIRE(terminal.state == SaveOperationState::Failed);
            REQUIRE(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
            REQUIRE_FALSE(provider->Write());
        }
    }  // namespace
}  // namespace Horo::Runtime
