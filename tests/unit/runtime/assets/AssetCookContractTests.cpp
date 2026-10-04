#include "Horo/Assets/AssetCook.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetCookTargetId Target(const std::string_view value) {
        auto parsed = AssetCookTargetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    Sha256Digest DigestOf(const std::span<const std::uint8_t> bytes) {
        return ComputeSha256(std::as_bytes(bytes));
    }

    Sha256Digest DigestOf(const std::span<const std::byte> bytes) {
        return ComputeSha256(bytes);
    }

    AssetCookArtifact Artifact(const AssetId id, const AssetTypeId type, const AssetCookTargetId target,
                               std::vector<std::uint8_t> payload) {
        AssetCookArtifact artifact;
        artifact.id = id;
        artifact.type = type;
        artifact.target = target;
        artifact.payload = std::move(payload);
        artifact.payloadDigest = DigestOf(std::span<const std::uint8_t>{artifact.payload});
        // The cache-key digest is opaque to this contract; any 32-byte value round-trips.
        artifact.cacheKeyDigest.bytes[0] = 0x11;
        artifact.cacheKeyDigest.bytes[31] = 0x22;
        // The source digest is opaque to this contract; any 32-byte value round-trips.
        artifact.sourceDigest.bytes[0] = 0x33;
        artifact.sourceDigest.bytes[31] = 0x44;
        return artifact;
    }
}  // namespace

TEST_CASE("Cook Target Ids Accept Canonical And Reject Invalid Text", "[unit][runtime][assets][cook]") {
    REQUIRE((AssetCookTargetId::Parse("headless-null").HasValue()));
    REQUIRE((AssetCookTargetId::Parse("desktop-opengl").HasValue()));
    REQUIRE((AssetCookTargetId::Parse("desktop-metal").HasValue()));
    REQUIRE((Target("headless-null").Value() == "headless-null"));

    REQUIRE((AssetCookTargetId::Parse("").HasError()));
    REQUIRE((AssetCookTargetId::Parse("Headless-null").HasError()));
    REQUIRE((AssetCookTargetId::Parse("headless_null").HasError()));
    REQUIRE((AssetCookTargetId::Parse("headless").HasError()));        // single segment, no separator
    REQUIRE((AssetCookTargetId::Parse("-headless-null").HasError()));  // leading dash
    REQUIRE((AssetCookTargetId::Parse("headless-null-").HasError()));  // trailing dash
    REQUIRE((AssetCookTargetId::Parse("headless--null").HasError()));  // empty segment
    REQUIRE((AssetCookTargetId::Parse("headless.null").HasError()));   // dot not allowed
    REQUIRE((AssetCookTargetId::Parse("headless/null").HasError()));   // path separator rejected
    REQUIRE((AssetCookTargetId::Parse("../escape").HasError()));       // root-escape-like
    REQUIRE((AssetCookTargetId::Parse("..").HasError()));              // root-escape-like
    REQUIRE((AssetCookTargetId::Parse("/").HasError()));               // root-escape-like
    REQUIRE((AssetCookTargetId::Parse("headless-0null").HasError()));  // segment must start with letter

    const std::string maximumTarget = "a-" + std::string(MaximumAssetCookTargetIdBytes - 2, 'a');
    REQUIRE((AssetCookTargetId::Parse(maximumTarget).HasValue()));
    REQUIRE((AssetCookTargetId::Parse(maximumTarget + "a").HasError()));
}

TEST_CASE("Cook Target Ids Are Ordered And Comparable", "[unit][runtime][assets][cook]") {
    const auto a = Target("headless-null");
    const auto b = Target("headless-null");
    const auto c = Target("desktop-opengl");
    REQUIRE((a == b));
    REQUIRE((a != c));
    REQUIRE((c < a));
}

TEST_CASE("Cook Limits Provide Sensible Defaults", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    REQUIRE((limits.maximumSourceBytes == 256U * 1024U * 1024U));
    REQUIRE((limits.maximumArtifactBytes == 256U * 1024U * 1024U));
    REQUIRE((limits.maximumAssets == 16U * 1024U));
    REQUIRE((limits.maximumConcurrentCooks == 8));
}

