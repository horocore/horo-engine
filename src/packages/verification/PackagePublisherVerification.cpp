#include "Horo/Packages/PackagePublisherVerification.h"

#include "Horo/Packages/PackagePublisherVerificationErrors.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <utility>

namespace Horo::Packages {
    namespace {
        [[nodiscard]] bool IsCanonicalIdentityCharacter(const unsigned char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' || value == '-' || value == '_';
        }

        [[nodiscard]] bool ValidBound(const std::size_t value, const std::size_t maximum) noexcept {
            return value != 0U && value <= maximum;
        }

        constexpr std::size_t MaximumAuditCapacity = 100'000U;
        constexpr std::size_t MaximumArtifactBytes = 1024U * 1024U * 1024U;
        constexpr std::size_t MaximumPublisherRecords = 1024U;
        constexpr std::size_t MaximumPublisherIdBytes = 255U;
        constexpr std::size_t MaximumKeyIdBytes = 256U;
        constexpr std::size_t MaximumPublicKeyBytes = 1024U;
        constexpr std::size_t MaximumSignatureBytes = 1024U;

        [[nodiscard]] bool IsCanonicalIdentity(const std::string_view text, const std::size_t maximum) noexcept {
            if (text.empty())
                return false;
            if (text.size() > maximum)
                return false;
            if (text.front() == '.' || text.back() == '.')
                return false;
            if (text.find("..") != std::string_view::npos)
                return false;
            return std::ranges::all_of(text, IsCanonicalIdentityCharacter);
        }

        [[nodiscard]] bool IsPrintableIdentity(const std::string_view text, const std::size_t maximum) noexcept {
            return !text.empty() && text.size() <= maximum && std::ranges::all_of(text, [](const unsigned char value) {
                return value >= 0x21U && value <= 0x7eU;
            });
        }

        [[nodiscard]] bool IsSupportedAlgorithm(const Security::SignatureAlgorithm algorithm) noexcept {
            return algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256;
        }

        [[nodiscard]] bool IsSecurityError(const Error &error, const ErrorCodeDescriptor &descriptor) noexcept {
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }

        [[nodiscard]] Error MakeClosedError() {
            return MakeError(PublisherVerificationErrors::LifecycleClosed);
        }

        [[nodiscard]] Error MakeCancelledError() {
            return MakeError(PublisherVerificationErrors::Cancelled);
        }
    }  // namespace

    struct PackagePublisherVerificationService::Impl final {
        explicit Impl(PackagePublisherVerificationPolicy policy, std::shared_ptr<const Security::SignatureProvider> provider)
            : policy(std::move(policy)), provider(std::move(provider)) {
            audit.reserve(this->policy.auditCapacity);
        }

    private:
        friend class PackagePublisherVerificationService;

        PackagePublisherVerificationPolicy policy;
        std::shared_ptr<const Security::SignatureProvider> provider;
        PackagePublisherVerificationState state;
        std::vector<PackagePublisherAuditRecord> audit;
        mutable std::mutex mutex;
    };

    namespace {
        [[nodiscard]] bool ValidLimits(const PackagePublisherVerificationLimits &limits) noexcept {
            if (!ValidBound(limits.maximumArtifactBytes, MaximumArtifactBytes))
                return false;
            if (!ValidBound(limits.maximumPublisherRecords, MaximumPublisherRecords))
                return false;
            if (!ValidBound(limits.maximumPublisherIdBytes, MaximumPublisherIdBytes))
                return false;
            if (!ValidBound(limits.maximumKeyIdBytes, MaximumKeyIdBytes))
                return false;
            if (!ValidBound(limits.maximumPublicKeyBytes, MaximumPublicKeyBytes))
                return false;
            if (!ValidBound(limits.maximumSignatureBytes, MaximumSignatureBytes))
                return false;
            return true;
        }

        [[nodiscard]] bool ValidTrustRecord(const PackagePublisherTrustRecord &publisher,
                                            const PackagePublisherVerificationLimits &limits) noexcept {
            return IsCanonicalIdentity(publisher.publisher.Value(), limits.maximumPublisherIdBytes) &&
                   IsPrintableIdentity(publisher.keyId, limits.maximumKeyIdBytes) && IsSupportedAlgorithm(publisher.algorithm) &&
                   !publisher.publicKey.empty() && publisher.publicKey.size() <= limits.maximumPublicKeyBytes;
        }

