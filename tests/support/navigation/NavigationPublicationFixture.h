#pragma once

#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "NativePublicationFiles.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavigationPublicationEntropy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

namespace Horo::Application::TestSupport {
    using namespace Horo::Navigation;

    /** @brief Owns only one freshly created private publication test directory. */
    struct PublicationDirectory {
        std::filesystem::path path =
            std::filesystem::current_path() /
            std::format("horo atomic publication ü {}", std::chrono::steady_clock::now().time_since_epoch().count());

        PublicationDirectory() {
            REQUIRE(std::filesystem::create_directory(path));
        }

        PublicationDirectory(const PublicationDirectory &) = delete;
        PublicationDirectory &operator=(const PublicationDirectory &) = delete;

        ~PublicationDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    enum class PublicationFault {
        None,
        ArtifactWrite,
        ManifestWrite,
        CurrentWrite,
        GenerationRename,
        CurrentRename,
        AfterCurrentRename,
        AfterCurrentThrow,
        DirectorySync,
    };

    /** @brief Delegates all successful actions to the native durable filesystem and pauses at true disk boundaries. */
    class PublicationFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        std::atomic<PublicationFault> fault{PublicationFault::None};
        std::atomic<bool> pauseBeforeCurrent{};
        std::atomic<bool> pauseAfterCurrent{};
        std::atomic<bool> beforeCurrentReached{};
        std::atomic<bool> afterCurrentReached{};
        std::atomic<bool> lockContended{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            auto acquired = native.TryAcquireExclusive(path, owner);
            if (acquired.HasError())
                lockContended.store(true);
            return acquired;
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            if (WriteFails(path))
                return InjectedFailure();
            auto written = native.WriteDurable(path, bytes);
            const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            if (written.HasValue() && path.filename().string().starts_with("current.json") &&
                text.find("\"generationPath\"") != std::string_view::npos) {
                beforeCurrentReached.store(true);
                while (pauseBeforeCurrent.load())
                    std::this_thread::yield();
            }
            return written;
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            if (RenameFails(destination))
                return InjectedFailure();
            return native.AtomicReplace(prepared, destination);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (RenameFails(destination))
                return InjectedFailure();
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (receipt.WasCommitted() && destination.filename() == "current.json" && prepared.filename() == "current.json") {
                afterCurrentReached.store(true);
                while (pauseAfterCurrent.load())
                    std::this_thread::yield();
                if (fault.load() == PublicationFault::AfterCurrentRename)
                    return InjectedFailure();
                if (fault.load() == PublicationFault::AfterCurrentThrow)
                    throw 73;
            }
            return replaced;
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if (fault.load() == PublicationFault::DirectorySync)
                return InjectedFailure();
            return native.SyncDirectory(path);
        }

