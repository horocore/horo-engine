#include "Horo/Security/ArtifactSignature.h"
#include "Horo/Security/SecureMemory.h"
#include "PackageCommand.h"
#include "SignatureDocument.h"

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

        [[nodiscard]] Outcome SignPayload(const std::filesystem::path &keyPath, Security::DetachedSignatureEnvelope &envelope) {
            std::error_code error;
            const auto permissions = std::filesystem::status(keyPath, error).permissions();
            if (error)
                return Failure("package.key_invalid", "Private key file is unavailable.");
#ifndef _WIN32
            const auto shared = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
            if ((permissions & shared) != std::filesystem::perms::none)
                return Failure("package.key_permissions", "Private key file must not be accessible to group or others.");
#endif
            std::vector<std::byte> privateBytes;
            privateBytes.reserve(16U * 1024U + 1U);  // Appending PEM terminator must not reallocate and strand secret bytes.
            if (const auto read = ReadBounded(keyPath, 16U * 1024U, privateBytes); !read.success)
                return read;
            const auto erase = [&privateBytes] {
                Security::SecureZero(std::span{privateBytes});
            };
            if (privateBytes.empty() || privateBytes.back() != std::byte{0})
                privateBytes.push_back(std::byte{0});
            SigningState state;
            constexpr std::string_view Personalization = "horo-package-sign-v1";
            const int seeded =
                mbedtls_ctr_drbg_seed(&state.random, mbedtls_entropy_func, &state.entropy,
                                      reinterpret_cast<const unsigned char *>(Personalization.data()), Personalization.size());
            const int parsed = seeded == 0 ? mbedtls_pk_parse_key(&state.key, reinterpret_cast<const unsigned char *>(privateBytes.data()),
                                                                  privateBytes.size(), nullptr, 0, mbedtls_ctr_drbg_random, &state.random)
                                           : -1;
            erase();
            if (parsed != 0)
                return Failure("package.key_invalid", "Private key could not be parsed as unencrypted P-256 PEM.");
            auto *ec = mbedtls_pk_ec(state.key);
            if (!ec || ec->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1)
                return Failure("package.key_invalid", "Private key must be P-256 EC.");
            const auto digest = Security::ComputeSignaturePayloadDigest(envelope);
            if (mbedtls_ecdsa_sign(&ec->MBEDTLS_PRIVATE(grp), &state.r, &state.s, &ec->MBEDTLS_PRIVATE(d), digest.bytes.data(),
                                   digest.bytes.size(), mbedtls_ctr_drbg_random, &state.random) != 0)
                return Failure("package.sign_failed", "Private-key signing failed.");
            envelope.signature.resize(64U);
            auto *out = reinterpret_cast<unsigned char *>(envelope.signature.data());
            if (mbedtls_mpi_write_binary(&state.r, out, 32U) != 0 || mbedtls_mpi_write_binary(&state.s, out + 32U, 32U) != 0)
                return Failure("package.sign_failed", "Signature encoding failed.");
            std::array<std::byte, 65> publicKey{};
            std::size_t keyBytes{};
            if (mbedtls_ecp_point_write_binary(&ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q), MBEDTLS_ECP_PF_UNCOMPRESSED, &keyBytes,
                                               reinterpret_cast<unsigned char *>(publicKey.data()), publicKey.size()) != 0 ||
                keyBytes != publicKey.size())
                return Failure("package.sign_failed", "Signing key public point is invalid.");
            const auto provider = Security::CreateMbedTlsSignatureProvider();
            if (const auto checked = provider->Verify(envelope.algorithm, publicKey, digest, envelope.signature); checked.HasError())
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
