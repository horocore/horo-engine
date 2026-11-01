#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Application/TerrainAuthoringCook.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "NativeCacheFiles.h"
#include "OwnedTestDirectory.h"
#include "TerrainAuthoringCookTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace Horo;
using namespace Horo::Application;
using namespace Horo::Terrain;
using namespace Horo::Application::TerrainCookTestSupport;

namespace {
    /** @brief Bounds every length before slicing the independently verified Assets artifact. */
    struct BundleReader final {
        std::span<const std::uint8_t> bytes;
        std::size_t at{72};

        std::uint64_t U64() {
            REQUIRE(at <= bytes.size());
            REQUIRE(bytes.size() - at >= 8);
            std::uint64_t value{};
            for (std::size_t i = 0; i < 8; ++i)
                value |= std::uint64_t{bytes[at++]} << (8 * i);
            return value;
        }

        std::span<const std::uint8_t> Payload() {
            const auto size = U64();
            REQUIRE(size <= bytes.size() - at);
            const auto payload = bytes.subspan(at, static_cast<std::size_t>(size));
            at += size;
            return payload;
        }
    };

    void CheckGeometry(CookProject &project, const Assets::AssetCookReport &report) {
        const auto contents = Assets::ReadCookGenerationContents(project.files, report.generation, 4 * 1024 * 1024, project.profile.assets);
        REQUIRE(contents.HasValue());
        REQUIRE(contents.Value().artifacts.size() == 1);
        const auto artifact = Assets::DecodeCookedArtifact(contents.Value().artifacts.front(), project.profile.assets);
        REQUIRE(artifact.HasValue());
        REQUIRE(artifact.Value().id == Asset(1));
        const auto &payload = artifact.Value().payload;
        REQUIRE(payload.size() >= 104);
        REQUIRE(std::string_view(reinterpret_cast<const char *>(payload.data()), 4) == "HTAB");
        const auto body = std::span(payload).first(payload.size() - 32);
        const auto digest = ComputeSha256(std::as_bytes(body));
        CHECK(std::ranges::equal(digest.bytes, std::span(payload).last(32)));
        auto expected = CookTerrainSourceArtifacts(project.source.source, project.profile.artifacts, {}, {});
        REQUIRE(expected.HasValue());
        BundleReader reader{body};
        CHECK(std::ranges::equal(reader.Payload(), expected.Value().Manifest()));
        REQUIRE(reader.U64() == expected.Value().Tiles().tiles.size());
        for (const auto &tile : expected.Value().Tiles().tiles)
            CHECK(std::ranges::equal(reader.Payload(), tile.payload));
        REQUIRE(reader.U64() == 3);
        REQUIRE(expected.Value().Artifacts().size() == 3);
        for (const auto &role : expected.Value().Artifacts()) {
            const auto bytes = reader.Payload();
            CHECK(std::ranges::equal(bytes, role.payload));
            CHECK(VerifyTerrainSourceArtifactPayload(bytes, role.digest).HasValue());
        }
        CHECK(reader.U64() == 0);  // No authored foliage is present in this fixture.
        CHECK(reader.at == body.size());
    }
}  // namespace

TEST_CASE("Terrain cook host publishes real three-role geometry and reuses the exact source cache", "[terrain][cook-host]") {
    CookProject project;
    const auto first = project.Cook();
    CHECK(first.cookedAssets == 1);
    CHECK(first.cacheHits == 0);
    CheckGeometry(project, first);
    const auto second = project.Cook();
    CHECK(second.cookedAssets == 0);
    CHECK(second.cacheHits == 1);
    CheckGeometry(project, second);
}

TEST_CASE("Terrain cook completion remains owned until consumed and survives explicit drain", "[terrain][cook-host]") {
    CookProject project;
    const auto request = project.Request();
    REQUIRE(project.host->Start(request).HasValue());
    project.AwaitTerminal();
    const auto busy = project.host->Start(request);
    REQUIRE(busy.HasError());
    CHECK(busy.ErrorValue().code.Value() == TerrainAuthoringCookErrors::Busy.code.Value());
    project.host->Drain();
    const auto closed = project.host->Start(request);
    REQUIRE(closed.HasError());
    CHECK(closed.ErrorValue().code.Value() == TerrainAuthoringCookErrors::Closed.code.Value());
    auto completed = project.host->Poll();
    REQUIRE(completed.has_value());
    CHECK(completed->fence == request.fence);
    REQUIRE(completed->result.HasValue());
    CheckGeometry(project, completed->result.Value());
    CHECK_FALSE(project.host->Poll().has_value());
}

TEST_CASE("Terrain cook rejects changed saved source without replacing the prior generation", "[terrain][cook-host]") {
    CookProject project;
    const auto prior = project.Cook();
    const auto request = project.Request();
    project.source.source.heightsMeters[0] = 3;
    const auto changed = EncodeTerrainAuthoringSource(project.source);
    REQUIRE(changed.HasValue());
    REQUIRE(project.files.WriteDurable(project.SourcePath(), changed.Value()).HasValue());
    REQUIRE(project.host->Start(request).HasValue());
    project.AwaitTerminal();
    auto completed = project.Completion();
    REQUIRE(completed.result.HasError());
    CHECK(completed.result.ErrorValue().code.Value() == TerrainAuthoringCookErrors::Stale.code.Value());
    const auto current = Assets::ResolveCurrentCookGeneration(project.files, request.cookedRoot, project.profile.assets);
    REQUIRE(current.HasValue());
    CHECK(current.Value().manifestDigest == prior.generation.manifestDigest);
    CHECK(current.Value().generationRoot == prior.generation.generationRoot);
}
