#pragma once

/**
 * @file ArtifactSignature.h
 * @brief Detached signature, trusted-root, and exact-artifact verification contracts.
 */

#include "Horo/Foundation/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Security {
    /** @brief Approved detached-signature algorithms understood by the security boundary. */
    enum class SignatureAlgorithm : std::uint8_t {
        EcdsaP256Sha256 /**< SEC1 P-256 ECDSA over a SHA-256 payload digest. */
    };

    /** @brief Detached signature envelope bound to one exact artifact digest and signing identity. */
    struct DetachedSignatureEnvelope {
        SignatureAlgorithm algorithm{SignatureAlgorithm::EcdsaP256Sha256}; /**< Declared verification algorithm. */
        std::string publisherId;                                           /**< Stable publisher identity. */
        std::string keyId;                                                 /**< Publisher-scoped signing key identity. */
        Sha256Digest artifactDigest;                                       /**< Digest of the exact signed artifact bytes. */
        std::vector<std::byte> signature;                                  /**< Provider-specific detached signature bytes. */
    };

    /** @brief Explicit trusted signing root containing a provider-specific public key. */
    struct TrustedSigningKey {
        SignatureAlgorithm algorithm{SignatureAlgorithm::EcdsaP256Sha256}; /**< Algorithm admitted for this root. */
        std::string publisherId;                                           /**< Stable trusted publisher identity. */
        std::string keyId;                                                 /**< Publisher-scoped trusted key identity. */
        std::vector<std::byte> publicKey;                                  /**< Provider-specific public key encoding. */
    };

    /** @brief Host-owned collection of built-in, organization, or user-approved signing roots. */
    class TrustedRootStore final {
    public:
        /** @brief Adds one unique bounded trusted root. @param key Root to add. @return Success or a typed invalid-input error. */
        [[nodiscard]] Result<void> Add(TrustedSigningKey key);
        /** @brief Removes one trusted root. @param publisherId Publisher identity. @param keyId Key identity. @return Success or
         * unknown-key error. */
        [[nodiscard]] Result<void> Revoke(std::string_view publisherId, std::string_view keyId);
        /**
         * @brief Finds an exact trusted-root snapshot.
         * @param publisherId Publisher identity.
         * @param keyId Key identity.
         * @return Independent key snapshot, or no value when the identity is unknown.
         * @note Access is synchronized so verification may run concurrently with host-driven key rotation.
         */
        [[nodiscard]] std::optional<TrustedSigningKey> Find(std::string_view publisherId, std::string_view keyId) const;

    private:
        mutable std::shared_mutex mutex_; /**< Protects the complete trusted-root collection from concurrent rotation and lookup. */
        std::vector<TrustedSigningKey> keys_;
    };

    /** @brief Private crypto-provider contract for detached signature verification. */
    class SignatureProvider {
    public:
        virtual ~SignatureProvider() = default;
        /** @brief Reports whether an algorithm is implemented. @param algorithm Algorithm to query. @return True only when supported. */
        [[nodiscard]] virtual bool Supports(SignatureAlgorithm algorithm) const noexcept = 0;
        /**
         * @brief Verifies a detached signature over an already hashed domain-separated payload.
         * @param algorithm Approved signature algorithm.
         * @param publicKey Provider-specific public key encoding.
         * @param payloadDigest Exact payload digest.
         * @param signature Provider-specific detached signature encoding.
         * @return Success or a typed provider/signature failure.
         */
        [[nodiscard]] virtual Result<void> Verify(SignatureAlgorithm algorithm, std::span<const std::byte> publicKey,
                                                  const Sha256Digest &payloadDigest, std::span<const std::byte> signature) const = 0;
    };

    /** @brief Immutable proof produced only by ArtifactVerifier after trust, integrity, and signature checks. */
    class VerifiedArtifactEvidence final {
    public:
        /** @brief Returns the verified exact-artifact digest. @return Immutable SHA-256 digest. */
        [[nodiscard]] const Sha256Digest &ArtifactDigest() const noexcept;
        /** @brief Returns the trusted publisher identity. @return Borrowed identity text. */
        [[nodiscard]] std::string_view PublisherId() const noexcept;
        /** @brief Returns the trusted signing key identity. @return Borrowed identity text. */
        [[nodiscard]] std::string_view KeyId() const noexcept;

    private:
        VerifiedArtifactEvidence(const Sha256Digest &digest, std::string publisherId, std::string keyId);
        Sha256Digest digest_;
        std::string publisherId_;
        std::string keyId_;
        friend class ArtifactVerifier;
    };

    /** @brief Verifies exact artifact bytes against a detached envelope and explicit trust roots. */
    class ArtifactVerifier final {
    public:
        /**
         * @brief Creates a verifier from an explicit crypto provider and trusted-root store.
         * @param provider Crypto provider; null fails closed.
         * @param trustedRoots Host-owned trusted roots; null fails closed.
         */
        ArtifactVerifier(std::shared_ptr<const SignatureProvider> provider, std::shared_ptr<const TrustedRootStore> trustedRoots);
        /**
         * @brief Verifies integrity, trust, algorithm, and signature for exact artifact bytes.
         * @param artifact Exact bytes proposed for activation.
         * @param envelope Detached signature envelope.
         * @return Unforgeable evidence or a typed failure.
         */
        [[nodiscard]] Result<VerifiedArtifactEvidence> Verify(std::span<const std::byte> artifact,
                                                              const DetachedSignatureEnvelope &envelope) const;
        /**
         * @brief Streams an exact private regular file through SHA-256 before verifying its detached signature.
         * @param path Quiescent private file; the caller prevents modification until the verified bytes are consumed.
         * @param expectedBytes Exact expected length, used to reject truncation and growth during the read.
         * @param envelope Detached signature envelope.
         * @return Evidence only when the complete file length, digest, trust root, and signature match.
         */
        [[nodiscard]] Result<VerifiedArtifactEvidence> VerifyFile(const std::filesystem::path &path, std::uint64_t expectedBytes,
                                                                  const DetachedSignatureEnvelope &envelope) const;

    private:
        [[nodiscard]] Result<VerifiedArtifactEvidence> VerifyDigest(const Sha256Digest &digest,
                                                                    const DetachedSignatureEnvelope &envelope) const;
        std::shared_ptr<const SignatureProvider> provider_;
        std::shared_ptr<const TrustedRootStore> trustedRoots_;
    };

    /** @brief Mandatory host-supplied gate evaluated immediately before native library loading. */
    class NativeArtifactGate {
    public:
        virtual ~NativeArtifactGate() = default;
        /** @brief Verifies a native artifact before loading. @param artifactPath Exact artifact path. @return Evidence or typed failure. */
        [[nodiscard]] virtual Result<VerifiedArtifactEvidence> Verify(const std::filesystem::path &artifactPath) const = 0;
    };

    /** @brief Host-composed detached-envelope gate for native files. */
    class DetachedFileArtifactGate final : public NativeArtifactGate {
    public:
        using EnvelopeResolver = std::function<Result<DetachedSignatureEnvelope>(const std::filesystem::path &)>;

        /**
         * @brief Creates a gate from an exact-byte verifier and explicit envelope resolver.
         * @param verifier Trust and signature verifier.
         * @param resolver Host policy that obtains the envelope bound to an artifact path.
         */
        DetachedFileArtifactGate(std::shared_ptr<const ArtifactVerifier> verifier, EnvelopeResolver resolver);

        /** @copydoc NativeArtifactGate::Verify */
        [[nodiscard]] Result<VerifiedArtifactEvidence> Verify(const std::filesystem::path &artifactPath) const override;

    private:
        std::shared_ptr<const ArtifactVerifier> verifier_;
        EnvelopeResolver resolver_;
    };

    /** @brief Creates the portable MbedTLS ECDSA-P256/SHA-256 verification provider. @return Shared immutable provider. */
    [[nodiscard]] std::shared_ptr<const SignatureProvider> CreateMbedTlsSignatureProvider();

    /** @brief Computes the domain-separated digest signed by SEC-001 envelopes. @param envelope Envelope identities and artifact digest.
     * @return Payload digest. */
    [[nodiscard]] Sha256Digest ComputeSignaturePayloadDigest(const DetachedSignatureEnvelope &envelope);
}  // namespace Horo::Security