TEST_CASE("Cooked Artifact Round-Trips Through Binary Envelope", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    const auto artifact =
        Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {0xDE, 0xAD, 0xBE, 0xEF});
    auto encoded = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encoded.HasValue()));
    auto decoded = DecodeCookedArtifact(encoded.Value(), limits);
    REQUIRE((decoded.HasValue()));
    REQUIRE((decoded.Value().id == artifact.id));
    REQUIRE((decoded.Value().type == artifact.type));
    REQUIRE((decoded.Value().target == artifact.target));
    REQUIRE((decoded.Value().cacheKeyDigest == artifact.cacheKeyDigest));
    REQUIRE((decoded.Value().sourceDigest == artifact.sourceDigest));
    REQUIRE((decoded.Value().payloadDigest == artifact.payloadDigest));
    REQUIRE((decoded.Value().payload == artifact.payload));
    auto byteDecoded = DecodeCookedArtifactBytes(std::as_bytes(std::span{encoded.Value()}), limits);
    REQUIRE(byteDecoded.HasValue());
    CHECK(byteDecoded.Value().id == decoded.Value().id);
    CHECK(byteDecoded.Value().payload == decoded.Value().payload);
    auto damaged = encoded.Value();
    damaged.back() ^= 0x01;
    CHECK(DecodeCookedArtifactBytes(std::as_bytes(std::span{damaged}), limits).HasError());
    CHECK(DecodeCookedArtifactBytes({}, limits).HasError());
}

TEST_CASE("Cooked Artifact Round-Tips Cache-Key Digest Verbatim", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    auto artifact = Artifact(Id("11112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {1, 2, 3, 4});
    for (std::size_t i = 0; i < artifact.cacheKeyDigest.bytes.size(); ++i)
        artifact.cacheKeyDigest.bytes[i] = static_cast<std::uint8_t>(0xF0 + i);

    auto encoded = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encoded.HasValue()));
    auto decoded = DecodeCookedArtifact(encoded.Value(), limits);
    REQUIRE((decoded.HasValue()));
    REQUIRE((decoded.Value().cacheKeyDigest == artifact.cacheKeyDigest));
}

TEST_CASE("Cooked Artifact Admits Empty Payload", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {});
    auto encoded = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encoded.HasValue()));
    auto decoded = DecodeCookedArtifact(encoded.Value(), limits);
    REQUIRE((decoded.HasValue()));
    REQUIRE((decoded.Value().payload.empty()));
    REQUIRE((decoded.Value().payloadDigest == artifact.payloadDigest));
}

TEST_CASE("Cooked Artifact Rejects Unsupported Format Version", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {1, 2, 3});
    auto encodedResult = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encodedResult.HasValue()));
    auto encoded = std::move(encodedResult).Value();
    REQUIRE((encoded.size() >= 13));
    // The format version is a u32 LE at offset 8 (after the 8-byte magic).
    encoded[8] = static_cast<std::uint8_t>(99);
    auto decoded = DecodeCookedArtifact(encoded, limits);
    REQUIRE((decoded.HasError()));
    REQUIRE((decoded.ErrorValue().code.Value() == "asset.cook.unsupported_format"));
}

TEST_CASE("Cooked Artifact Rejects Bad Magic", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {1});
    auto encodedResult = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encodedResult.HasValue()));
    auto encoded = std::move(encodedResult).Value();
    encoded[0] = 0x00;
    auto decoded = DecodeCookedArtifact(encoded, limits);
    REQUIRE((decoded.HasError()));
    REQUIRE((decoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact"));
}

TEST_CASE("Cooked Artifact Rejects Payload Digest Mismatch", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {1, 2, 3, 4});
    // Tamper with the payload digest so decode verification fails.
    artifact.payloadDigest.bytes[0] ^= 0xFF;
    auto encodedResult = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encodedResult.HasValue()));
    auto decoded = DecodeCookedArtifact(encodedResult.Value(), limits);
    REQUIRE((decoded.HasError()));
    REQUIRE((decoded.ErrorValue().code.Value() == "asset.cook.hash_mismatch"));
}

