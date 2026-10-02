#include "Horo/Platform/SecureRandom.h"
#include "Horo/Security/ArtifactSignature.h"
#include "Horo/Security/CredentialStore.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <type_traits>

namespace Horo::Security::Tests {
    namespace {
        [[nodiscard]] SecureBytes Secret(const std::string_view value) {
            return SecureBytes{{reinterpret_cast<const std::byte *>(value.data()), value.size()}};
        }

        [[nodiscard]] std::string SecretText(const SecureBytes &value) {
            const auto bytes = value.View();
            return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        }

        class DeterministicRandom final : public SecureRandomSource {
        public:
            bool fail{};
            std::uint8_t next{1U};

            [[nodiscard]] Result<void> Fill(const std::span<std::byte> destination) override {
                if (fail) {
                    SecureZero(destination);
                    return Result<void>::Failure(MakeError(SecurityErrors::EntropyUnavailable));
                }
                for (std::byte &byte : destination)
                    byte = static_cast<std::byte>(next++);
                return Result<void>::Success();
            }
        };

        class MemoryCredentialBackend final : public CredentialBackend {
        public:
            bool available{true};
            std::size_t removals{};

            [[nodiscard]] bool Available() const noexcept override {
                return available;
            }

            [[nodiscard]] Result<void> Put(const CredentialReference &reference, SecureBytes secret) override {
                if (!available)
                    return Result<void>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
                values[std::string{reference.Value()}] = std::vector<std::byte>{secret.View().begin(), secret.View().end()};
                return Result<void>::Success();
            }

            [[nodiscard]] Result<SecureBytes> Resolve(const CredentialReference &reference) override {
                if (!available)
                    return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
                const auto value = values.find(std::string{reference.Value()});
                if (value == values.end())
                    return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialNotFound));
                return Result<SecureBytes>::Success(SecureBytes{value->second});
            }

            [[nodiscard]] Result<void> Remove(const CredentialReference &reference) noexcept override {
                ++removals;
                values.erase(std::string{reference.Value()});
                return Result<void>::Success();
            }