    private:
        [[nodiscard]] static Result<void> InjectedFailure() {
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputFailed));
        }

        [[nodiscard]] bool WriteFails(const std::filesystem::path &path) const {
            const auto name = path.filename().string();
            return (fault.load() == PublicationFault::ArtifactWrite && name.find(".cooked") != std::string::npos) ||
                   (fault.load() == PublicationFault::ManifestWrite && name.starts_with("manifest.json")) ||
                   (fault.load() == PublicationFault::CurrentWrite && name.starts_with("current.json"));
        }

        [[nodiscard]] bool RenameFails(const std::filesystem::path &path) const {
            return (fault.load() == PublicationFault::GenerationRename && path.parent_path().filename() == "generations") ||
                   (fault.load() == PublicationFault::CurrentRename && path.filename() == "current.json");
        }
    };

    /** @brief Releases a paused native callback even when a test assertion aborts. */
    struct PublicationPause {
        std::shared_ptr<PublicationFiles> files;

        ~PublicationPause() {
            files->pauseBeforeCurrent.store(false);
            files->pauseAfterCurrent.store(false);
        }
    };

    [[nodiscard]] inline bool WaitFor(const std::atomic<bool> &reached) {
        for (std::size_t i = 0; i < 5000; ++i) {
            if (reached.load())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return reached.load();
    }

    /** @brief Owns process services before the facade so accepted jobs drain before private files disappear. */
    struct PublicationHarness {
        std::shared_ptr<PublicationFiles> files{std::make_shared<PublicationFiles>()};
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
        NavigationBakeServiceConfig config;
        std::unique_ptr<NavigationBakeService> service;
        Navigation::TestSupport::IncrementalBakeFixture fixture;

        explicit PublicationHarness(const std::filesystem::path &root)
            : config{.definition = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value(),
                     .artifactType = Assets::AssetTypeId::Parse("core.navmesh").Value(),
                     .target = AssetCookTargetId::Parse("headless-null").Value(),
                     .cacheRoot = root / "cache",
                     .targetRoot = root / "cooked",
                     .builder = CreateRecastDetourNavigationMeshBuilder().Value(),
                     .files = files,
                     .budget = {.maximumConcurrentJobs = 1,
                                .maximumResidentBytes = 1024ULL * 1024ULL * 1024ULL,
                                .maximumTemporaryBytes = 128U * 1024U * 1024U,
                                .maximumWorkItems = 8,
                                .maximumWorkUnits = 1024ULL * 1024ULL * 1024ULL,
                                .childDrainTimeout = Duration::FromMilliseconds(2000)},
                     .cookLimits = {.maximumArtifactBytes = 8U * 1024U * 1024U},
                     .maximumCandidateBytes = 8U * 1024U * 1024U,
                     .sourceAuthority = std::make_shared<NavigationBakeSourceAuthority>(),
                     .newOperationId = NewPublicationOperationId} {
            REQUIRE(config.sourceAuthority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
            auto created = NavigationBakeService::Create(config, operations, jobs);
            REQUIRE(created.HasValue());
            service = std::move(created).Value();
        }

        [[nodiscard]] OperationId Submit(const CancellationToken &cancellation = {}) {
            REQUIRE(config.sourceAuthority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
            auto submitted = service->Submit({.input = fixture.Input(),
                                              .compatibility = fixture.compatibility,
                                              .tiles = fixture.Tiles(),
                                              .sources = fixture.Observations(),
                                              .cancellation = cancellation});
            REQUIRE(submitted.HasValue());
            return submitted.Value();
        }

        [[nodiscard]] OperationRecord Terminal(const OperationId id) {
            for (std::size_t i = 0; i < 5000; ++i) {
                service->Pump();
                const auto snapshot = operations.SnapshotIfChanged(0);
                const auto found = std::ranges::find(snapshot->operations, id, &OperationRecord::id);
                if (found != snapshot->operations.end() && found->finishedAt)
                    return *found;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("Atomic navigation publication did not reach a terminal state");
            return {};
        }

        [[nodiscard]] Assets::AssetCookGeneration Current() const {
            auto current = Assets::ResolveCurrentCookGeneration(config.targetRoot, config.cookLimits);
            REQUIRE(current.HasValue());
            return std::move(current).Value();
        }

        [[nodiscard]] Assets::AssetCookGenerationContents Contents(const Assets::AssetCookGeneration &generation) const {
            auto contents = Assets::ReadCookGenerationContents(generation, config.maximumCandidateBytes, config.cookLimits);
            REQUIRE(contents.HasValue());
            return std::move(contents).Value();
        }
    };

    [[nodiscard]] inline std::vector<std::uint8_t> PublicationBytes(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.is_open());
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    inline void WritePublicationBytes(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.is_open());
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }

    [[nodiscard]] inline NavigationCookedTileSet DecodePublication(const PublicationHarness &harness,
                                                                   const Assets::AssetCookGeneration &generation) {
        const auto contents = harness.Contents(generation);
        const auto entry = std::ranges::find(contents.entries, harness.config.definition, &Assets::AssetCookManifestEntry::assetId);
        REQUIRE(entry != contents.entries.end());
        const auto index = static_cast<std::size_t>(entry - contents.entries.begin());
        auto envelope = Assets::DecodeCookedArtifact(contents.artifacts[index], harness.config.cookLimits);
        REQUIRE(envelope.HasValue());
        CHECK(envelope.Value().id == harness.config.definition);
        CHECK(envelope.Value().type == harness.config.artifactType);
        CHECK(envelope.Value().target == harness.config.target);
        CHECK(envelope.Value().payloadDigest == ComputeSha256(std::as_bytes(std::span{envelope.Value().payload})));
        auto tiles = DecodeNavigationCookedTileSet(envelope.Value().payload, harness.config.maximumCandidateBytes);
        REQUIRE(tiles.HasValue());
        CHECK(tiles.Value().inputFingerprint == envelope.Value().sourceDigest);
        return std::move(tiles).Value();
    }
}  // namespace Horo::Application::TestSupport
