#pragma once

#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "NativeCacheFiles.h"
#include "PublicationOperationId.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavMeshAssetTestFixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

namespace Horo::Navigation::AssetTestSupport {
    /** @brief Execute the authoritative native producer and read its durable published generation. */
    [[nodiscard]] inline std::vector<std::uint8_t> BakeCanonicalContent(const std::filesystem::path &projectRoot,
                                                                        std::span<const NavigationBakeScopeId> scopes = {}) {
        const auto canonicalRoot = std::filesystem::canonical(projectRoot);
        TestSupport::IncrementalBakeFixture input;
        if (!scopes.empty())
            input.scopes.assign(scopes.begin(), scopes.end());
        OperationStore operations{8, 16};
        JobSystem bakeJobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
        Application::NavigationBakeServiceConfig config{.definition = Asset(),
                                                        .artifactType = Type(),
                                                        .target = Target(),
                                                        .cacheFiles = Horo::TestSupport::NativeCacheFiles(canonicalRoot / "tile-cache"),
                                                        .targetRoot = canonicalRoot / "cook-output",
                                                        .builder = CreateRecastDetourNavigationMeshBuilder().Value(),
                                                        .files = std::make_shared<NativeDurableFileSystem>(),
                                                        .budget = {1, 1024ULL * 1024ULL * 1024ULL, 128U * 1024U * 1024U, 8,
                                                                   1024ULL * 1024ULL * 1024ULL, Duration::FromMilliseconds(2000)},
                                                        .sourceAuthority = std::make_shared<Application::NavigationBakeSourceAuthority>(),
                                                        .newOperationId = Horo::TestSupport::NewPublicationOperationId};
        REQUIRE(config.sourceAuthority->UpdateCurrent(input.revisions, input.Observations()).HasValue());
        auto created = Application::NavigationBakeService::Create(config, operations, bakeJobs);
        REQUIRE(created.HasValue());
        auto bake = std::move(created).Value();
        REQUIRE(bake->Submit({.input = input.Input(),
                              .compatibility = input.compatibility,
                              .tiles = input.Tiles(),
                              .sources = input.Observations(),
                              .authoredSurfaces = input.SurfaceCatalog()})
                    .HasValue());
        for (std::size_t iteration = 0; iteration < 5000 && !bake->Published(); ++iteration) {
            bake->Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(bake->Published());
        const auto current = Assets::ResolveCurrentCookGeneration(config.targetRoot);
        REQUIRE(current.HasValue());
        REQUIRE(current.Value().manifestDigest == bake->Published()->generation.manifestDigest);
        const auto contents = Assets::ReadCookGenerationContents(current.Value(), config.maximumCandidateBytes);
        REQUIRE(contents.HasValue());
        REQUIRE(contents.Value().artifacts.size() == 1);
        return contents.Value().artifacts.front();
    }
}  // namespace Horo::Navigation::AssetTestSupport
