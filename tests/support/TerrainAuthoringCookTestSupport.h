#pragma once
/** @file TerrainAuthoringCookTestSupport.h @brief Native saved-source cook fixture; no runtime/native-consumer readiness claim. */
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Application/TerrainAuthoringCook.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "NativeCacheFiles.h"
#include "OwnedTestDirectory.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Application::TerrainCookTestSupport {
    using namespace Horo::Terrain;

    inline Assets::AssetId Asset(std::uint8_t value) {
        SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline TerrainAuthoringCookProfile Profile() {
        const auto configuration =
            TerrainConfigurationSnapshot::Create({.configuration = TerrainConfigurationRevision::Create(1).Value(),
                                                  .capability = TerrainCapabilityRevision::Create(1).Value(),
                                                  .tier = TerrainFeatureTier::Ultra,
                                                  .limits = GetTerrainTierProfile(TerrainFeatureTier::Ultra).Value().limits});
        REQUIRE(configuration.HasValue());
        TerrainSourceArtifactProfile artifacts;
        artifacts.tiles.interiorQuads = 8;
        artifacts.tiles.tier = TerrainFeatureTier::Ultra;
        const std::array<std::byte, 1> target{std::byte{1}}, toolchain{std::byte{2}};
        artifacts.tiles.targetDigest = ComputeSha256(target);
        artifacts.tiles.toolchainDigest = ComputeSha256(toolchain);
        TerrainAuthoringCookProfile profile{configuration.Value(),
                                            artifacts,
                                            AssetCookTargetId::Parse("terrain-offline-v1").Value(),
                                            {},
                                            {}};
        profile.assets.maximumSourceBytes = 64 * 1024 * 1024;
        profile.assets.maximumAssets = 16;
        profile.assets.maximumConcurrentCooks = 1;
        return profile;
    }

    struct CookProject final {
        Tests::OwnedTestDirectory directory{"terrain-cook-host"};
        NativeDurableFileSystem files;
        Editor::ProjectMutationCoordinator mutations{files};
        JobSystem jobs;
        Assets::AssetRegistry registry;
        TerrainAuthoringSource source;
        TerrainAuthoringCookProfile profile{Profile()};
        std::unique_ptr<TerrainAuthoringCookHost> host;
        std::uint8_t operation{10};

        CookProject() {
            source.source = {.dataset = TerrainDatasetId::Create(Asset(1).Bytes()).Value(),
                             .sourceAsset = Asset(1),
                             .revision = TerrainSourceRevision::Create(1).Value(),
                             .capability = TerrainCapabilityRevision::Create(1).Value(),
                             .width = 9,
                             .height = 9,
                             .heightsMeters = std::vector<float>(81, 2),
                             .holes = std::vector<std::uint8_t>(81)};
            source.source.holes[40] = 1;
            auto encoded = EncodeTerrainAuthoringSource(source);
            REQUIRE(encoded.HasValue());
            PreparedAuthoredProjectAssetBatch batch{.operationId = "terrain-cook-source",
                                                    .projectRoot = directory.Path(),
                                                    .items = {{Asset(1), Assets::AssetTypeId::Parse("core.terrain").Value(),
                                                               ProjectPath::Parse("assets/terrain.horoasset").Value(),
                                                               std::move(encoded).Value()}}};
            ProjectAssetImportCommitter publisher{mutations, files, &registry};
            REQUIRE(publisher.CommitAuthored(batch).HasValue());
            auto created =
                TerrainAuthoringCookHost::Create({.jobs = &jobs,
                                                  .registry = &registry,
                                                  .mutations = &mutations,
                                                  .cacheFiles = TestSupport::NativeCacheFiles(directory.Path() / "cache"),
                                                  .publicationFiles = std::shared_ptr<DurableFileSystem>(&files,
                                                                                                         [](DurableFileSystem *) {
            }),
                                                  .newPublicationOperationId = [this] {
                return Result<Assets::AssetId>::Success(Asset(operation++));
            }});
            REQUIRE(created.HasValue());
            host = std::move(created).Value();
        }

        std::filesystem::path SourcePath() const {
            return directory.Path() / "assets/terrain.horoasset";
        }

        TerrainAuthoringCookRequest Request() const {
            const auto captured = Assets::CaptureAssetSource(directory.Path(), SourcePath(), 1024 * 1024);
            const auto metadata = Assets::CaptureAssetSource(directory.Path(), SourcePath().string() + ".horo", 4096);
            REQUIRE(captured.HasValue());
            REQUIRE(metadata.HasValue());
            return {.sourceRoot = directory.Path(),
                    .cookedRoot = directory.Path() / "cooked",
                    .asset = Asset(1),
                    .fence = {Asset(2),
                              Editor::TerrainDocumentSessionId::Create(1).Value(),
                              source.source.dataset,
                              source.source.revision,
                              source.source.capability,
                              captured.Value().identity.digest,
                              {}},
                    .acceptedMetadataDigest = metadata.Value().identity.digest,
                    .profile = profile};
        }

        void AwaitTerminal() const {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (host->IsRunning() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE_FALSE(host->IsRunning());
        }

        TerrainAuthoringCookCompletion Completion() const {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;) {
                if (auto completed = host->Poll())
                    return std::move(*completed);
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        Assets::AssetCookReport Cook() {
            REQUIRE(host->Start(Request()).HasValue());
            AwaitTerminal();
            auto completed = Completion();
            INFO("Terrain cook error: " << (completed.result.HasError() ? completed.result.ErrorValue().code.Value()
                                                                        : std::string_view{"none"}));
            REQUIRE(completed.result.HasValue());
            return std::move(completed.result).Value();
        }
    };

}  // namespace Horo::Application::TerrainCookTestSupport
