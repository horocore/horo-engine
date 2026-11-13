#include "Horo/PlatformServices/PlatformSessionBinding.h"

#include <algorithm>
#include <utility>

namespace Horo::PlatformServices {
    namespace PlatformIdentityBindingErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.identity"};
        }

        const ErrorCodeDescriptor BindingFailed{Domain,
                                                ErrorCode{"platform.identity.binding_failed"},
                                                ErrorSeverity::Error,
                                                "Protected platform binding evidence is incomplete or inconsistent.",
                                                "Resolve the authenticated binding through its private identity broker.",
                                                false,
                                                false};
        const ErrorCodeDescriptor BindingMismatch{Domain,
                                                  ErrorCode{"platform.identity.binding_mismatch"},
                                                  ErrorSeverity::Error,
                                                  "The verified session does not match the expected protected binding.",
                                                  "Keep the old durable partition suspended; never retarget the current account.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor
            MappingUnavailable{Domain,
                               ErrorCode{"platform.identity.mapping_unavailable"},
                               ErrorSeverity::Error,
                               "A stable authenticated platform binding is unavailable.",
                               "Keep account-bound persistence unavailable; do not infer a mapping from personal data.",
                               false,
                               false};
    }  // namespace PlatformIdentityBindingErrors

    namespace {
        /** @brief Rejects reserved zero values without treating protected storage identities as live subject nonces. */
        bool HasNonzeroByte(const std::array<std::byte, 16> &value) noexcept {
            return std::ranges::any_of(value, [](const std::byte byte) {
                return byte != std::byte{};
            });
        }

        /** @brief Validates inert provider/session composition without invoking any provider code. */
        Result<void> ValidateComposition(const PlatformServiceCapabilitySnapshot &provider, const PlatformSessionSnapshot &session) {
            if (const auto valid = ValidatePlatformServiceCapabilitySnapshot(provider, {}); valid.HasError())
                return Result<void>::Failure(WithCause(MakeError(PlatformIdentityBindingErrors::BindingFailed), valid.ErrorValue()));
            if (provider.providerGeneration != session.ProviderGeneration())
                return Result<void>::Failure(MakeError(PlatformIdentityBindingErrors::BindingFailed));
            return Result<void>::Success();
        }

        /** @brief Preserves typed inactive-phase errors when no live subject exists to capture. */
        const ErrorCodeDescriptor &InactiveError(const PlatformSessionPhase phase) noexcept {
            switch (phase) {
                case PlatformSessionPhase::Authenticating:
                    return PlatformSessionErrors::Authenticating;
                case PlatformSessionPhase::Closing:
                    return PlatformSessionErrors::Closing;
                case PlatformSessionPhase::Failed:
                    return PlatformSessionErrors::Failed;
                default:
                    return PlatformSessionErrors::NoSubject;
            }
        }
    }  // namespace

    /** @copydoc PlatformSessionBindingIssuer::Create */
    PlatformSessionBindingIssuer PlatformSessionBindingIssuer::Create() {
        return PlatformSessionBindingIssuer(std::make_shared<const Identity>());
    }

    /** @copydoc PlatformSessionBindingIssuer::PlatformSessionBindingIssuer */
    PlatformSessionBindingIssuer::PlatformSessionBindingIssuer(std::shared_ptr<const Identity> identity) noexcept
        : identity_(std::move(identity)) {}

    /** @copydoc PlatformSessionBindingIssuer::Close */
    void PlatformSessionBindingIssuer::Close() noexcept {
        identity_.reset();
    }

    /** @copydoc PlatformSessionBindingIssuer::IsOpen */
    bool PlatformSessionBindingIssuer::IsOpen() const noexcept {
        return static_cast<bool>(identity_);
    }

    /** @copydoc PlatformSessionBindingIssuer::Owns */
    bool PlatformSessionBindingIssuer::Owns(const std::weak_ptr<const void> &identity) const noexcept {
        return identity_ && !identity.owner_before(identity_) && !identity_.owner_before(identity);
    }

    /** @copydoc ProviderAccountBindingId::IsValid */
    bool ProviderAccountBindingId::IsValid() const noexcept {
        return !issuer_.expired() && provider_.IsValid() && HasNonzeroByte(scope_) && HasNonzeroByte(pseudonym_);
    }

    /** @copydoc ProviderAccountBindingId::operator== */
    bool ProviderAccountBindingId::operator==(const ProviderAccountBindingId &other) const noexcept {
        return provider_ == other.provider_ && scope_ == other.scope_ && pseudonym_ == other.pseudonym_ &&
               !issuer_.owner_before(other.issuer_) && !other.issuer_.owner_before(issuer_);
    }

    /** @copydoc ProviderAccountBindingId::Provider */
    PlatformProviderId ProviderAccountBindingId::Provider() const noexcept {
        return provider_;
    }

    /** @copydoc PlatformSessionBindingEvidence::IsValid */
    bool PlatformSessionBindingEvidence::IsValid() const noexcept {
        return binding_.IsValid() && subject_.IsValid() && accessRevision_.IsValid();
    }

    /** @copydoc PlatformSessionBindingEvidence::Binding */
    const ProviderAccountBindingId &PlatformSessionBindingEvidence::Binding() const noexcept {
        return binding_;
    }

    /** @copydoc PlatformSessionBindingEvidence::Subject */
    PlatformSubjectHandle PlatformSessionBindingEvidence::Subject() const noexcept {
        return subject_;
    }

    /** @copydoc PlatformSessionBindingEvidence::AccessRevision */
    PlatformAccessPolicyRevision PlatformSessionBindingEvidence::AccessRevision() const noexcept {
        return accessRevision_;
    }

    /** @copydoc BuildProviderAccountBindingId */
    Result<ProviderAccountBindingId> BuildProviderAccountBindingId(const PlatformSessionBindingIssuer &issuer,
                                                                   const ProviderAccountBindingCandidate &candidate) {
        if (!issuer.IsOpen())
            return Result<ProviderAccountBindingId>::Failure(MakeError(PlatformIdentityBindingErrors::MappingUnavailable));
        ProviderAccountBindingId binding;
        binding.provider_ = candidate.provider;
        binding.scope_ = candidate.productEnvironmentScope;
        binding.pseudonym_ = candidate.protectedPseudonym;
        binding.issuer_ = issuer.identity_;
        if (!binding.IsValid())
            return Result<ProviderAccountBindingId>::Failure(MakeError(PlatformIdentityBindingErrors::BindingFailed));
        return Result<ProviderAccountBindingId>::Success(binding);
    }

    /** @copydoc BuildPlatformSessionBindingEvidence */
    Result<PlatformSessionBindingEvidence> BuildPlatformSessionBindingEvidence(const PlatformSessionBindingIssuer &issuer,
                                                                               const ProviderAccountBindingId &binding,
                                                                               const PlatformServiceCapabilitySnapshot &provider,
                                                                               const PlatformSessionSnapshot &session) {
        if (!issuer.IsOpen())
            return Result<PlatformSessionBindingEvidence>::Failure(MakeError(PlatformIdentityBindingErrors::MappingUnavailable));
        if (const auto valid = ValidateComposition(provider, session); valid.HasError())
            return Result<PlatformSessionBindingEvidence>::Failure(valid.ErrorValue());
        if (!binding.IsValid())
            return Result<PlatformSessionBindingEvidence>::Failure(MakeError(PlatformIdentityBindingErrors::BindingFailed));
        if (!issuer.Owns(binding.issuer_) || binding.Provider() != provider.provider)
            return Result<PlatformSessionBindingEvidence>::Failure(MakeError(PlatformIdentityBindingErrors::BindingMismatch));
        if (!session.Subject())
            return Result<PlatformSessionBindingEvidence>::Failure(MakeError(InactiveError(session.Phase())));
        PlatformSessionBindingEvidence evidence;
        evidence.binding_ = binding;
        evidence.subject_ = *session.Subject();
        evidence.accessRevision_ = session.AccessRevision();
        return Result<PlatformSessionBindingEvidence>::Success(evidence);
    }

    /** @copydoc ValidatePlatformSessionBindingAccess */
    Result<void> ValidatePlatformSessionBindingAccess(const PlatformServiceCapabilitySnapshot &provider,
                                                      const PlatformSessionSnapshot &session,
                                                      const PlatformSessionBindingEvidence &evidence,
                                                      const ProviderAccountBindingId &expectedBinding, const PlatformServiceKind service) {
        if (const auto valid = ValidateComposition(provider, session); valid.HasError())
            return valid;
        if (!evidence.IsValid() || !expectedBinding.IsValid() || service >= PlatformServiceKind::Count)
            return Result<void>::Failure(MakeError(PlatformIdentityBindingErrors::BindingFailed));
        const auto capability = std::ranges::find_if(provider.services, [service](const auto &entry) {
            return entry.service == service;
        });
        if (capability->availability != PlatformServiceAvailability::Available)
            return Result<void>::Failure(MakeError(capability->unavailableReason == PlatformServiceUnavailableReason::NullProviderSelected
                                                       ? BackendErrors::NullProvider
                                                       : BackendErrors::ServiceUnavailable));
        if (const auto access = ValidatePlatformSessionAccess(session, evidence.Subject(), evidence.AccessRevision(), service);
            access.HasError())
            return access;
        if (evidence.Binding() != expectedBinding || expectedBinding.Provider() != provider.provider)
            return Result<void>::Failure(MakeError(PlatformIdentityBindingErrors::BindingMismatch));
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformServices
