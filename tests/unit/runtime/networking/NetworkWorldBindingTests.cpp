#include "Horo/Network/NetworkWorldBinding.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
    using namespace Horo::Network;

    NetworkWorldBindingRecord InitialBinding() {
        return {NetworkWorldBindingKind::Initial,
                NetworkSessionGeneration::Create(17).Value(),
                ReplicationAuthorityEpoch::Create(29).Value(),
                41,
                {}};
    }
}  // namespace

TEST_CASE("World binding v1 preserves exact identity bytes without a local Scene or peer handle", "[network][world_binding]") {
    const auto record = InitialBinding();
    const auto encoded = EncodeNetworkWorldBinding(record);
    REQUIRE(encoded.HasValue());
    REQUIRE(encoded.Value().size() == 60);
    REQUIRE(encoded.Value()[0] == std::byte{0});
    REQUIRE(encoded.Value()[1] == std::byte{1});
    REQUIRE(encoded.Value()[11] == std::byte{17});
    REQUIRE(encoded.Value()[19] == std::byte{29});
    REQUIRE(encoded.Value()[27] == std::byte{41});
    const auto decoded = DecodeNetworkWorldBinding(encoded.Value());
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value() == record);
    REQUIRE(NetworkWorldBindingDigest(record).HasValue());
}

TEST_CASE("World binding rejects incomplete unknown truncated and trailing representations", "[network][world_binding]") {
    auto record = InitialBinding();
    record.kind = NetworkWorldBindingKind::Count;
    REQUIRE(EncodeNetworkWorldBinding(record).HasError());
    record = InitialBinding();
    record.schemaFingerprint = 0;
    REQUIRE(EncodeNetworkWorldBinding(record).HasError());
    auto result = EncodeNetworkWorldBinding(InitialBinding());
    REQUIRE(result.HasValue());
    auto encoded = std::move(result).Value();
    REQUIRE(DecodeNetworkWorldBinding(std::span{encoded}.first(59)).HasError());
    std::array<std::byte, 61> trailing{};
    std::copy(encoded.begin(), encoded.end(), trailing.begin());
    REQUIRE(DecodeNetworkWorldBinding(trailing).HasError());
    encoded[1] = std::byte{2};
    REQUIRE(DecodeNetworkWorldBinding(encoded).HasError());
    encoded[1] = std::byte{1};
    encoded[3] = std::byte{0xff};
    REQUIRE(DecodeNetworkWorldBinding(encoded).HasError());
}

TEST_CASE("World travel handoff requires the exact prior binding and distinguishes prepare from commit", "[network][world_binding]") {
    auto replacement = InitialBinding();
    replacement.kind = NetworkWorldBindingKind::PrepareTravel;
    REQUIRE(EncodeNetworkWorldBinding(replacement).HasError());
    const auto prior = NetworkWorldBindingDigest(InitialBinding());
    REQUIRE(prior.HasValue());
    replacement.expectedPriorBinding = prior.Value();
    const auto prepared = EncodeNetworkWorldBinding(replacement);
    REQUIRE(prepared.HasValue());
    replacement.kind = NetworkWorldBindingKind::CommitTravel;
    const auto committed = EncodeNetworkWorldBinding(replacement);
    REQUIRE(committed.HasValue());
    REQUIRE(prepared.Value() != committed.Value());
    REQUIRE(DecodeNetworkWorldBinding(committed.Value()).Value() == replacement);
    replacement.kind = NetworkWorldBindingKind::Initial;
    REQUIRE(EncodeNetworkWorldBinding(replacement).HasError());
}
