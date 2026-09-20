#pragma once

/**
 * @file PackagePublisherVerification.h
 * @brief Bounded publisher identity, signature verification, and audit contracts.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Packages/PackageDependencyResolver.h"
#include "Horo/Security/ArtifactSignature.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Packages {
    /** @brief Canonical publisher identity used by package signature policy. */
    class PackagePublisherId {
    public:
        /**
         * @brief Parses a bounded lowercase reverse-domain-style publisher identity.
         * @param text Candidate publisher identity.
         * @return Parsed identity or a typed invalid-input failure.
         */
        [[nodiscard]] static Result<PackagePublisherId> Parse(std::string_view text);
        /** @brief Returns the canonical publisher identity. @return Stable identity storage owned by this value. */
        [[nodiscard]] const std::string &Value() const noexcept;
        [[nodiscard]] auto operator<=>(const PackagePublisherId &) const = default;

    private:
        explicit PackagePublisherId(std::string value);
        std::string value_;
    };

    /** @brief Explicit outcome recorded for each admitted publisher verification attempt. */
    enum class PackagePublisherVerificationOutcome : std::uint8_t {
        Accepted,
        Unsigned,
        RevokedPublisher,
        ExpiredPublisher,
        MismatchedPublisher,
        UnknownPublisher,
        UnknownSigningKey,
        MalformedInput,
        UnsupportedAlgorithm,
        IntegrityMismatch,
        InvalidSignature,
        VerificationUnavailable,
    };

    /** @brief Bounded publisher trust entry supplied by the host or policy owner. */
    struct PackagePublisherTrustRecord {
        PackagePublisherId publisher;
        std::string keyId;
        Security::SignatureAlgorithm algorithm{Security::SignatureAlgorithm::EcdsaP256Sha256};
        std::vector<std::byte> publicKey;
        std::uint64_t expiresAtUnixMilliseconds{}; /**< Zero means no configured expiry. */
        bool publisherRevoked{false};
    };

    /** @brief Resource ceilings applied before verification or audit publication. */
    struct PackagePublisherVerificationLimits {
        std::size_t maximumArtifactBytes = 64U * 1024U * 1024U;
        std::size_t maximumPublisherRecords = 1024U;
        std::size_t maximumPublisherIdBytes = 255U;
        std::size_t maximumKeyIdBytes = 256U;
        std::size_t maximumPublicKeyBytes = 1024U;
        std::size_t maximumSignatureBytes = 1024U;
    };

    /** @brief Immutable policy snapshot for one publisher verification service. */
    struct PackagePublisherVerificationPolicy {
        bool allowUnsigned{false};
        std::vector<PackagePublisherTrustRecord> publishers;
        PackagePublisherVerificationLimits limits;
        std::size_t auditCapacity{256U};
    };

    /** @brief One exact package artifact and detached signature proposed for installation. */
    struct PackagePublisherVerificationRequest {
        HoroPackageId package;
        std::vector<std::byte> artifact;
        std::optional<Security::DetachedSignatureEnvelope> signature;
        std::optional<PackagePublisherId> expectedPublisher;
        std::uint64_t nowUnixMilliseconds{};
        CancellationToken cancellation;
    };

    /** @brief One immutable policy decision produced by publisher verification. */
    struct PackagePublisherVerificationDecision {
        PackagePublisherVerificationOutcome outcome{PackagePublisherVerificationOutcome::MalformedInput};
        bool installPermitted{false};
        std::optional<PackagePublisherId> publisher;
        std::string keyId;
        Sha256Digest artifactDigest;
    };

    /** @brief Bounded audit record published atomically with a verification decision. */
    struct PackagePublisherAuditRecord {
        HoroPackageId package;
        std::optional<PackagePublisherId> publisher;
        std::string keyId;
        PackagePublisherVerificationOutcome outcome{PackagePublisherVerificationOutcome::MalformedInput};
        bool installPermitted{false};
        std::uint64_t atUnixMilliseconds{};
        std::uint64_t revision{};
    };

    /** @brief Decision and its corresponding audit record. */
    struct PackagePublisherVerificationResult {
        PackagePublisherVerificationDecision decision;
        PackagePublisherAuditRecord audit;
    };

    /** @brief Lifecycle state of the bounded publisher verification service. */
    enum class PackagePublisherVerificationLifecycleState : std::uint8_t {
        Ready,
        RecoverableFailure,
        Closed,
    };

    /** @brief Observable publisher verification lifecycle counters and state. */
    struct PackagePublisherVerificationState {
        PackagePublisherVerificationLifecycleState lifecycle{PackagePublisherVerificationLifecycleState::Ready};
        std::uint64_t revision{};
        std::uint64_t attempts{};
    };

    /** @brief Host-owned, backend-neutral publisher verification service used before package installation. */
    class PackagePublisherVerificationService final {
    public:
        /**
         * @brief Creates a bounded service from explicit trust policy and an injected signature provider.
         * @param policy Trust entries, resource bounds, unsigned policy, and audit capacity.
         * @param provider Injected backend-neutral signature provider; null starts in a recoverable state.
         * @return Ready service or a typed policy failure.
         */
        [[nodiscard]] static Result<PackagePublisherVerificationService> Create(
            PackagePublisherVerificationPolicy policy, std::shared_ptr<const Security::SignatureProvider> provider);

        PackagePublisherVerificationService(PackagePublisherVerificationService &&) noexcept;
        PackagePublisherVerificationService &operator=(PackagePublisherVerificationService &&) noexcept;
        PackagePublisherVerificationService(const PackagePublisherVerificationService &) = delete;
        PackagePublisherVerificationService &operator=(const PackagePublisherVerificationService &) = delete;
        ~PackagePublisherVerificationService();

        /**
         * @brief Verifies one exact artifact and publishes one complete audit outcome.
         * @param request Bounded artifact, optional detached envelope, expected publisher, clock snapshot, and cancellation token.
         * @return A policy outcome, or a typed failure when publication is cancelled, closed, or capacity is exhausted.
         */
        [[nodiscard]] Result<PackagePublisherVerificationResult> Verify(const PackagePublisherVerificationRequest &request) const;

        /**
         * @brief Replaces the injected provider after a recoverable provider failure.
         * @param provider New provider; null leaves the service recoverable and fails closed.
         * @return Success, or lifecycle-closed failure.
         */
        [[nodiscard]] Result<void> SetProvider(std::shared_ptr<const Security::SignatureProvider> provider);

        /** @brief Closes verification admission without discarding published audit records. @return Success or a typed lifecycle failure.
         */
        [[nodiscard]] Result<void> Shutdown();

        /** @brief Returns a synchronized snapshot of lifecycle state. @return Current state snapshot. */
        [[nodiscard]] PackagePublisherVerificationState State() const;

        /** @brief Returns a synchronized copy of all published audit records. @return Bounded audit history snapshot. */
        [[nodiscard]] std::vector<PackagePublisherAuditRecord> AuditRecords() const;

    private:
        struct Impl;

        explicit PackagePublisherVerificationService(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Packages
