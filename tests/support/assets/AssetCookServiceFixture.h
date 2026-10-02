#pragma once

#include "HeadlessMeshCooker.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Sha256.h"
#include "assets/AssetCookPublicationFixture.h"
#include "assets/AssetCookTestValues.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets::ServiceTestSupport {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Assets::CookTestValues;

    inline AssetRecord TestMeshRecord() {
        const auto sourcePath = ProjectPath::Parse("assets/test_mesh.fbx");
        const auto metadataPath = ProjectPath::Parse("assets/test_mesh.fbx.horo");
        REQUIRE(sourcePath.HasValue());
        REQUIRE(metadataPath.HasValue());
        return AssetRecord{.id = Id("00000000-0000-0000-0000-0000000000a1"),
                           .type = Type("core.mesh"),
                           .sourcePath = sourcePath.Value(),
                           .metadataPath = metadataPath.Value()};
    }

    struct TempDir final : OwnedCookTestDirectory {
        TempDir() : OwnedCookTestDirectory("horo_service_test", false) {}
    };

    /** @brief Creates a minimal file with given content. */
    inline void WriteFile(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    /** @brief Creates a fake .horo sidecar so the registry picks up the file. */
    inline std::string SidecarJson(std::string_view assetId, std::string_view assetType) {
        return std::string("{\"schemaVersion\":1,\"assetId\":\"") + std::string(assetId) + "\",\"assetType\":\"" + std::string(assetType) +
               "\"}";
    }

    /**
     * @brief Sets up a minimal project directory structure with one source asset and sidecar.
     */
    struct TestProject {
        TempDir dir;
        std::filesystem::path assetsDir;
        std::filesystem::path sourceFile;

        TestProject() {
            assetsDir = dir.path / "assets";
            std::filesystem::create_directories(assetsDir);

            // Create a minimal source file
            sourceFile = assetsDir / "test_mesh.fbx";
            std::vector<std::uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
            WriteFile(sourceFile, data);

            // Create sidecar
            auto sidecarJson = SidecarJson("00000000-0000-0000-0000-0000000000a1", "core.mesh");
            auto sidecarBytes =
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sidecarJson.data()), sidecarJson.size());
            WriteFile(std::string(sourceFile.string()) + ".horo", sidecarBytes);
        }
    };

    /** @brief Adds a second distinct source asset for concurrent cancellation checks. */
    inline AssetRecord AddSecondMesh(TestProject &project) {
        const std::filesystem::path sourceFile = project.assetsDir / "second_mesh.fbx";
        std::filesystem::copy_file(project.sourceFile, sourceFile);
        const std::string sidecarJson = SidecarJson("00000000-0000-0000-0000-0000000000a2", "core.mesh");
        const auto sidecarBytes =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sidecarJson.data()), sidecarJson.size());
        WriteFile(std::string(sourceFile.string()) + ".horo", sidecarBytes);
        AssetRecord record = TestMeshRecord();
        record.id = Id("00000000-0000-0000-0000-0000000000a2");
        record.sourcePath = ProjectPath::Parse("assets/second_mesh.fbx").Value();
        record.metadataPath = ProjectPath::Parse("assets/second_mesh.fbx.horo").Value();
        return record;
    }

    /** @brief Requests cancellation while returning either an acknowledged cook cancellation or a real failure. */
    class CancellingCooker final : public ICookerStrategy {
    public:
        CancellingCooker(CancellationSource &source, const bool fail) : source_(source), fail_(fail) {}

        [[nodiscard]] Result<CookOutputSink> Cook(const CookSourceView &, const CancellationToken &) const override {
            source_.RequestCancellation();
            return Result<CookOutputSink>::Failure(Error{ErrorCode{fail_ ? "test.cook.failed" : "asset.cook.cancelled"}});
        }

    private:
        CancellationSource &source_;
        bool fail_;
    };

    /** @brief Test strategy whose effective settings must participate in AST cache admission. */
    class SettingsCooker final : public ICookerStrategy {
    public:
        explicit SettingsCooker(const std::uint8_t setting) : setting_(setting) {}

        [[nodiscard]] CookerCacheIdentity CacheIdentity() const noexcept override {
            const std::array<std::byte, 1> bytes{static_cast<std::byte>(setting_)};
            return {.version = "test.1", .settingsDigest = ComputeSha256(bytes), .settingsSchemaVersion = 1};
        }

        [[nodiscard]] Result<CookOutputSink> Cook(const CookSourceView &, const CancellationToken &) const override {
            return Result<CookOutputSink>::Success(CookOutputSink{.payload = {setting_}});
        }

        [[nodiscard]] Result<void> ValidateCookedPayload(const CookSourceView &,
                                                         const std::span<const std::uint8_t> payload) const override {
            return payload.size() == 1 && payload.front() == setting_
                       ? Result<void>::Success()
                       : Result<void>::Failure(Error{ErrorCode{"test.cook.payload_mismatch"}});
        }

    private:
        std::uint8_t setting_{};
    };

    /** @brief Verifies that the initial cook and later cache hit retain source attribution. */
    inline void AssertCachedCookOutput(const BuildOutputSnapshot &first, const BuildOutputSnapshot &second,
                                       const std::filesystem::path &sourcePath) {
        const auto cached = std::ranges::find_if(second.records, [](const BuildOutputRecord &record) {
            return record.code.Value() == "asset.cook.cache_hit";
        });
        REQUIRE((cached != second.records.end()));
        REQUIRE((cached->result == BuildOutputResult::Cached));
        REQUIRE(cached->source.has_value());
        REQUIRE(cached->source->absolutePath == sourcePath.string());
        REQUIRE(cached->sessionId.has_value());
        REQUIRE((cached->sessionId == second.records.back().sessionId));
        REQUIRE((second.records.back().result == BuildOutputResult::Succeeded));

        const auto cooked = std::ranges::find_if(first.records, [](const BuildOutputRecord &record) {
            return record.code.Value() == "asset.cook.asset_cooked";
        });
        REQUIRE(cooked != first.records.end());
        REQUIRE(cooked->result == BuildOutputResult::Succeeded);
        REQUIRE(cooked->source.has_value());
        REQUIRE(cooked->source->absolutePath == sourcePath.string());
    }

    /** @brief Checks that both a failed or cancelled cook and its cancelled sibling retain source attribution. */
    inline void AssertCancelledSiblingOutput(const BuildOutputSnapshot &snapshot, const OperationRecord &operation,
                                             const TestProject &project, const bool fail) {
        REQUIRE(snapshot.records.back().result == (fail ? BuildOutputResult::Failed : BuildOutputResult::Cancelled));
        const auto assetFailure = std::ranges::find_if(snapshot.records, [](const BuildOutputRecord &record) {
            return record.source.has_value();
        });
        REQUIRE(assetFailure != snapshot.records.end());
        REQUIRE(assetFailure->result == (fail ? BuildOutputResult::Failed : BuildOutputResult::Cancelled));
        REQUIRE(assetFailure->source->absolutePath == project.sourceFile.string());
        REQUIRE(assetFailure->operationId == operation.id);
        REQUIRE(assetFailure->sessionId == snapshot.records.back().sessionId);
        const std::filesystem::path secondSource = project.assetsDir / "second_mesh.fbx";
        REQUIRE(std::ranges::count_if(snapshot.records, [&](const BuildOutputRecord &record) {
            return record.source.has_value() && record.source->absolutePath == secondSource.string() &&
                   record.result == BuildOutputResult::Cancelled;
        }) == 1);
    }

    /** @brief Owns a real native publication callback so cancellation can be injected on either side of replacement. */
    class CookPublicationFiles final : public DurableFileSystem {
    public:
        NativeDurableFileSystem native;
        CancellationSource *cancellation{};
        bool cancelAfterCommit{};
        bool failCommittedSync{};
        bool committed{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, const std::string_view owner) override {
            return native.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            auto written = native.WriteDurable(path, bytes);
            if (written.HasValue() && cancellation && !cancelAfterCommit && path.filename() == "current.json")
                cancellation->RequestCancellation();
            return written;
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            return native.AtomicReplace(prepared, destination);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (receipt.WasCommitted() && destination.filename() == "current.json" && prepared.filename() != "unpublished.current.json") {
                committed = true;
                if (cancellation && cancelAfterCommit)
                    cancellation->RequestCancellation();
                if (failCommittedSync)
                    return Result<void>::Failure(Error{ErrorCode{"test.cook.publication_sync_failed"}});
            }
            return replaced;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native.SyncDirectory(path);
        }
    };

    /** @brief Creates an empty full-cook host with genuine publication files and entropy. */
    struct EmptyCookPublicationFixture {
        TempDir source;
        TempDir cache;
        TempDir cooked;
        JobSystem jobs;
        AssetCookService service{jobs, Catalog()};
        AssetCookRequest request{.sourceRoot = source.path,
                                 .cacheRoot = cache.path,
                                 .cookedRoot = cooked.path,
                                 .registry = AssetRegistry{}.Snapshot(),
                                 .target = Target("headless-null")};

        EmptyCookPublicationFixture() {
            Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
        }

    private:
        [[nodiscard]] static std::shared_ptr<const CookerCatalogSnapshot> Catalog() {
            CookerCatalog catalog;
            REQUIRE(RegisterHeadlessMeshCooker(catalog).HasValue());
            auto snapshot = catalog.Publish();
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }
    };

    /** @brief Exercises both standard and foreign exceptions from optional operation-history delivery. */
    class ThrowingCookHistorySink final : public IOperationHistorySink {
    public:
        bool nonstandard{};
        std::size_t attempted{};

        void AppendTerminal(const OperationRecord &) override {
            ++attempted;
            if (nonstandard)
                throw 73;
            throw std::runtime_error("Optional cook history notification failed");
        }
    };

    /** @brief Verifies that an empty cook has one successful operation and one consistently attributed terminal output. */
    inline void AssertEmptyCookOutputScopes(const BuildOutputStore &buildOutput, const OperationStore &operations) {
        const auto buildSnapshot = buildOutput.SnapshotIfChanged(0);
        REQUIRE(buildSnapshot.has_value());
        REQUIRE((buildSnapshot->records.size() == 2U));
        REQUIRE((buildSnapshot->records.back().result == BuildOutputResult::Succeeded));
        REQUIRE((buildSnapshot->records.back().code.Value() == "asset.cook.succeeded"));
        REQUIRE((std::ranges::count_if(buildSnapshot->records, [](const BuildOutputRecord &record) {
            return record.result != BuildOutputResult::None;
        }) == 1));
        const auto operationSnapshot = operations.SnapshotIfChanged(0);
        REQUIRE(operationSnapshot.has_value());
        REQUIRE((operationSnapshot->operations.size() == 1));
        REQUIRE((operationSnapshot->operations.front().state == OperationState::Succeeded));
        for (const BuildOutputRecord &record : buildSnapshot->records) {
            REQUIRE(record.sessionId.has_value());
            REQUIRE(record.sessionId->IsValid());
            REQUIRE((record.sessionId == buildSnapshot->records.front().sessionId));
            REQUIRE((record.operationId == operationSnapshot->operations.front().id));
        }
    }
}  // namespace Horo::Assets::ServiceTestSupport