        [[nodiscard]] bool HasDuplicateTrustRecord(const std::vector<PackagePublisherTrustRecord> &publishers,
                                                   const PackagePublisherTrustRecord &publisher) {
            return std::ranges::any_of(publishers, [&](const auto &candidate) {
                return &candidate != &publisher && candidate.publisher == publisher.publisher && candidate.keyId == publisher.keyId;
            });
        }

        [[nodiscard]] Result<void> ValidatePolicy(const PackagePublisherVerificationPolicy &policy) {
            const auto &limits = policy.limits;
            if (policy.auditCapacity == 0U || policy.auditCapacity > MaximumAuditCapacity || !ValidLimits(limits) ||
                policy.publishers.size() > limits.maximumPublisherRecords)
                return Result<void>::Failure(MakeError(PublisherVerificationErrors::InvalidInput));

            for (const auto &publisher : policy.publishers) {
                if (!ValidTrustRecord(publisher, limits) || HasDuplicateTrustRecord(policy.publishers, publisher))
                    return Result<void>::Failure(MakeError(PublisherVerificationErrors::InvalidInput));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequestBounds(const PackagePublisherVerificationPolicy &policy,
                                                         const PackagePublisherVerificationRequest &request) {
            const auto &limits = policy.limits;
            if (request.artifact.size() > limits.maximumArtifactBytes)
                return Result<void>::Failure(MakeError(PublisherVerificationErrors::ResourceLimit));
            if (request.signature && request.signature->signature.size() > limits.maximumSignatureBytes)
                return Result<void>::Failure(MakeError(PublisherVerificationErrors::ResourceLimit));
            if (request.signature && request.signature->publisherId.size() > limits.maximumPublisherIdBytes)
                return Result<void>::Failure(MakeError(PublisherVerificationErrors::ResourceLimit));
            if (request.signature && request.signature->keyId.size() > limits.maximumKeyIdBytes)
                return Result<void>::Failure(MakeError(PublisherVerificationErrors::ResourceLimit));
            if (request.expectedPublisher && request.expectedPublisher->Value().size() > limits.maximumPublisherIdBytes)
                return Result<void>::Failure(MakeError(PublisherVerificationErrors::ResourceLimit));
            return Result<void>::Success();
        }

        [[nodiscard]] PackagePublisherVerificationResult BuildResult(const PackagePublisherVerificationRequest &request,
                                                                     const PackagePublisherVerificationDecision &decision,
                                                                     const std::uint64_t revision) {
            return {.decision = decision,
                    .audit = {.package = request.package,
                              .publisher = decision.publisher,
                              .keyId = decision.keyId,
                              .outcome = decision.outcome,
                              .installPermitted = decision.installPermitted,
                              .atUnixMilliseconds = request.nowUnixMilliseconds,
                              .revision = revision}};
        }

        [[nodiscard]] PackagePublisherVerificationDecision Decision(const PackagePublisherVerificationOutcome outcome,
                                                                    const bool installPermitted,
                                                                    std::optional<PackagePublisherId> publisher = std::nullopt,
                                                                    std::string keyId = {}, const Sha256Digest &digest = {}) {
            return {.outcome = outcome,
                    .installPermitted = installPermitted,
                    .publisher = std::move(publisher),
                    .keyId = std::move(keyId),
                    .artifactDigest = digest};
        }

        struct Evaluation final {
            PackagePublisherVerificationDecision decision;
            PackagePublisherVerificationLifecycleState lifecycle{PackagePublisherVerificationLifecycleState::Ready};
        };

        [[nodiscard]] Evaluation Outcome(const PackagePublisherVerificationOutcome outcome, const bool installPermitted,
                                         const PackagePublisherVerificationLifecycleState lifecycle,
                                         std::optional<PackagePublisherId> publisher = std::nullopt, std::string keyId = {},
                                         const Sha256Digest &digest = {}) {
            return {.decision = Decision(outcome, installPermitted, std::move(publisher), std::move(keyId), digest),
                    .lifecycle = lifecycle};
        }

        [[nodiscard]] Result<Evaluation> EvaluateProvider(const std::shared_ptr<const Security::SignatureProvider> &provider,
                                                          const PackagePublisherVerificationRequest &request,
                                                          const PackagePublisherId &publisher, const std::string_view keyId,
                                                          const Sha256Digest &artifactDigest,
                                                          const Security::DetachedSignatureEnvelope &envelope,
                                                          const std::vector<std::byte> &publicKey) {
            using enum PackagePublisherVerificationLifecycleState;
            using enum PackagePublisherVerificationOutcome;
            if (request.cancellation.IsCancellationRequested())
                return Result<Evaluation>::Failure(MakeCancelledError());
            if (!provider)
                return Result<Evaluation>::Success(
                    Outcome(VerificationUnavailable, false, RecoverableFailure, publisher, std::string{keyId}, artifactDigest));
            if (!provider->Supports(envelope.algorithm))
                return Result<Evaluation>::Success(
                    Outcome(UnsupportedAlgorithm, false, Ready, publisher, std::string{keyId}, artifactDigest));

            const auto verified =
                provider->Verify(envelope.algorithm, publicKey, ComputeSignaturePayloadDigest(envelope), envelope.signature);
            if (request.cancellation.IsCancellationRequested())
                return Result<Evaluation>::Failure(MakeCancelledError());
            if (verified.HasValue())
                return Result<Evaluation>::Success(Outcome(Accepted, true, Ready, publisher, std::string{keyId}, artifactDigest));

            if (IsSecurityError(verified.ErrorValue(), SecurityErrors::InvalidSignature))
                return Result<Evaluation>::Success(Outcome(InvalidSignature, false, Ready, publisher, std::string{keyId}, artifactDigest));
            if (IsSecurityError(verified.ErrorValue(), SecurityErrors::UnsupportedAlgorithm))
                return Result<Evaluation>::Success(
                    Outcome(UnsupportedAlgorithm, false, Ready, publisher, std::string{keyId}, artifactDigest));
            return Result<Evaluation>::Success(
                Outcome(VerificationUnavailable, false, RecoverableFailure, publisher, std::string{keyId}, artifactDigest));
        }

        [[nodiscard]] Result<Evaluation> EvaluateTrusted(const PackagePublisherVerificationPolicy &policy,
                                                         const std::shared_ptr<const Security::SignatureProvider> &provider,
                                                         const PackagePublisherVerificationRequest &request,
                                                         const PackagePublisherId &publisher, const Sha256Digest &artifactDigest,
                                                         const Security::DetachedSignatureEnvelope &envelope) {
            if (const auto publisherEnd = std::ranges::find_if(policy.publishers,
                                                               [&](const auto &candidate) {
                return candidate.publisher == publisher;
            });
                publisherEnd == policy.publishers.end())
                return Result<Evaluation>::Success(Outcome(PackagePublisherVerificationOutcome::UnknownPublisher, false,
                                                           PackagePublisherVerificationLifecycleState::Ready, publisher, envelope.keyId,
                                                           artifactDigest));
            if (std::ranges::any_of(policy.publishers, [&](const auto &candidate) {
                return candidate.publisher == publisher && candidate.publisherRevoked;
            }))
                return Result<Evaluation>::Success(Outcome(PackagePublisherVerificationOutcome::RevokedPublisher, false,
                                                           PackagePublisherVerificationLifecycleState::Ready, publisher, envelope.keyId,
                                                           artifactDigest));

            const auto trustedKey = std::ranges::find_if(policy.publishers, [&](const auto &candidate) {
                return candidate.publisher == publisher && candidate.keyId == envelope.keyId;
            });
            if (trustedKey == policy.publishers.end())
                return Result<Evaluation>::Success(Outcome(PackagePublisherVerificationOutcome::UnknownSigningKey, false,
                                                           PackagePublisherVerificationLifecycleState::Ready, publisher, envelope.keyId,
                                                           artifactDigest));
            if (trustedKey->expiresAtUnixMilliseconds != 0U && request.nowUnixMilliseconds >= trustedKey->expiresAtUnixMilliseconds)
                return Result<Evaluation>::Success(Outcome(PackagePublisherVerificationOutcome::ExpiredPublisher, false,
                                                           PackagePublisherVerificationLifecycleState::Ready, publisher, envelope.keyId,
                                                           artifactDigest));
            if (trustedKey->algorithm != envelope.algorithm)
                return Result<Evaluation>::Success(Outcome(PackagePublisherVerificationOutcome::UnsupportedAlgorithm, false,
                                                           PackagePublisherVerificationLifecycleState::Ready, publisher, envelope.keyId,
                                                           artifactDigest));
            if (envelope.artifactDigest != artifactDigest)
                return Result<Evaluation>::Success(Outcome(PackagePublisherVerificationOutcome::IntegrityMismatch, false,
                                                           PackagePublisherVerificationLifecycleState::Ready, publisher, envelope.keyId,
                                                           artifactDigest));
            return EvaluateProvider(provider, request, publisher, envelope.keyId, artifactDigest, envelope, trustedKey->publicKey);
        }

        [[nodiscard]] Result<Evaluation> EvaluateSigned(const PackagePublisherVerificationPolicy &policy,
                                                        const std::shared_ptr<const Security::SignatureProvider> &provider,
                                                        const PackagePublisherVerificationRequest &request,
                                                        const Sha256Digest &artifactDigest) {
            using enum PackagePublisherVerificationLifecycleState;
            using enum PackagePublisherVerificationOutcome;
            const auto &envelope = *request.signature;
            if (!IsSupportedAlgorithm(envelope.algorithm))
                return Result<Evaluation>::Success(Outcome(UnsupportedAlgorithm, false, Ready));

            const auto parsedPublisher = PackagePublisherId::Parse(envelope.publisherId);
            if (request.artifact.empty() || parsedPublisher.HasError() || envelope.keyId.empty() || envelope.signature.empty())
                return Result<Evaluation>::Success(Outcome(MalformedInput, false, Ready));
            const PackagePublisherId publisher = parsedPublisher.Value();
            if (request.expectedPublisher && publisher != *request.expectedPublisher)
                return Result<Evaluation>::Success(Outcome(MismatchedPublisher, false, Ready, publisher, envelope.keyId, artifactDigest));
            return EvaluateTrusted(policy, provider, request, publisher, artifactDigest, envelope);
        }

        [[nodiscard]] Evaluation EvaluateUnsigned(const PackagePublisherVerificationPolicy &policy,
                                                  const PackagePublisherVerificationRequest &request, const Sha256Digest &artifactDigest) {
            if (request.artifact.empty())
                return Outcome(PackagePublisherVerificationOutcome::MalformedInput, false,
                               PackagePublisherVerificationLifecycleState::Ready, std::nullopt, {}, artifactDigest);
            return Outcome(PackagePublisherVerificationOutcome::Unsigned, policy.allowUnsigned,
                           PackagePublisherVerificationLifecycleState::Ready, std::nullopt, {}, artifactDigest);
        }
    }  // namespace

    /** @copydoc PackagePublisherId::Parse */
    Result<PackagePublisherId> PackagePublisherId::Parse(const std::string_view text) {
        if (!IsCanonicalIdentity(text, 255U))
            return Result<PackagePublisherId>::Failure(MakeError(PublisherVerificationErrors::InvalidInput));
        return Result<PackagePublisherId>::Success(PackagePublisherId{std::string{text}});
    }

    /** @copydoc PackagePublisherId::PackagePublisherId */
    PackagePublisherId::PackagePublisherId(std::string value) : value_(std::move(value)) {}

    /** @copydoc PackagePublisherId::Value */
    const std::string &PackagePublisherId::Value() const noexcept {
        return value_;
    }

    /** @copydoc PackagePublisherVerificationService::Create */
    Result<PackagePublisherVerificationService> PackagePublisherVerificationService::Create(
        PackagePublisherVerificationPolicy policy, std::shared_ptr<const Security::SignatureProvider> provider) {
        if (auto valid = ValidatePolicy(policy); valid.HasError())
            return Result<PackagePublisherVerificationService>::Failure(valid.ErrorValue());
        return Result<PackagePublisherVerificationService>::Success(
            PackagePublisherVerificationService{std::make_unique<Impl>(std::move(policy), std::move(provider))});
    }

    /** @copydoc PackagePublisherVerificationService::PackagePublisherVerificationService */
    PackagePublisherVerificationService::PackagePublisherVerificationService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    PackagePublisherVerificationService::PackagePublisherVerificationService(PackagePublisherVerificationService &&) noexcept = default;

    PackagePublisherVerificationService &PackagePublisherVerificationService::operator=(PackagePublisherVerificationService &&) noexcept =
        default;

    PackagePublisherVerificationService::~PackagePublisherVerificationService() = default;

    /** @copydoc PackagePublisherVerificationService::Verify */
    Result<PackagePublisherVerificationResult> PackagePublisherVerificationService::Verify(
        const PackagePublisherVerificationRequest &request) const {
        if (!impl_)
            return Result<PackagePublisherVerificationResult>::Failure(MakeClosedError());

        if (auto valid = ValidateRequestBounds(impl_->policy, request); valid.HasError())
            return Result<PackagePublisherVerificationResult>::Failure(valid.ErrorValue());
        const Sha256Digest artifactDigest = ComputeSha256(request.artifact);

        std::unique_lock lock{impl_->mutex};
        if (impl_->state.lifecycle == PackagePublisherVerificationLifecycleState::Closed)
            return Result<PackagePublisherVerificationResult>::Failure(MakeClosedError());
        if (request.cancellation.IsCancellationRequested())
            return Result<PackagePublisherVerificationResult>::Failure(MakeCancelledError());
        if (impl_->audit.size() >= impl_->policy.auditCapacity)
            return Result<PackagePublisherVerificationResult>::Failure(MakeError(PublisherVerificationErrors::AuditCapacityExceeded));

        Result<Evaluation> evaluation = request.signature
                                            ? EvaluateSigned(impl_->policy, impl_->provider, request, artifactDigest)
                                            : Result<Evaluation>::Success(EvaluateUnsigned(impl_->policy, request, artifactDigest));
        if (evaluation.HasError())
            return Result<PackagePublisherVerificationResult>::Failure(evaluation.ErrorValue());

        ++impl_->state.attempts;
        ++impl_->state.revision;
        impl_->state.lifecycle = evaluation.Value().lifecycle;
        auto result = BuildResult(request, evaluation.Value().decision, impl_->state.revision);
        impl_->audit.push_back(result.audit);
        return Result<PackagePublisherVerificationResult>::Success(std::move(result));
    }

    /** @copydoc PackagePublisherVerificationService::SetProvider */
    Result<void> PackagePublisherVerificationService::SetProvider(std::shared_ptr<const Security::SignatureProvider> provider) {
        using enum PackagePublisherVerificationLifecycleState;
        if (!impl_)
            return Result<void>::Failure(MakeClosedError());
        std::unique_lock lock{impl_->mutex};
        if (impl_->state.lifecycle == Closed)
            return Result<void>::Failure(MakeClosedError());
        impl_->provider = std::move(provider);
        impl_->state.lifecycle = impl_->provider ? Ready : RecoverableFailure;
        return Result<void>::Success();
    }

    /** @copydoc PackagePublisherVerificationService::Shutdown */
    Result<void> PackagePublisherVerificationService::Shutdown() {
        if (!impl_)
            return Result<void>::Failure(MakeClosedError());
        std::unique_lock lock{impl_->mutex};
        if (impl_->state.lifecycle == PackagePublisherVerificationLifecycleState::Closed)
            return Result<void>::Failure(MakeClosedError());
        impl_->state.lifecycle = PackagePublisherVerificationLifecycleState::Closed;
        ++impl_->state.revision;
        return Result<void>::Success();
    }

    /** @copydoc PackagePublisherVerificationService::State */
    PackagePublisherVerificationState PackagePublisherVerificationService::State() const {
        if (!impl_)
            return {.lifecycle = PackagePublisherVerificationLifecycleState::Closed};
        std::scoped_lock lock{impl_->mutex};
        return impl_->state;
    }

    /** @copydoc PackagePublisherVerificationService::AuditRecords */
    std::vector<PackagePublisherAuditRecord> PackagePublisherVerificationService::AuditRecords() const {
        if (!impl_)
            return {};
        std::scoped_lock lock{impl_->mutex};
        return impl_->audit;
    }
}  // namespace Horo::Packages
