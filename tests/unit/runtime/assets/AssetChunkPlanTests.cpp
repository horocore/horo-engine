#include "Horo/Assets/AssetChunkPlan.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace Horo::Assets;

namespace {
    AssetChunkId Chunk(const char *text) {
        auto parsed = AssetChunkId::Parse(text);
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    AssetId Asset(const char *text) {
        auto parsed = AssetId::Parse(text);
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    AssetChunkDefinition Definition(const char *name, const char *asset, const AssetChunkKind kind = AssetChunkKind::Base) {
        return AssetChunkDefinition{.id = Chunk(name), .kind = kind, .assets = {Asset(asset)}};
    }
}  // namespace

TEST_CASE("Release chunk plan canonicalizes independent input order", "[assets][release]") {
    auto later = Definition("world", "00000000-0000-0000-0000-000000000002", AssetChunkKind::Optional);
    later.dependencies.push_back(Chunk("core"));
    auto earlier = Definition("core", "00000000-0000-0000-0000-000000000001");
    const std::array input{later, earlier};

    auto result = AssetChunkPlan::Create(input);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().Chunks().size() == 2U);
    CHECK(result.Value().Chunks()[0].id.Value() == "core");
    CHECK(result.Value().Chunks()[1].id.Value() == "world");
    CHECK(result.Value().Chunks()[1].dependencies[0].Value() == "core");
}

TEST_CASE("Release chunk plan rejects ambiguous or missing membership", "[assets][release]") {
    const auto first = Definition("core", "00000000-0000-0000-0000-000000000001");
    auto duplicateAsset = Definition("other", "00000000-0000-0000-0000-000000000001");
    CHECK(AssetChunkPlan::Create(std::array{first, duplicateAsset}).HasError());

    duplicateAsset.assets[0] = Asset("00000000-0000-0000-0000-000000000002");
    duplicateAsset.dependencies.push_back(Chunk("missing"));
    CHECK(AssetChunkPlan::Create(std::array{first, duplicateAsset}).HasError());
}

TEST_CASE("Release chunk plan rejects cycles and self dependencies", "[assets][release]") {
    auto first = Definition("a", "00000000-0000-0000-0000-000000000001");
    auto second = Definition("b", "00000000-0000-0000-0000-000000000002");
    first.dependencies.push_back(Chunk("b"));
    second.dependencies.push_back(Chunk("a"));
    CHECK(AssetChunkPlan::Create(std::array{first, second}).HasError());

    first.dependencies[0] = Chunk("a");
    CHECK(AssetChunkPlan::Create(std::array{first, second}).HasError());
}

TEST_CASE("Release chunk plan resolves shared dependencies before dependents", "[assets][release]") {
    auto core = Definition("core", "00000000-0000-0000-0000-000000000001");
    auto audio = Definition("audio", "00000000-0000-0000-0000-000000000002");
    auto world = Definition("world", "00000000-0000-0000-0000-000000000003");
    auto dlc = Definition("dlc", "00000000-0000-0000-0000-000000000004", AssetChunkKind::Optional);
    world.dependencies = {Chunk("core"), Chunk("audio")};
    dlc.dependencies = {Chunk("world")};
    CHECK(AssetChunkPlan::Create(std::array{dlc, world, audio, core}).HasValue());

    audio.dependencies = {Chunk("dlc")};
    CHECK(AssetChunkPlan::Create(std::array{dlc, world, audio, core}).HasError());
}

TEST_CASE("DLC chunks require exact base compatibility evidence", "[assets][release]") {
    auto dlc = Definition("dlc", "00000000-0000-0000-0000-000000000001", AssetChunkKind::Dlc);
    CHECK(AssetChunkPlan::Create(std::array{dlc}).HasError());

    Horo::Sha256Digest base{};
    base.bytes[0] = 1U;
    dlc.requiredBaseManifest = base;
    CHECK(AssetChunkPlan::Create(std::array{dlc}).HasValue());
    dlc.kind = AssetChunkKind::Base;
    CHECK(AssetChunkPlan::Create(std::array{dlc}).HasError());
}

TEST_CASE("Release chunk plan enforces finite bounds", "[assets][release]") {
    const auto core = Definition("core", "00000000-0000-0000-0000-000000000001");
    CHECK(AssetChunkPlan::Create(std::array{core}, {.maximumChunks = 1U, .maximumAssets = 1U}).HasValue());
    CHECK(AssetChunkPlan::Create(std::array{core}, {.maximumChunks = 1U, .maximumAssets = 0U}).HasError());
    CHECK(AssetChunkPlan::Create(std::array{core}, {.maximumChunks = 0U, .maximumAssets = 1U}).HasError());
}
