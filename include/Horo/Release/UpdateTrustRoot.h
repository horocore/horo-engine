#pragma once

/**
 * @file UpdateTrustRoot.h
 * @brief Versioned signed key rotation for installed update trust roots.
 */

#include "Horo/Release/DistributionModel.h"
#include "Horo/Security/ArtifactSignature.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Release {
    /** @brief Complete replacement root state authenticated by its installed predecessor. */
    struct UpdateTrustRootData final {
        DistributionProductIdentity product;
        std::uint64_t revision{};
        std::uint64_t parentRevision{};
        std::uint64_t minimumManifestSequence{};
        std::uint64_t expiresAt{}; /**< Unix seconds in UTC. */
        std::vector<Security::TrustedSigningKey> keys;
    };

    /**
     * @brief Canonicalizes a proposed root revision for signing by an installed old key.
     * @param data Bounded complete replacement key set.
     * @return Exact bytes to sign, or a typed invalid-metadata error.
     */
    [[nodiscard]] Result<std::string> BuildCanonicalUpdateTrustRootPayload(const UpdateTrustRootData &data);

    /** @brief Immutable signed root-transition document from an untrusted source. */
    class SignedUpdateTrustRoot final {
    public:
        /**
         * @brief Creates canonical signed transition bytes after validating the supplied envelope digest.
         * @param data Proposed root and monotonic state.
         * @param signature Signature made by an installed old trusted key.
         * @return Canonical transition or invalid-metadata failure.
         */
        [[nodiscard]] static Result<SignedUpdateTrustRoot> Create(UpdateTrustRootData data, Security::DetachedSignatureEnvelope signature);
        /**
         * @brief Parses only bounded exact canonical schema-v1 transition bytes.
         * @param bytes Complete signed transition document.
         * @return Parsed document or invalid-metadata failure.
         */
        [[nodiscard]] static Result<SignedUpdateTrustRoot> ParseCanonical(std::string_view bytes);

        /** @brief Returns replacement root data. @return Borrowed immutable data. */
        [[nodiscard]] const UpdateTrustRootData &Data() const noexcept;
        /** @brief Returns exact signed payload bytes. @return Borrowed canonical JSON. */
        [[nodiscard]] const std::string &CanonicalPayload() const noexcept;
        /** @brief Returns exact signed document bytes. @return Borrowed canonical JSON. */
        [[nodiscard]] const std::string &CanonicalDocument() const noexcept;
        /** @brief Returns old-root signature. @return Borrowed envelope. */
        [[nodiscard]] const Security::DetachedSignatureEnvelope &Signature() const noexcept;

    private:
        SignedUpdateTrustRoot(UpdateTrustRootData data, Security::DetachedSignatureEnvelope signature, std::string payload,
                              std::string document);
        UpdateTrustRootData data_;
        Security::DetachedSignatureEnvelope signature_;
        std::string payload_;
        std::string document_;
    };

    /** @brief Host-owned installed root snapshot; transition returns a replacement without mutating the old root. */
    class UpdateTrustRootSnapshot final {
    public:
        /**
         * @brief Admits an installer-authenticated initial root, never bytes from an update transport.
         * @param trusted Installer-owned root data with revision one and parent revision zero.
         * @return Installed snapshot or invalid-root failure.
         */
        [[nodiscard]] static Result<UpdateTrustRootSnapshot> Bootstrap(UpdateTrustRootData trusted);

        /**
         * @brief Verifies a one-step signed key rotation against the current installed roots.
         * @param proposed Canonical untrusted transition document.
         * @param provider Installed signature provider; null fails closed.
         * @param now Trusted host time in Unix seconds.
         * @return Independent replacement snapshot for durable host commit.
         */
        [[nodiscard]] Result<UpdateTrustRootSnapshot> Transition(const SignedUpdateTrustRoot &proposed,
                                                                 std::shared_ptr<const Security::SignatureProvider> provider,
                                                                 std::uint64_t now) const;

        /** @brief Returns immutable product scope. @return Product identity. */
        [[nodiscard]] const DistributionProductIdentity &Product() const noexcept;
        /** @brief Returns monotonic root revision. @return Installed revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Returns monotonic accepted manifest sequence floor. @return Sequence floor. */
        [[nodiscard]] std::uint64_t MinimumManifestSequence() const noexcept;
        /** @brief Returns signed root expiration time. @return Unix seconds in UTC. */
        [[nodiscard]] std::uint64_t ExpiresAt() const noexcept;
        /** @brief Returns roots for exact signature verification. @return Shared immutable store. */
        [[nodiscard]] std::shared_ptr<const Security::TrustedRootStore> Roots() const noexcept;

    private:
        UpdateTrustRootSnapshot(UpdateTrustRootData data, std::shared_ptr<const Security::TrustedRootStore> roots);
        UpdateTrustRootData data_;
        std::shared_ptr<const Security::TrustedRootStore> roots_;
    };
}  // namespace Horo::Release
