#include "VerifiedShaderHostCatalog.h"

#include <catch2/catch_test_macros.hpp>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <string>

namespace Horo::Application::Internal {
    namespace {
        using namespace Security;

        int TestRandom(void *state, unsigned char *output, const std::size_t length) {
            auto &value = *static_cast<std::uint32_t *>(state);
            for (std::size_t index = 0; index < length; ++index) {
                value = value * 1664525U + 1013904223U;
                output[index] = static_cast<unsigned char>(value >> 24U);
            }
            return 0;
        }

        struct SignedFixture {
            std::vector<std::byte> artifact;
            TrustedSigningKey trustedKey;
            DetachedSignatureEnvelope envelope;
        };

        /** @brief Test-only signing state; assertion unwinding releases every native crypto allocation. */
        struct SigningState final {
            mbedtls_ecp_group group;
            mbedtls_ecp_point publicPoint;
            mbedtls_mpi privateKey;
            mbedtls_mpi r;
            mbedtls_mpi s;

            SigningState() {
                mbedtls_ecp_group_init(&group);
                mbedtls_ecp_point_init(&publicPoint);
                mbedtls_mpi_init(&privateKey);
                mbedtls_mpi_init(&r);
                mbedtls_mpi_init(&s);
            }

            ~SigningState() {
                mbedtls_mpi_free(&s);
                mbedtls_mpi_free(&r);
                mbedtls_mpi_free(&privateKey);
                mbedtls_ecp_point_free(&publicPoint);
                mbedtls_ecp_group_free(&group);
            }

            SigningState(const SigningState &) = delete;
            SigningState &operator=(const SigningState &) = delete;
        };

        /** @brief Signs arbitrary test inputs with an explicitly test-only root, never a production catalog approval. */
        [[nodiscard]] SignedFixture MakeSignedFixture(const std::string_view Artifact) {
            SignedFixture fixture;
            fixture.artifact.assign(reinterpret_cast<const std::byte *>(Artifact.data()),
                                    reinterpret_cast<const std::byte *>(Artifact.data() + Artifact.size()));
            fixture.envelope.publisherId = "com.horo.tests";
            fixture.envelope.keyId = "release-1";
            fixture.envelope.artifactDigest = ComputeSha256(fixture.artifact);
            const Sha256Digest payloadDigest = ComputeSignaturePayloadDigest(fixture.envelope);

            SigningState state;
            auto &group = state.group;
            auto &publicPoint = state.publicPoint;
            auto &privateKey = state.privateKey;
            auto &r = state.r;
            auto &s = state.s;
            REQUIRE(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0);
            REQUIRE(mbedtls_mpi_lset(&privateKey, 1) == 0);
            std::uint32_t randomState = 0x48305230U;
            REQUIRE(mbedtls_ecp_mul(&group, &publicPoint, &privateKey, &group.G, TestRandom, &randomState) == 0);
            REQUIRE(mbedtls_ecdsa_sign(&group, &r, &s, &privateKey, payloadDigest.bytes.data(), payloadDigest.bytes.size(), TestRandom,
                                       &randomState) == 0);

            fixture.trustedKey.publisherId = fixture.envelope.publisherId;
            fixture.trustedKey.keyId = fixture.envelope.keyId;
            fixture.trustedKey.publicKey.resize(65U);
            std::size_t publicKeySize{};
            REQUIRE(mbedtls_ecp_point_write_binary(&group, &publicPoint, MBEDTLS_ECP_PF_UNCOMPRESSED, &publicKeySize,
                                                   reinterpret_cast<unsigned char *>(fixture.trustedKey.publicKey.data()),
                                                   fixture.trustedKey.publicKey.size()) == 0);
            REQUIRE(publicKeySize == fixture.trustedKey.publicKey.size());
            fixture.envelope.signature.resize(64U);
            REQUIRE(mbedtls_mpi_write_binary(&r, reinterpret_cast<unsigned char *>(fixture.envelope.signature.data()), 32U) == 0);
            REQUIRE(mbedtls_mpi_write_binary(&s, reinterpret_cast<unsigned char *>(fixture.envelope.signature.data()) + 32U, 32U) == 0);
            return fixture;
        }

