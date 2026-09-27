#include "Horo/Assets/AssetArchive.h"
#include "Horo/Assets/AssetCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <utility>
#include <vector>

using namespace Horo;
using namespace Horo::Assets;

namespace {
    AssetId Id(const char *text) {
        auto value = AssetId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    AssetChunkId Chunk(const char *text) {
        auto value = AssetChunkId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    AssetCookTargetId Target(const char *text) {
        auto value = AssetCookTargetId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    AssetArchiveInput Cooked(const AssetId id, const AssetCookTargetId &target, std::vector<std::uint8_t> payload) {
        AssetCookArtifact artifact;
        artifact.id = id;
        artifact.type = AssetTypeId::Parse("core.mesh").Value();
        artifact.target = target;
        artifact.payload = std::move(payload);
        artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
        auto encoded = EncodeCookedArtifact(artifact);
        REQUIRE(encoded.HasValue());
        return {id, std::move(encoded).Value()};
    }

    AssetChunkPlan Plan(const AssetId first, const AssetId second) {
        const std::array chunks{AssetChunkDefinition{.id = Chunk("core"), .assets = {first}},
                                AssetChunkDefinition{.id = Chunk("world"),
                                                     .kind = AssetChunkKind::Optional,
                                                     .assets = {second},
                                                     .dependencies = {Chunk("core")}}};
        auto plan = AssetChunkPlan::Create(chunks);
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }
}  // namespace

TEST_CASE("Release archive is deterministic and provides exact cooked bytes", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto plan = Plan(first, second);
    const auto core = Cooked(first, target, {1U, 2U, 3U});
    const auto world = Cooked(second, target, {4U, 5U});
    const std::array forward{core, world};
    const std::array reverse{world, core};

    auto encoded = BuildAssetArchive(plan, target, forward);
    auto reordered = BuildAssetArchive(plan, target, reverse);
    REQUIRE(encoded.HasValue());
    REQUIRE(reordered.HasValue());
    CHECK(encoded.Value() == reordered.Value());

    auto opened = AssetArchiveProvider::Open(encoded.Value(), target);
    REQUIRE(opened.HasValue());
    auto provider = std::move(opened).Value();
    const CancellationToken cancellation;
    auto exists = provider.Exists(first, cancellation);
    REQUIRE(exists.HasValue());
    CHECK(exists.Value());
    auto loaded = provider.Load(world.id, cancellation);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value() == world.bytes);
    CHECK(provider.Load(Id("00000000-0000-0000-0000-000000000003"), cancellation).HasError());
}

TEST_CASE("Release archive rejects missing, extra, or wrong-target cooked artifacts", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto plan = Plan(first, second);
    const auto core = Cooked(first, target, {1U});
    const auto wrongTarget = Cooked(second, Target("desktop-opengl"), {2U});
    CHECK(BuildAssetArchive(plan, target, std::array{core}).HasError());
    CHECK(BuildAssetArchive(plan, target, std::array{core, core}).HasError());
    CHECK(BuildAssetArchive(plan, target, std::array{core, wrongTarget}).HasError());
}

TEST_CASE("Release archive fails closed on tampering and unsupported target", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto inputs = std::array{Cooked(first, target, {1U}), Cooked(second, target, {2U})};
    auto encoded = BuildAssetArchive(Plan(first, second), target, inputs);
    REQUIRE(encoded.HasValue());

    CHECK(AssetArchiveProvider::Open(encoded.Value(), Target("desktop-opengl")).HasError());
    auto tampered = encoded.Value();
    tampered[tampered.size() / 2U] ^= 1U;
    CHECK(AssetArchiveProvider::Open(tampered, target).HasError());
    tampered = encoded.Value();
    tampered[8U] = 2U;
    const auto digest = ComputeSha256(std::as_bytes(std::span{tampered.data(), tampered.size() - 32U}));
    std::copy(digest.bytes.begin(), digest.bytes.end(), tampered.end() - 32);
    CHECK(AssetArchiveProvider::Open(tampered, target).HasError());

    tampered = encoded.Value();
    tampered[12U] = 1U;
    const auto featureDigest = ComputeSha256(std::as_bytes(std::span{tampered.data(), tampered.size() - 32U}));
    std::copy(featureDigest.bytes.begin(), featureDigest.bytes.end(), tampered.end() - 32);
    CHECK(AssetArchiveProvider::Open(tampered, target).HasError());
}
