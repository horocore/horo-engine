#include "HostSignatureDocument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::Application::Internal::Tests {
    namespace {
        std::span<const std::byte> Bytes(const std::string_view text) {
            return std::as_bytes(std::span{text.data(), text.size()});
        }

        std::string TrustDocument() {
            return R"({"schemaVersion":1,"allowUnsigned":false,"publishers":[{"publisherId":"com.horo.test","keyId":"release-1","algorithm":"ecdsa-p256-sha256","publicKeyHex":")" +
                   std::string{"04"} + std::string(128, '1') + R"(","expiresAtUnixMilliseconds":12345,"revoked":true}]})";
        }

        Packages::PackagePublisherVerificationPolicy SeedPolicy() {
            Packages::PackagePublisherVerificationPolicy seed;
            seed.allowUnsigned = true;
            seed.auditCapacity = 7;
            seed.limits = {1234, 2, 40, 48, 65, 64};
            auto publisher = Packages::PackagePublisherId::Parse("com.horo.existing");
            REQUIRE(publisher.HasValue());
            seed.publishers.push_back({std::move(publisher).Value(), "existing-key", Security::SignatureAlgorithm::EcdsaP256Sha256,
                                       std::vector<std::byte>(65, std::byte{0x04}), 99999, false});
            return seed;
        }
    }  // namespace

    TEST_CASE("Host and package author share exact detached envelope bytes without issuing verification", "[packages][host][security]") {
        Security::DetachedSignatureEnvelope source{.publisherId = "com.horo.test",
                                                   .keyId = "release-1",
                                                   .artifactDigest = ComputeSha256(Bytes("actual artifact")),
                                                   .signature = std::vector<std::byte>(64, std::byte{0x01})};
        const auto encoded = EncodeDetachedSignatureDocument(source);
        const auto decoded = DecodeDetachedSignatureDocument(Bytes(encoded));
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().publisherId == source.publisherId);
        CHECK(decoded.Value().keyId == source.keyId);
        CHECK(decoded.Value().artifactDigest == source.artifactDigest);
        CHECK(decoded.Value().signature == source.signature);
        CHECK(EncodeDetachedSignatureDocument(decoded.Value()) == encoded);
        Security::ArtifactVerifier unconfigured{nullptr, nullptr};
        CHECK(unconfigured.Verify(Bytes("actual artifact"), decoded.Value()).HasError());
        auto changed = encoded;
        changed.insert(1, "\"schemaVersion\":1,");
        CHECK(DecodeDetachedSignatureDocument(Bytes(changed)).HasError());
    }

    TEST_CASE("Host document decoder preserves explicit trust policy while malformed input publishes no policy",
              "[packages][host][security]") {
        const auto document = TrustDocument();
        const auto decoded = DecodePublisherTrustDocument(Bytes(document), Packages::PackagePublisherVerificationPolicy{});
        REQUIRE(decoded.HasValue());
        CHECK_FALSE(decoded.Value().allowUnsigned);
        REQUIRE(decoded.Value().publishers.size() == 1);
        const auto &publisher = decoded.Value().publishers.front();
        CHECK(publisher.publisher.Value() == "com.horo.test");
        CHECK(publisher.expiresAtUnixMilliseconds == 12345);
        CHECK(publisher.publisherRevoked);
        CHECK(publisher.publicKey.size() == 65);
        const std::array bad{std::string{}, std::string{"{}"},
                             std::string{R"({"schemaVersion":1,"schemaVersion":1,"allowUnsigned":false,"publishers":[]})"},
                             std::string{R"({"schemaVersion":1,"allowUnsigned":false,"publishers":[[[[[]]]]]})"},
                             std::string(MaximumHostSignatureDocumentBytes + 1, ' ')};
        for (const auto &candidate : bad)
            CHECK(DecodePublisherTrustDocument(Bytes(candidate), Packages::PackagePublisherVerificationPolicy{}).HasError());
        CHECK(decoded.Value().publishers.front().publisherRevoked);
        CHECK(decoded.Value().publishers.front().expiresAtUnixMilliseconds == 12345);
    }

    TEST_CASE("Host trust decoding appends transactionally within explicit caller policy", "[packages][host][security]") {
        const auto seed = SeedPolicy();
        const auto decoded = DecodePublisherTrustDocument(Bytes(TrustDocument()), seed);
        REQUIRE(decoded.HasValue());
        const auto &policy = decoded.Value();
        REQUIRE(policy.publishers.size() == 2);
        CHECK(policy.publishers.front().publisher == seed.publishers.front().publisher);
        CHECK(policy.publishers.front().keyId == "existing-key");
        CHECK(policy.publishers.front().publicKey == seed.publishers.front().publicKey);
        CHECK(policy.publishers.front().expiresAtUnixMilliseconds == 99999);
        CHECK_FALSE(policy.publishers.front().publisherRevoked);
        CHECK(policy.publishers.back().publisher.Value() == "com.horo.test");
        CHECK(policy.auditCapacity == 7);
        CHECK(policy.limits.maximumArtifactBytes == 1234);
        CHECK(policy.limits.maximumPublisherRecords == 2);
        CHECK(policy.limits.maximumPublisherIdBytes == 40);
        CHECK(policy.limits.maximumKeyIdBytes == 48);
        CHECK(policy.limits.maximumPublicKeyBytes == 65);
        CHECK(policy.limits.maximumSignatureBytes == 64);
        CHECK_FALSE(policy.allowUnsigned);
        CHECK(seed.allowUnsigned);
        REQUIRE(seed.publishers.size() == 1);
        CHECK(seed.publishers.front().keyId == "existing-key");
        STATIC_REQUIRE(std::is_nothrow_move_assignable_v<Packages::PackagePublisherVerificationPolicy>);
    }

    TEST_CASE("Host trust rejection leaves seeded policy unchanged including stricter combined bounds", "[packages][host][security]") {
        auto seed = SeedPolicy();
        SECTION("existing plus new records exceed the caller bound") {
            seed.limits.maximumPublisherRecords = 1;
            CHECK(DecodePublisherTrustDocument(Bytes(TrustDocument()), seed).HasError());
        }
        SECTION("over-limit seed cannot bypass the bound with an empty document") {
            seed.limits.maximumPublisherRecords = 0;
            CHECK(DecodePublisherTrustDocument(Bytes(R"({"schemaVersion":1,"allowUnsigned":false,"publishers":[]})"), seed).HasError());
        }
        SECTION("later invalid entry cannot append an earlier valid entry") {
            seed.limits.maximumPublisherRecords = 3;
            auto document = TrustDocument();
            document.insert(document.size() - 2, ",{}");
            CHECK(DecodePublisherTrustDocument(Bytes(document), seed).HasError());
        }
        SECTION("malformed document cannot replace existing approval") {
            CHECK(DecodePublisherTrustDocument(Bytes("{"), seed).HasError());
        }
        CHECK(seed.allowUnsigned);
        CHECK(seed.auditCapacity == 7);
        CHECK(seed.limits.maximumArtifactBytes == 1234);
        REQUIRE(seed.publishers.size() == 1);
        CHECK(seed.publishers.front().publisher.Value() == "com.horo.existing");
        CHECK(seed.publishers.front().keyId == "existing-key");
        CHECK(seed.publishers.front().expiresAtUnixMilliseconds == 99999);
        CHECK_FALSE(seed.publishers.front().publisherRevoked);
    }
}  // namespace Horo::Application::Internal::Tests