        std::string Catalog() {
            REQUIRE(ParseSha256("sha256:" + std::string(64, '1')).HasValue());
            REQUIRE(ParseSha256("sha256:" + std::string(64, '2')).HasValue());
            return R"({"version":1,"host":"macos-arm64","tools":[{"role":"dxc","release":"test-only","archive_sha256":"sha256:)" +
                   std::string(64, '1') + R"(","executable_sha256":"sha256:)" + std::string(64, '2') + R"("}]})";
        }
    }  // namespace

    TEST_CASE("Signed shader host catalog authenticates exact immutable approval tuples", "[application][shader][security]") {
        const auto fixture = MakeSignedFixture(Catalog());
        auto roots = std::make_shared<Security::TrustedRootStore>();
        REQUIRE(roots->Add(fixture.trustedKey).HasValue());
        const Security::ArtifactVerifier verifier{Security::CreateMbedTlsSignatureProvider(), roots};
        REQUIRE(verifier.Verify(fixture.artifact, fixture.envelope).HasValue());
        const auto catalog = VerifiedShaderHostCatalog::Create(fixture.artifact, fixture.envelope, verifier, "macos-arm64");
        INFO("catalog domain=" << (catalog.HasError() ? catalog.ErrorValue().domain.Value() : std::string_view{"none"})
                               << " code=" << (catalog.HasError() ? catalog.ErrorValue().code.Value() : std::string_view{"none"})
                               << " message=" << (catalog.HasError() ? catalog.ErrorValue().message : std::string{"catalog admitted"}));
        REQUIRE(catalog.HasValue());
        REQUIRE(catalog.Value()->Tools().size() == 1);
        const auto approval = catalog.Value()->Tools()[0];
        REQUIRE(catalog.Value()->Approves("macos-arm64", approval.identity, approval.executableDigest));
        CHECK_FALSE(catalog.Value()->Approves("macos-x86_64", approval.identity, approval.executableDigest));
        auto changed = approval.identity;
        changed.release += "-foreign";
        CHECK_FALSE(catalog.Value()->Approves("macos-arm64", changed, approval.executableDigest));
        changed = approval.identity;
        changed.tool = Render::ShaderCompilerTool::AppleMetal;
        CHECK_FALSE(catalog.Value()->Approves("macos-arm64", changed, approval.executableDigest));
        changed = approval.identity;
        changed.buildDigest.bytes[0] ^= 1;
        CHECK_FALSE(catalog.Value()->Approves("macos-arm64", changed, approval.executableDigest));
        auto digest = approval.executableDigest;
        digest.bytes[0] ^= 1;
        CHECK_FALSE(catalog.Value()->Approves("macos-arm64", approval.identity, digest));
    }

    TEST_CASE("Signed shader host catalog never derives trust from snapshot contents", "[application][shader][security]") {
        auto fixture = MakeSignedFixture(Catalog());
        auto roots = std::make_shared<Security::TrustedRootStore>();
        SECTION("Independent unknown root") {}
        SECTION("Tampered authenticated bytes") {
            REQUIRE(roots->Add(fixture.trustedKey).HasValue());
            fixture.artifact.back() ^= std::byte{1};
        }
        SECTION("Tampered detached signature") {
            REQUIRE(roots->Add(fixture.trustedKey).HasValue());
            fixture.envelope.signature.back() ^= std::byte{1};
        }
        const Security::ArtifactVerifier verifier{Security::CreateMbedTlsSignatureProvider(), roots};
        const auto authenticated = verifier.Verify(fixture.artifact, fixture.envelope);
        REQUIRE(authenticated.HasError());
        const auto catalog = VerifiedShaderHostCatalog::Create(fixture.artifact, fixture.envelope, verifier, "macos-arm64");
        REQUIRE(catalog.HasError());
        CHECK(catalog.ErrorValue().domain.Value() == authenticated.ErrorValue().domain.Value());
        CHECK(catalog.ErrorValue().code.Value() == authenticated.ErrorValue().code.Value());
    }

    TEST_CASE("Authenticated shader catalog rejects ambiguous and unsupported signed schema", "[application][shader][security]") {
        auto document = Catalog();
        SECTION("Duplicate root field") {
            document.insert(1, "\"version\":1,");
        }
        SECTION("Duplicate tool field") {
            document.insert(document.find("\"role\""), "\"role\":\"dxc\",");
        }
        SECTION("Unknown root field") {
            document.insert(1, "\"allow_all\":true,");
        }
        SECTION("Unknown tool role") {
            document.replace(document.find("dxc"), 3, "shell");
        }
        SECTION("Version integer cannot truncate into schema one") {
            document.replace(document.find("1,"), 1, "4294967297");
        }
        SECTION("Trailing bytes") {
            document += "{}";
        }
        SECTION("Wrong actual host") {
            document.replace(document.find("macos-arm64"), 11, "linux-x64");
        }
        SECTION("Bare hex is not canonical Foundation digest text") {
            document.erase(document.find("sha256:"), 7);
        }
        SECTION("Zero digest") {
            document.replace(document.find(std::string(64, '1')), 64, std::string(64, '0'));
        }
        SECTION("Excess nesting") {
            document = R"({"version":1,"host":"macos-arm64","tools":[[[[{}]]]]})";
        }
        SECTION("Oversized snapshot") {
            document.resize(65537, ' ');
        }
        const auto fixture = MakeSignedFixture(document);
        auto roots = std::make_shared<Security::TrustedRootStore>();
        REQUIRE(roots->Add(fixture.trustedKey).HasValue());
        const Security::ArtifactVerifier verifier{Security::CreateMbedTlsSignatureProvider(), roots};
        REQUIRE(verifier.Verify(fixture.artifact, fixture.envelope).HasValue());
        REQUIRE(VerifiedShaderHostCatalog::Create(fixture.artifact, fixture.envelope, verifier, "macos-arm64").HasError());
    }
}  // namespace Horo::Application::Internal
