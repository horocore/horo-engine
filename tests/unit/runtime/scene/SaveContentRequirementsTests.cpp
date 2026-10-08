#include "Horo/Runtime/Scene/SaveContentRequirements.h"

#include <catch2/catch_test_macros.hpp>
#include <format>

using namespace Horo;
using namespace Horo::Runtime;

namespace {
    std::vector<SaveContentRequirement> Requirements() {
        const auto owner = SaveParticipantId::Parse("project.content.test").Value();
        Sha256Digest digest;
        digest.bytes[0] = 17;
        return {{owner, SaveContentNecessity::Required,
                 SaveAssetContentRequirement{Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value(),
                                             Assets::AssetTypeId::Parse("core.scene").Value(), digest}},
                {owner, SaveContentNecessity::Optional,
                 SaveChunkContentRequirement{Assets::AssetChunkId::Parse("dlc_expansion").Value(), Assets::AssetChunkKind::Dlc}},
                {owner, SaveContentNecessity::Required,
                 SaveModuleContentRequirement{SaveParticipantId::Parse("project.gameplay").Value(), 7}}};
    }
}  // namespace

TEST_CASE("Content declaration schema retains typed identities and rejects duplicate or contradictory requirements",
          "[scene][save][content]") {
    const auto requirements = Requirements();
    auto encoded = EncodeSaveContentRequirements(requirements);
    REQUIRE(encoded.HasValue());
    auto decoded = DecodeSaveContentRequirements(encoded.Value().Bytes());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value() == requirements);
    auto repeated = requirements;
    repeated.insert(repeated.begin() + 1, repeated.front());
    CHECK(EncodeSaveContentRequirements(repeated).HasError());
    std::get<SaveAssetContentRequirement>(repeated[1].content).envelopeDigest.bytes[0] = 19;
    CHECK(EncodeSaveContentRequirements(repeated).HasError());
    auto outOfOrder = requirements;
    std::swap(outOfOrder.front(), outOfOrder.back());
    CHECK(EncodeSaveContentRequirements(outOfOrder).HasError());
    auto invalid = requirements;
    std::get<SaveModuleContentRequirement>(invalid.back().content).version = 0;
    CHECK(EncodeSaveContentRequirements(invalid).HasError());
    invalid = requirements;
    invalid.front().necessity = static_cast<SaveContentNecessity>(255);
    CHECK(EncodeSaveContentRequirements(invalid).HasError());
}

TEST_CASE("Content declaration rejects every truncated prefix, newer schema, trailing bytes and oversized inventories",
          "[scene][save][content]") {
    auto encoded = EncodeSaveContentRequirements(Requirements());
    REQUIRE(encoded.HasValue());
    const auto bytes = encoded.Value().Bytes();
    for (std::size_t length = 0; length < bytes.size(); ++length)
        REQUIRE(DecodeSaveContentRequirements(bytes.first(length)).HasError());
    std::vector<std::byte> newer(bytes.begin(), bytes.end());
    newer.front() = std::byte{2};
    CHECK(DecodeSaveContentRequirements(newer).HasError());
    std::vector<std::byte> trailing(bytes.begin(), bytes.end());
    trailing.push_back(std::byte{0});
    CHECK(DecodeSaveContentRequirements(trailing).HasError());
    std::vector<SaveContentRequirement> oversized(MaximumSaveContentRequirements + 1, Requirements().front());
    CHECK(EncodeSaveContentRequirements(oversized).HasError());
}

TEST_CASE("Content declaration admits the full unique inventory capacity and rejects the next entry", "[scene][save][content][capacity]") {
    const auto owner = SaveParticipantId::Parse("project.content.test").Value();
    std::vector<SaveContentRequirement> requirements;
    requirements.reserve(MaximumSaveContentRequirements + 1);
    for (std::size_t index = 0; index < MaximumSaveContentRequirements; ++index) {
        auto module = SaveParticipantId::Parse(std::format("project.module.m{:04}", index));
        REQUIRE(module.HasValue());
        requirements.push_back({owner, SaveContentNecessity::Optional, SaveModuleContentRequirement{std::move(module).Value(), 1}});
    }
    auto encoded = EncodeSaveContentRequirements(requirements);
    REQUIRE(encoded.HasValue());
    auto decoded = DecodeSaveContentRequirements(encoded.Value().Bytes());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value() == requirements);
    requirements.push_back(
        {owner, SaveContentNecessity::Optional, SaveModuleContentRequirement{SaveParticipantId::Parse("project.module.m1024").Value(), 1}});
    CHECK(EncodeSaveContentRequirements(requirements).HasError());
}
