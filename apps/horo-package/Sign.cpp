#include "Horo/Security/ArtifactSignature.h"
#include "Horo/Security/SecureMemory.h"
#include "PackageCommand.h"
#include "SignatureDocument.h"

#include <algorithm>
#include <array>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <system_error>

namespace Horo::PackageCommand {
    namespace {
        struct SigningState final {
            mbedtls_entropy_context entropy;
            mbedtls_ctr_drbg_context random;
            mbedtls_pk_context key;
            mbedtls_mpi r;
            mbedtls_mpi s;

            SigningState() {
                mbedtls_entropy_init(&entropy);
                mbedtls_ctr_drbg_init(&random);
                mbedtls_pk_init(&key);
                mbedtls_mpi_init(&r);
                mbedtls_mpi_init(&s);
            }

            SigningState(const SigningState &) = delete;
            SigningState &operator=(const SigningState &) = delete;

            ~SigningState() {
                mbedtls_mpi_free(&s);
                mbedtls_mpi_free(&r);
                mbedtls_pk_free(&key);
                mbedtls_ctr_drbg_free(&random);
                mbedtls_entropy_free(&entropy);
            }
        };

        struct PrivateKeyBuffer final {
            std::vector<unsigned char> bytes;

            PrivateKeyBuffer() = default;
            PrivateKeyBuffer(const PrivateKeyBuffer &) = delete;
            PrivateKeyBuffer &operator=(const PrivateKeyBuffer &) = delete;
            PrivateKeyBuffer(PrivateKeyBuffer &&) = delete;
            PrivateKeyBuffer &operator=(PrivateKeyBuffer &&) = delete;

            ~PrivateKeyBuffer() {
                Security::SecureZero(std::as_writable_bytes(std::span{bytes}));
            }
        };

        struct PublicPoint final {
            std::array<unsigned char, 65> bytes{};
            std::size_t size{};
        };

        [[nodiscard]] Outcome LoadSigningKey(const std::filesystem::path &keyPath, SigningState &state) {
            std::error_code error;
            const auto permissions = std::filesystem::status(keyPath, error).permissions();
            if (error)
                return Failure("package.key_invalid", "Private key file is unavailable.");
#ifndef _WIN32
            if (const auto shared = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
                (permissions & shared) != std::filesystem::perms::none)
                return Failure("package.key_permissions", "Private key file must not be accessible to group or others.");
#endif
            std::vector<std::byte> privateBytes;
            if (const auto read = ReadBounded(keyPath, 16U * 1024U, privateBytes); !read.success)
                return read;
            PrivateKeyBuffer privateKey;
            privateKey.bytes.reserve(16U * 1024U + 1U);
            privateKey.bytes.resize(privateBytes.size());
            std::ranges::transform(privateBytes, privateKey.bytes.begin(), [](const std::byte value) {
                return std::to_integer<unsigned char>(value);
            });
            Security::SecureZero(std::span{privateBytes});
            if (privateKey.bytes.empty() || privateKey.bytes.back() != 0)
                privateKey.bytes.push_back(0);
            constexpr std::string_view Personalization = "horo-package-sign-v1";
            std::array<unsigned char, Personalization.size()> personalization{};
            std::ranges::transform(Personalization, personalization.begin(), [](const char value) {
                return static_cast<unsigned char>(value);
            });
            const int seeded =
                mbedtls_ctr_drbg_seed(&state.random, mbedtls_entropy_func, &state.entropy, personalization.data(), personalization.size());
            if (const int parsed = seeded == 0 ? mbedtls_pk_parse_key(&state.key, privateKey.bytes.data(), privateKey.bytes.size(), nullptr,
                                                                      0, mbedtls_ctr_drbg_random, &state.random)
                                               : -1;
                parsed != 0)
                return Failure("package.key_invalid", "Private key could not be parsed as unencrypted P-256 PEM.");
            if (const auto *ec = mbedtls_pk_ec(state.key); !ec || ec->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1)
                return Failure("package.key_invalid", "Private key must be P-256 EC.");
            return Success();
        }

        [[nodiscard]] Outcome SignPayload(const std::filesystem::path &keyPath, Security::DetachedSignatureEnvelope &envelope) {
            SigningState state;
            if (const auto loaded = LoadSigningKey(keyPath, state); !loaded.success)
                return loaded;
            auto *ec = mbedtls_pk_ec(state.key);
            const auto digest = Security::ComputeSignaturePayloadDigest(envelope);
            if (mbedtls_ecdsa_sign(&ec->MBEDTLS_PRIVATE(grp), &state.r, &state.s, &ec->MBEDTLS_PRIVATE(d), digest.bytes.data(),
                                   digest.bytes.size(), mbedtls_ctr_drbg_random, &state.random) != 0)
                return Failure("package.sign_failed", "Private-key signing failed.");
            std::array<unsigned char, 64> encoded{};
            if (mbedtls_mpi_write_binary(&state.r, encoded.data(), 32U) != 0 ||
                mbedtls_mpi_write_binary(&state.s, encoded.data() + 32U, 32U) != 0)
                return Failure("package.sign_failed", "Signature encoding failed.");
            envelope.signature.resize(encoded.size());
            std::ranges::transform(encoded, envelope.signature.begin(), [](const unsigned char value) {
                return static_cast<std::byte>(value);
            });
            PublicPoint publicKey;
            if (mbedtls_ecp_point_write_binary(&ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q), MBEDTLS_ECP_PF_UNCOMPRESSED,
                                               &publicKey.size, publicKey.bytes.data(), publicKey.bytes.size()) != 0 ||
                publicKey.size != publicKey.bytes.size())
                return Failure("package.sign_failed", "Signing key public point is invalid.");
            const auto provider = Security::CreateMbedTlsSignatureProvider();
            if (const auto checked =
                    provider->Verify(envelope.algorithm, std::as_bytes(std::span{publicKey.bytes}), digest, envelope.signature);
                checked.HasError())
                return Failure("package.sign_failed", "Produced signature did not self-verify.");
            return Success();
        }
    }  // namespace

    Outcome Sign(const std::filesystem::path &archivePath, const std::filesystem::path &keyPath, const std::string_view publisher,
                 const std::string_view keyId, const std::filesystem::path &output) {
        auto publisherId = Packages::PackagePublisherId::Parse(publisher);
        if (publisherId.HasError() || keyId.empty() || keyId.size() > 256U)
            return Failure("package.sign_identity_invalid", "Publisher or key identity is invalid.");
        std::vector<std::byte> bytes;
        if (const auto read = ReadBounded(archivePath, MaximumArtifactBytes, bytes); !read.success)
            return read;
        std::optional<Packages::ValidatedPackageArchive> verified;
        if (const auto check = VerifyArchive(bytes, verified); !check.success)
            return check;
        Security::DetachedSignatureEnvelope envelope{.algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                                                     .publisherId = publisherId.Value().Value(),
                                                     .keyId = std::string{keyId},
                                                     .artifactDigest = verified->Digest()};
        if (const auto signedResult = SignPayload(keyPath, envelope); !signedResult.success)
            return signedResult;
        const std::string document = EncodeSignature(envelope);
        if (const auto written = WriteNew(output, std::as_bytes(std::span{document})); !written.success)
            return written;
        return Success("detached signature written; sha256=" + FormatSha256(envelope.artifactDigest));
    }
}  // namespace Horo::PackageCommand
