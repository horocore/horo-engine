#include "Horo/PlatformServices/PlatformServicesFrontend.h"

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] const PlatformServiceCapability *FindCapability(const PlatformServiceCapabilitySnapshot &snapshot,
                                                                      const PlatformServiceKind service) noexcept {
            const auto found = std::ranges::find_if(snapshot.services, [service](const PlatformServiceCapability &entry) {
                return entry.service == service;
            });
            return found == snapshot.services.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool SameLimits(const PlatformServiceLimits &left, const PlatformServiceLimits &right) noexcept {
            return left.maxConcurrentRequests == right.maxConcurrentRequests && left.maxPageEntries == right.maxPageEntries &&
                   left.maxPayloadBytes == right.maxPayloadBytes;
        }

        [[nodiscard]] bool SameCapability(const PlatformServiceCapability &left, const PlatformServiceCapability &right) noexcept {
            return left.service == right.service && left.availability == right.availability && SameLimits(left.limits, right.limits) &&
                   left.binding == right.binding && left.unavailableReason == right.unavailableReason;
        }

        [[nodiscard]] bool SameSnapshot(const PlatformServiceCapabilitySnapshot &left,
                                        const PlatformServiceCapabilitySnapshot &right) noexcept {
            if (left.interfaceVersion != right.interfaceVersion || left.provider != right.provider ||
                left.providerGeneration != right.providerGeneration)
                return false;
            return std::ranges::is_permutation(left.services, right.services, SameCapability);
        }

        template <typename T> [[nodiscard]] Result<PlatformRequestHandle<T>> ValidatedDispatch(Result<PlatformRequestHandle<T>> result) {
            if (result.HasError())
                return result;
            if (!result.Value().IsValid())
                return Failure<PlatformRequestHandle<T>>(FrontendErrors::InvalidDispatchResult);
            return result;
        }
    }  // namespace

    namespace FrontendErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.frontend"};
        }

        const ErrorCodeDescriptor
            InvalidComposition{Domain,
                               ErrorCode{"platform.frontend.invalid_composition"},
                               ErrorSeverity::Error,
                               "Platform Services frontend composition is invalid.",
                               "Bind one activated backend, exact capability snapshot, and matching session generation.",
                               false,
                               false};
        const ErrorCodeDescriptor Unavailable{Domain,
                                              ErrorCode{"platform.frontend.unavailable"},
                                              ErrorSeverity::Error,
                                              "Platform Services frontend admission is closed.",
                                              "Submit only through the active frontend generation.",
                                              false,
                                              true};
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"platform.frontend.invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "The Platform Services request is invalid.",
                                                 "Provide valid stable identities and values within the advertised finite limits.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor OperationDenied{Domain,
                                                  ErrorCode{"platform.frontend.operation_denied"},
                                                  ErrorSeverity::Error,
                                                  "Product policy denies this Platform Services operation.",
                                                  "Respect the frozen product service policy.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor NullProvider{Domain,
                                               ErrorCode{"platform.provider.null"},
                                               ErrorSeverity::Error,
                                               "The Null platform provider cannot accept remote service work.",
                                               "Select an available provider or explicitly suppress the optional intent before submission.",
                                               false,
                                               false};
        const ErrorCodeDescriptor InvalidDispatchResult{Domain,
                                                        ErrorCode{"platform.frontend.invalid_dispatch_result"},
                                                        ErrorSeverity::Error,
                                                        "The Platform Services backend returned malformed request identity evidence.",
                                                        "Reject the provider result and inspect its Horo contract implementation.",
                                                        false,
                                                        false};
    }  // namespace FrontendErrors

    /** @copydoc PlatformServicesFrontend::Create */
    Result<PlatformServicesFrontend> PlatformServicesFrontend::Create(std::shared_ptr<IPlatformServicesBackend> backend,
                                                                      PlatformServiceCapabilitySnapshot capabilities,
                                                                      PlatformSessionSnapshot session,
                                                                      PlatformServicesOperationPolicy policy) {
        if (!backend)
            return Failure<PlatformServicesFrontend>(FrontendErrors::InvalidComposition);
        const PlatformServicesBackendConfig validationPolicy;
        if (ValidatePlatformServiceCapabilitySnapshot(capabilities, validationPolicy).HasError() ||
            capabilities.providerGeneration != session.ProviderGeneration())
            return Failure<PlatformServicesFrontend>(FrontendErrors::InvalidComposition);
        auto inspected = backend->InspectCapabilities();
        if (inspected.HasError())
            return Result<PlatformServicesFrontend>::Failure(inspected.ErrorValue());
        if (ValidatePlatformServiceCapabilitySnapshot(inspected.Value(), validationPolicy).HasError() ||
            !SameSnapshot(capabilities, inspected.Value()))
            return Failure<PlatformServicesFrontend>(FrontendErrors::InvalidComposition);
        return Result<PlatformServicesFrontend>::Success(
            PlatformServicesFrontend(std::move(backend), std::move(capabilities), std::move(session), std::move(policy)));
    }

    PlatformServicesFrontend::~PlatformServicesFrontend() {
        try {
            static_cast<void>(Close());
        } catch (const std::bad_alloc &) {
            // Close already stopped admission and invoked shutdown before storing the typed error.
            open_ = false;
            shutdownInvoked_ = true;
        }
    }

    PlatformServicesFrontend::PlatformServicesFrontend(PlatformServicesFrontend &&other) noexcept
        : backend_(std::move(other.backend_)), capabilities_(std::move(other.capabilities_)), session_(std::move(other.session_)),
          policy_(std::move(other.policy_)), open_(std::exchange(other.open_, false)), shutdownInvoked_(other.shutdownInvoked_),
          closeError_(std::move(other.closeError_)) {
        other.shutdownInvoked_ = true;
    }

    /** @copydoc PlatformServicesFrontend::UnlockAchievement */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::UnlockAchievement(AchievementUnlockRequest request) const {
        if (!request.achievement.IsValid())
            return Failure<PlatformRequestHandle<void>>(FrontendErrors::InvalidRequest);
        if (const auto valid = ValidateSubjectService(PlatformServiceKind::Achievements, request.subject); valid.HasError())
            return Result<PlatformRequestHandle<void>>::Failure(valid.ErrorValue());
        return ValidatedDispatch(backend_->UnlockAchievement(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::SubmitScore */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::SubmitScore(LeaderboardScoreRequest request) const {
        if (const auto valid = ValidateLeaderboardOrStat(request.leaderboard.IsValid(), request.subject); valid.HasError())
            return Result<PlatformRequestHandle<void>>::Failure(valid.ErrorValue());
        return ValidatedDispatch(backend_->SubmitScore(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::WriteStat */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::WriteStat(StatWriteRequest request) const {
        if (const auto valid = ValidateLeaderboardOrStat(request.stat.IsValid(), request.subject); valid.HasError())
            return Result<PlatformRequestHandle<void>>::Failure(valid.ErrorValue());
        return ValidatedDispatch(backend_->WriteStat(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::ReadCloudObject */
    Result<PlatformRequestHandle<CloudReadResult>> PlatformServicesFrontend::ReadCloudObject(CloudReadRequest request) const {
        if (!request.object.IsValid())
            return Failure<PlatformRequestHandle<CloudReadResult>>(FrontendErrors::InvalidRequest);
        if (const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject); valid.HasError())
            return Result<PlatformRequestHandle<CloudReadResult>>::Failure(valid.ErrorValue());
        return ValidatedDispatch(backend_->ReadCloudObject(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::WriteCloudObject */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::WriteCloudObject(CloudWriteRequest request) const {
        if (!request.object.IsValid())
            return Failure<PlatformRequestHandle<void>>(FrontendErrors::InvalidRequest);
        const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<void>>::Failure(valid.ErrorValue());
        if (std::cmp_greater(request.bytes.size(), valid.Value()->limits.maxPayloadBytes))
            return Failure<PlatformRequestHandle<void>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->WriteCloudObject(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::SetPresence */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::SetPresence(PresenceUpdateRequest request) const {
        if (!request.status.IsValid())
            return Failure<PlatformRequestHandle<void>>(FrontendErrors::InvalidRequest);
        const auto valid = ValidateSubjectService(PlatformServiceKind::Presence, request.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<void>>::Failure(valid.ErrorValue());
        if (std::cmp_greater(request.detail.size(), valid.Value()->limits.maxPayloadBytes))
            return Failure<PlatformRequestHandle<void>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->SetPresence(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::ClearPresence */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::ClearPresence(PlatformSubjectHandle subject) const {
        if (const auto valid = ValidateSubjectService(PlatformServiceKind::Presence, subject); valid.HasError())
            return Result<PlatformRequestHandle<void>>::Failure(valid.ErrorValue());
        return ValidatedDispatch(backend_->ClearPresence(std::move(subject)));
    }

    /** @copydoc PlatformServicesFrontend::QueryFriends */
    Result<PlatformRequestHandle<FriendsPage>> PlatformServicesFrontend::QueryFriends(FriendsQuery query) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Friends, query.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<FriendsPage>>::Failure(valid.ErrorValue());
        if (query.pageSize == 0 || query.pageSize > valid.Value()->limits.maxPageEntries)
            return Failure<PlatformRequestHandle<FriendsPage>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->QueryFriends(std::move(query)));
    }

    /** @copydoc PlatformServicesFrontend::QueryCurrentSession */
    Result<PlatformRequestHandle<PlatformSessionSnapshot>> PlatformServicesFrontend::QueryCurrentSession() const {
        if (const auto valid = ValidateService(PlatformServiceKind::Session); valid.HasError())
            return Result<PlatformRequestHandle<PlatformSessionSnapshot>>::Failure(valid.ErrorValue());
        return ValidatedDispatch(backend_->QueryCurrentSession());
    }

    /** @copydoc PlatformServicesFrontend::ServiceLimits */
    Result<PlatformServiceLimits> PlatformServicesFrontend::ServiceLimits(const PlatformServiceKind service) const {
        const auto valid = ValidateService(service);
        if (valid.HasError())
            return Result<PlatformServiceLimits>::Failure(valid.ErrorValue());
        return Result<PlatformServiceLimits>::Success(valid.Value()->limits);
    }

    /** @copydoc PlatformServicesFrontend::Close */
    Result<void> PlatformServicesFrontend::Close() {
        open_ = false;
        if (shutdownInvoked_)
            return closeError_ ? Result<void>::Failure(*closeError_) : Result<void>::Success();
        shutdownInvoked_ = true;
        auto result = [&]() {
            try {
                return backend_->Shutdown();
            } catch (...) {  // NOSONAR: backend adapters are an extension boundary and may throw non-standard exceptions.
                return Failure<void>(BackendErrors::ServiceUnavailable);
            }
        }();
        if (result.HasError())
            closeError_ = result.ErrorValue();
        return result;
    }

    /** @copydoc PlatformServicesFrontend::IsOpen */
    bool PlatformServicesFrontend::IsOpen() const noexcept {
        return open_;
    }

    PlatformServicesFrontend::PlatformServicesFrontend(std::shared_ptr<IPlatformServicesBackend> backend,
                                                       PlatformServiceCapabilitySnapshot capabilities, PlatformSessionSnapshot session,
                                                       PlatformServicesOperationPolicy policy) noexcept
        : backend_(std::move(backend)), capabilities_(std::move(capabilities)), session_(std::move(session)), policy_(std::move(policy)) {}

    Result<const PlatformServiceCapability *> PlatformServicesFrontend::ValidateService(const PlatformServiceKind service) const {
        if (!open_)
            return Failure<const PlatformServiceCapability *>(FrontendErrors::Unavailable);
        if (service >= PlatformServiceKind::Count)
            return Failure<const PlatformServiceCapability *>(FrontendErrors::InvalidRequest);
        if (const auto index = static_cast<std::size_t>(service); policy_.deniedServices[index])
            return Failure<const PlatformServiceCapability *>(FrontendErrors::OperationDenied);
        const auto *capability = FindCapability(capabilities_, service);
        if (capability == nullptr || capability->availability != PlatformServiceAvailability::Available) {
            if (capability != nullptr && capability->unavailableReason == PlatformServiceUnavailableReason::NullProviderSelected)
                return Failure<const PlatformServiceCapability *>(FrontendErrors::NullProvider);
            return Failure<const PlatformServiceCapability *>(BackendErrors::ServiceUnavailable);
        }
        return Result<const PlatformServiceCapability *>::Success(capability);
    }

    /** @copydoc PlatformServicesFrontend::ValidateLeaderboardOrStat */
    Result<void> PlatformServicesFrontend::ValidateLeaderboardOrStat(const bool identityValid, const PlatformSubjectHandle &subject) const {
        if (!identityValid)
            return Failure<void>(FrontendErrors::InvalidRequest);
        const auto valid = ValidateSubjectService(PlatformServiceKind::LeaderboardsAndStats, subject);
        return valid.HasError() ? Result<void>::Failure(valid.ErrorValue()) : Result<void>::Success();
    }

    Result<const PlatformServiceCapability *> PlatformServicesFrontend::ValidateSubjectService(const PlatformServiceKind service,
                                                                                               const PlatformSubjectHandle &subject) const {
        const auto capability = ValidateService(service);
        if (capability.HasError())
            return capability;
        if (const auto access = ValidatePlatformSessionAccess(session_, subject, session_.AccessRevision(), service); access.HasError())
            return Result<const PlatformServiceCapability *>::Failure(access.ErrorValue());
        return capability;
    }
}  // namespace Horo::PlatformServices
