#include "HostSignatureDocument.h"
#include "OpenXRInstalledHostCatalog.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <mbedtls/ecdsa.h>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace Horo::Application::Internal::Tests {
    namespace {
        using Json = nlohmann::json;

        /** @brief Test-owned key material, unrelated to product release roots; every partial failure has RAII cleanup. */
        struct SigningKey final {
            struct Group final {
                mbedtls_ecp_group value;

                Group() {
                    mbedtls_ecp_group_init(&value);
                }

                ~Group() {
                    mbedtls_ecp_group_free(&value);
                }
            } group;

            struct Point final {
                mbedtls_ecp_point value;

                Point() {
                    mbedtls_ecp_point_init(&value);
                }

                ~Point() {
                    mbedtls_ecp_point_free(&value);
                }
            } point;

            struct Integer final {
                mbedtls_mpi value;

                Integer() {
                    mbedtls_mpi_init(&value);
                }

                ~Integer() {
                    mbedtls_mpi_free(&value);
                }
            } secret;

            std::uint32_t random{0x48305230U};
            Security::TrustedSigningKey root;

            static int Random(void *state, unsigned char *output, const std::size_t size) {
                auto &value = *static_cast<std::uint32_t *>(state);
                for (std::size_t index = 0; index < size; ++index) {
                    value = value * 1664525U + 1013904223U;
                    output[index] = static_cast<unsigned char>(value >> 24U);
                }
                return 0;
            }

            SigningKey() {
                REQUIRE(mbedtls_ecp_group_load(&group.value, MBEDTLS_ECP_DP_SECP256R1) == 0);
                REQUIRE(mbedtls_mpi_lset(&secret.value, 1) == 0);
                REQUIRE(mbedtls_ecp_mul(&group.value, &point.value, &secret.value, &group.value.G, Random, &random) == 0);
                root.publisherId = "com.horo.tests";
                root.keyId = "host-test-key";
                root.publicKey.resize(65);
                std::size_t size{};
                REQUIRE(mbedtls_ecp_point_write_binary(&group.value, &point.value, MBEDTLS_ECP_PF_UNCOMPRESSED, &size,
                                                       reinterpret_cast<unsigned char *>(root.publicKey.data()),
                                                       root.publicKey.size()) == 0);
                REQUIRE(size == root.publicKey.size());
            }

            Security::DetachedSignatureEnvelope Sign(const std::span<const std::byte> bytes) {
                Security::DetachedSignatureEnvelope envelope{.publisherId = root.publisherId,
                                                             .keyId = root.keyId,
                                                             .artifactDigest = ComputeSha256(bytes),
                                                             .signature = std::vector<std::byte>(64)};
                const auto digest = Security::ComputeSignaturePayloadDigest(envelope);
                Integer r, s;
                REQUIRE(mbedtls_ecdsa_sign(&group.value, &r.value, &s.value, &secret.value, digest.bytes.data(), digest.bytes.size(),
                                           Random, &random) == 0);
                REQUIRE(mbedtls_mpi_write_binary(&r.value, reinterpret_cast<unsigned char *>(envelope.signature.data()), 32) == 0);
                REQUIRE(mbedtls_mpi_write_binary(&s.value, reinterpret_cast<unsigned char *>(envelope.signature.data()) + 32, 32) == 0);
                return envelope;
            }
        };

        std::span<const std::byte> Bytes(const std::string_view text) {
            return std::as_bytes(std::span{text.data(), text.size()});
        }

        /** @brief Native execution is not exercised: this is explicit signed deployment-selection data for the Linux ABI. */
        Json Catalog(const Security::DetachedSignatureEnvelope &embedded) {
            return {{"schemaVersion", 1},
                    {"backend", 7},
                    {"package", "com.horo.openxr"},
                    {"component", "runtime/xr/component.json"},
                    {"deployment", "runtime/xr/deployment.json"},
                    {"deploymentSignature", EncodeDetachedSignatureDocument(embedded)},
                    {"loaderSignature", EncodeDetachedSignatureDocument(embedded)},
                    {"bindings", "runtime/xr/bindings.json"},
                    {"deploymentContract", "horo-test-linux-abi-v1"},
                    {"platformBridge", 1},
                    {"rendererBridge", 1},
                    {"expiresAt", 1000},
                    {"profile", 0},
                    {"limits", {{"views", 2}, {"spaces", 16}, {"actions", 16}, {"devices", 3}}}};
        }

        struct CatalogFixture final {
            SigningKey key;
            std::shared_ptr<Security::TrustedRootStore> roots{std::make_shared<Security::TrustedRootStore>()};
            std::shared_ptr<const Security::ArtifactVerifier> verifier;
            Json document = Catalog(key.Sign(Bytes("separate artifact; catalog admission does not verify its contents")));
            Packages::PackagePlatform platform{"linux", "x64", "horo-sdk-2"};
            std::filesystem::path temporary{std::filesystem::temp_directory_path() / "horo-xr-catalog-test"};

            CatalogFixture() {
                REQUIRE(document.is_object());
                REQUIRE(roots->Add(key.root).HasValue());
                verifier = std::make_shared<Security::ArtifactVerifier>(Security::CreateMbedTlsSignatureProvider(), roots);
            }

            Result<OpenXRInstalledHostCatalog> Verify(const std::string &text) {
                return OpenXRInstalledHostCatalog::Verify(Bytes(text), key.Sign(Bytes(text)), verifier, platform, temporary, 100);
            }
        };
    }  // namespace

    TEST_CASE("Installed XR catalog uses real exact signature and independent host roots without installing or granting Ready",
              "[xr][host][security][install]") {
        CatalogFixture fixture;
        const auto text = fixture.document.dump();
        const auto signature = fixture.key.Sign(Bytes(text));
        auto admitted =
            OpenXRInstalledHostCatalog::Verify(Bytes(text), signature, fixture.verifier, fixture.platform, fixture.temporary, 100);
        INFO((admitted.HasError() ? admitted.ErrorValue().message : "catalog admitted"));
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().Evidence().ArtifactDigest() == ComputeSha256(Bytes(text)));
        CHECK(admitted.Value().Policy().component.publisher.Value() == fixture.key.root.publisherId);
        CHECK(admitted.Value().Bindings().Value() == "runtime/xr/bindings.json");
        CHECK(admitted.Value().Limits().maximumSpaces == 16);
        CHECK(admitted.Value().ExpiresAt() == 1000);
        STATIC_REQUIRE_FALSE(std::is_default_constructible_v<OpenXRInstalledHostCatalog>);
        auto changed = text;
        changed.front() ^= 1;
        CHECK(OpenXRInstalledHostCatalog::Verify(Bytes(changed), signature, fixture.verifier, fixture.platform, fixture.temporary, 100)
                  .HasError());
        REQUIRE(fixture.roots->Revoke(fixture.key.root.publisherId, fixture.key.root.keyId).HasValue());
        CHECK(OpenXRInstalledHostCatalog::Verify(Bytes(text), signature, fixture.verifier, fixture.platform, fixture.temporary, 100)
                  .HasError());
    }

    TEST_CASE("Signed XR catalog rejects expired foreign-envelope unbounded and path-escape declarations",
              "[xr][host][security][install]") {
        CatalogFixture fixture;
        SECTION("signed expired selection") {
            fixture.document["expiresAt"] = 100;
        }
        SECTION("signed excess space budget") {
            fixture.document["limits"]["spaces"] = XR::XRHardLimits::MaximumSpaces + 1;
        }
        SECTION("signed escaping binding path") {
            fixture.document["bindings"] = "runtime/../elsewhere.json";
        }
        SECTION("signed unsupported bridge") {
            fixture.document["rendererBridge"] = 2;
        }
        SECTION("signed unknown authority field") {
            fixture.document["allowUnsigned"] = true;
        }
        SECTION("signed foreign loader signer") {
            auto envelope = fixture.key.Sign(Bytes("loader"));
            envelope.keyId = "unapproved-other-key";
            fixture.document["loaderSignature"] = EncodeDetachedSignatureDocument(envelope);
        }
        SECTION("signed duplicate field") {
            auto text = fixture.document.dump();
            text.insert(1, "\"backend\":7,");
            CHECK(fixture.Verify(text).HasError());
            return;
        }
        CHECK(fixture.Verify(fixture.document.dump()).HasError());
    }
}  // namespace Horo::Application::Internal::Tests
