#include "HostSignatureDocument.h"
#include "OpenXRInstalledHostCatalog.h"
#include "support/OpenXRProductSigningTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <mbedtls/ecdsa.h>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace Horo::Application::Internal::Tests {
    namespace {
        using Json = nlohmann::json;

        using Horo::Tests::OpenXRProduct::SigningKey;

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

    TEST_CASE("Signed XR catalog parses exact UTF8 path bytes and rejects signed malformed text", "[xr][host][security][install]") {
        CatalogFixture fixture;
        constexpr std::string_view path = "runtime/xr/bindings-ü.json";
        fixture.document["bindings"] = path;
        auto bytes = fixture.document.dump();
        bool malformed{};
        SECTION("valid non-ASCII path") {}
        SECTION("invalid UTF8 byte inside an otherwise signed path") {
            const auto offset = bytes.find("ü");
            REQUIRE(offset != std::string::npos);
            bytes[offset] = static_cast<char>(0xff);
            malformed = true;
        }
        SECTION("truncated signed JSON") {
            bytes.pop_back();
            malformed = true;
        }
        // Sign the exact supplied bytes, so malformed-input rejection cannot be mistaken for signature rejection.
        const auto parsed = fixture.Verify(bytes);
        if (malformed) {
            CHECK(parsed.HasError());
        } else {
            REQUIRE(parsed.HasValue());
            CHECK(parsed.Value().Bindings().Value() == path);
            CHECK(parsed.Value().Evidence().ArtifactDigest() == ComputeSha256(Bytes(bytes)));
        }
    }
}  // namespace Horo::Application::Internal::Tests