TEST_CASE("Cooked Artifact Rejects Too-Large Payload", "[unit][runtime][assets][cook]") {
    AssetCookLimits limits;
    limits.maximumArtifactBytes = 64;
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"),
                                   std::vector<std::uint8_t>(128, 0x7A));
    auto encoded = EncodeCookedArtifact(artifact, limits);
    // Encoder must reject oversized artifacts before producing bytes.
    REQUIRE((encoded.HasError()));
    REQUIRE((encoded.ErrorValue().code.Value() == "asset.cook.too_large"));
}

TEST_CASE("Cooked Artifact Rejects Trailing Bytes", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {1, 2, 3});
    auto encodedResult = EncodeCookedArtifact(artifact, limits);
    REQUIRE((encodedResult.HasValue()));
    auto encoded = std::move(encodedResult).Value();
    encoded.push_back(0x00);
    auto decoded = DecodeCookedArtifact(encoded, limits);
    REQUIRE((decoded.HasError()));
    REQUIRE((decoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact"));
}

TEST_CASE("Cooked Artifact Rejects Truncated Input", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    std::vector<std::uint8_t> truncated{'H', 'O', 'R', 'O'};
    auto decoded = DecodeCookedArtifact(truncated, limits);
    REQUIRE((decoded.HasError()));
    REQUIRE((decoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact"));
}

TEST_CASE("Cooked Artifact Rejects Empty Input", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    std::vector<std::uint8_t> empty;
    auto decoded = DecodeCookedArtifact(empty, limits);
    REQUIRE((decoded.HasError()));
    REQUIRE((decoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact"));
}

TEST_CASE("Cooked Artifact Rejects Text Fields Outside Envelope", "[unit][runtime][assets][cook]") {
    const AssetCookLimits limits;
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {});
    auto encodedResult = EncodeCookedArtifact(artifact, limits);
    REQUIRE(encodedResult.HasValue());
    auto encoded = std::move(encodedResult).Value();

    SECTION("Target length exceeds remaining bytes") {
        encoded[12] = 0xFF;
        encoded[13] = 0xFF;
    }
    SECTION("Type length exceeds remaining bytes") {
        encoded[14] = 0xFF;
        encoded[15] = 0xFF;
    }
    SECTION("Text is truncated after the fixed header") {
        encoded.resize(136);
    }

    const auto decoded = DecodeCookedArtifact(encoded, limits);
    REQUIRE(decoded.HasError());
    CHECK(decoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact");
    const auto byteDecoded = DecodeCookedArtifactBytes(std::as_bytes(std::span{encoded}), limits);
    REQUIRE(byteDecoded.HasError());
    CHECK(byteDecoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact");
}

TEST_CASE("Cooked Artifact Rejects Wrapped Envelope Length", "[unit][runtime][assets][cook]") {
    AssetCookLimits limits;
    limits.maximumArtifactBytes = std::numeric_limits<std::uint64_t>::max();
    const auto artifact = Artifact(Id("00112233-4455-6677-8899-aabbccddeeff"), Type("core.mesh"), Target("headless-null"), {});
    auto encodedResult = EncodeCookedArtifact(artifact, limits);
    REQUIRE(encodedResult.HasValue());
    auto encoded = std::move(encodedResult).Value();
    encoded[12] = 0xFF;
    encoded[13] = 0xFF;
    const std::uint64_t headerEnd = 136 + 65535 + artifact.type.Value().size();
    // This payload length made the old headerEnd + payloadSize check wrap to encoded.size().
    const auto payloadSize = std::numeric_limits<std::uint64_t>::max() - (headerEnd - encoded.size()) + 1;
    for (std::size_t i = 0; i < 8; ++i)
        encoded[128 + i] = static_cast<std::uint8_t>(payloadSize >> (8 * i));

    const auto decoded = DecodeCookedArtifactBytes(std::as_bytes(std::span{encoded}), limits);
    REQUIRE(decoded.HasError());
    CHECK(decoded.ErrorValue().code.Value() == "asset.cook.malformed_artifact");
}