            std::map<std::string, std::vector<std::byte>, std::less<>> values;
        };

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

        [[nodiscard]] SignedFixture MakeSignedFixture() {
            SignedFixture fixture;
            constexpr std::string_view Artifact = "signed artifact";
            fixture.artifact.assign(reinterpret_cast<const std::byte *>(Artifact.data()),
                                    reinterpret_cast<const std::byte *>(Artifact.data() + Artifact.size()));
            fixture.envelope.publisherId = "com.horo.tests";
            fixture.envelope.keyId = "release-1";
            fixture.envelope.artifactDigest = ComputeSha256(fixture.artifact);
            const Sha256Digest payloadDigest = ComputeSignaturePayloadDigest(fixture.envelope);

            mbedtls_ecp_group group;
            mbedtls_ecp_point publicPoint;
            mbedtls_mpi privateKey;
            mbedtls_mpi r;
            mbedtls_mpi s;
            mbedtls_ecp_group_init(&group);
            mbedtls_ecp_point_init(&publicPoint);
            mbedtls_mpi_init(&privateKey);
            mbedtls_mpi_init(&r);
            mbedtls_mpi_init(&s);
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
            mbedtls_mpi_free(&s);
            mbedtls_mpi_free(&r);
            mbedtls_mpi_free(&privateKey);
            mbedtls_ecp_point_free(&publicPoint);
            mbedtls_ecp_group_free(&group);
            return fixture;
        }
    }  // namespace

    static_assert(!std::is_copy_constructible_v<SecureBytes>);
    static_assert(!std::is_copy_assignable_v<SecureBytes>);
    static_assert(std::is_nothrow_move_constructible_v<SecureBytes>);

    TEST_CASE("Secure memory and injected entropy preserve primitive boundaries", "[Security][Memory]") {
        std::array<std::byte, 8> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        SecureZero(bytes);
        CHECK(std::ranges::all_of(bytes, [](const std::byte byte) {
            return byte == std::byte{0};
        }));

        auto random = std::make_shared<DeterministicRandom>();
        std::array<std::byte, 4> output{};
        REQUIRE(random->Fill(output).HasValue());
        CHECK(output == std::array<std::byte, 4>{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}});
        random->fail = true;
        REQUIRE(random->Fill(output).HasError());
        CHECK(std::ranges::all_of(output, [](const std::byte byte) {
            return byte == std::byte{0};
        }));

        const auto native = Platform::CreateNativeSecureRandomSource();
        REQUIRE(native != nullptr);
        CHECK(native->Fill(output).HasValue());
    }

    TEST_CASE("Credential vault fails closed and supports recovery and full lifecycle", "[Security][Credentials]") {
        auto random = std::make_shared<DeterministicRandom>();
        CredentialVault vault{nullptr, random};
        CHECK(vault.Put(Secret("unavailable")).ErrorValue().code.Value() == "credential_backend_unavailable");

        auto backend = std::make_shared<MemoryCredentialBackend>();
        vault.SetBackend(backend);
        random->fail = true;
        CHECK(vault.Put(Secret("entropy-failure")).ErrorValue().code.Value() == "entropy_unavailable");
        random->fail = false;
        CHECK(vault.Put(SecureBytes{}).ErrorValue().code.Value() == "invalid_input");
        auto stored = vault.Put(Secret("alpha"), 50U);
        REQUIRE(stored.HasValue());
        CHECK(stored.Value().Value().starts_with("cred:v1:"));
        CHECK(stored.Value().Value().find("alpha") == std::string_view::npos);
        CHECK(SecretText(vault.Resolve(stored.Value(), 49U).Value()) == "alpha");

        REQUIRE(vault.Rotate(stored.Value(), Secret("beta"), 100U).HasValue());
        CHECK(SecretText(vault.Resolve(stored.Value(), 99U).Value()) == "beta");
        backend->available = false;
        CHECK(vault.Resolve(stored.Value()).ErrorValue().code.Value() == "credential_backend_unavailable");
        backend->available = true;
        CHECK(SecretText(vault.Resolve(stored.Value()).Value()) == "beta");
        CHECK(vault.Resolve(stored.Value(), 100U).ErrorValue().code.Value() == "credential_expired");
        CHECK(vault.Resolve(stored.Value()).ErrorValue().code.Value() == "credential_revoked");

        auto second = vault.Put(Secret("gamma"));
        REQUIRE(second.HasValue());
        REQUIRE(vault.Revoke(second.Value()).HasValue());
        CHECK(vault.Resolve(second.Value()).ErrorValue().code.Value() == "credential_revoked");
        CHECK(backend->values.empty());
    }

    TEST_CASE("Credential vault tears down provider material without exposing secrets", "[Security][Credentials]") {
        auto backend = std::make_shared<MemoryCredentialBackend>();
        {
            CredentialVault vault{backend, std::make_shared<DeterministicRandom>()};
            REQUIRE(vault.Put(Secret("never-log-this")).HasValue());
            REQUIRE(backend->values.size() == 1U);
        }
        CHECK(backend->values.empty());
        CHECK(backend->removals == 1U);
    }

    TEST_CASE("Artifact verifier binds trust and signature to exact bytes", "[Security][Signature]") {
        SignedFixture fixture = MakeSignedFixture();
        auto roots = std::make_shared<TrustedRootStore>();
        REQUIRE(roots->Add(fixture.trustedKey).HasValue());
        ArtifactVerifier verifier{CreateMbedTlsSignatureProvider(), roots};
        auto verified = verifier.Verify(fixture.artifact, fixture.envelope);
        REQUIRE(verified.HasValue());
        CHECK(verified.Value().ArtifactDigest() == fixture.envelope.artifactDigest);
        CHECK(verified.Value().PublisherId() == fixture.envelope.publisherId);

        fixture.artifact.front() ^= std::byte{1};
        CHECK(verifier.Verify(fixture.artifact, fixture.envelope).ErrorValue().code.Value() == "integrity_mismatch");
        fixture.artifact.front() ^= std::byte{1};
        fixture.envelope.signature.front() ^= std::byte{1};
        CHECK(verifier.Verify(fixture.artifact, fixture.envelope).ErrorValue().code.Value() == "invalid_signature");
        REQUIRE(roots->Revoke("com.horo.tests", "release-1").HasValue());
        CHECK(verifier.Verify(fixture.artifact, fixture.envelope).ErrorValue().code.Value() == "unknown_signing_key");
    }

    TEST_CASE("Artifact verifier rejects unknown algorithms keys and missing providers", "[Security][Signature]") {
        SignedFixture fixture = MakeSignedFixture();
        auto roots = std::make_shared<TrustedRootStore>();
        ArtifactVerifier unknownKey{CreateMbedTlsSignatureProvider(), roots};
        CHECK(unknownKey.Verify(fixture.artifact, fixture.envelope).ErrorValue().code.Value() == "unknown_signing_key");
        REQUIRE(roots->Add(fixture.trustedKey).HasValue());
        ArtifactVerifier missingProvider{nullptr, roots};
        CHECK(missingProvider.Verify(fixture.artifact, fixture.envelope).ErrorValue().code.Value() == "signature_provider_unavailable");
        fixture.envelope.algorithm = static_cast<SignatureAlgorithm>(255U);
        CHECK(unknownKey.Verify(fixture.artifact, fixture.envelope).ErrorValue().code.Value() == "unsupported_algorithm");
    }

    TEST_CASE("Detached file gate verifies current bytes and fails closed without composition", "[Security][Signature][File]") {
        SignedFixture fixture = MakeSignedFixture();
        auto roots = std::make_shared<TrustedRootStore>();
        REQUIRE(roots->Add(fixture.trustedKey).HasValue());
        auto verifier = std::make_shared<ArtifactVerifier>(CreateMbedTlsSignatureProvider(), roots);
        const std::filesystem::path artifactPath =
            std::filesystem::temp_directory_path() /
            ("horo-security-artifact-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        struct Cleanup final {
            std::filesystem::path path;

            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } cleanup{artifactPath};

        {
            std::ofstream output{artifactPath, std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char *>(fixture.artifact.data()), static_cast<std::streamsize>(fixture.artifact.size()));
        }

        DetachedFileArtifactGate gate{verifier, [envelope = fixture.envelope](const std::filesystem::path &) {
            return Result<DetachedSignatureEnvelope>::Success(envelope);
        }};
        REQUIRE(gate.Verify(artifactPath).HasValue());
        REQUIRE(verifier->VerifyFile(artifactPath, fixture.artifact.size(), fixture.envelope).HasValue());
        CHECK(verifier->VerifyFile(artifactPath, fixture.artifact.size() - 1U, fixture.envelope).HasError());
        {
            std::ofstream changed{artifactPath, std::ios::binary | std::ios::app};
            changed.put('\0');
        }
        CHECK(gate.Verify(artifactPath).ErrorValue().code.Value() == "integrity_mismatch");
        CHECK(verifier->VerifyFile(artifactPath, fixture.artifact.size(), fixture.envelope).HasError());

        DetachedFileArtifactGate missingComposition{nullptr, {}};
        CHECK(missingComposition.Verify(artifactPath).ErrorValue().code.Value() == "missing_evidence");
    }
}  // namespace Horo::Security::Tests
