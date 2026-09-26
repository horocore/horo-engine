#include "Horo/PlatformServices/PlatformServicesFrontend.h"

#include "PlatformServicesMetrics.h"

#include <algorithm>
#include <limits>
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
                   left.leaderboardQueries == right.leaderboardQueries && left.binding == right.binding &&
                   left.unavailableReason == right.unavailableReason && left.cloudMutation == right.cloudMutation;
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

        template <typename T> [[nodiscard]] Result<PlatformRequestHandle<T>> RejectRequest(Error error) {
            Detail::RecordPlatformRequestMetric(Detail::PlatformRequestMetricOutcome::Rejected);
            return Result<PlatformRequestHandle<T>>::Failure(std::move(error));
        }

        template <typename T> [[nodiscard]] Result<PlatformRequestHandle<T>> RecordDispatch(Result<PlatformRequestHandle<T>> result) {
            auto validated = ValidatedDispatch(std::move(result));
            Detail::RecordPlatformRequestMetric(validated.HasValue() ? Detail::PlatformRequestMetricOutcome::Accepted
                                                                     : Detail::PlatformRequestMetricOutcome::Rejected);
            return validated;
        }

        [[nodiscard]] Detail::PlatformSessionMetricOutcome SessionMetricOutcome(const Error &error) noexcept {
            if (ErrorChainContains(error, PlatformSessionErrors::StaleSession.domain, PlatformSessionErrors::StaleSession.code))
                return Detail::PlatformSessionMetricOutcome::StaleSession;
            if (ErrorChainContains(error, PlatformSessionErrors::StaleAccessPolicy.domain, PlatformSessionErrors::StaleAccessPolicy.code))
                return Detail::PlatformSessionMetricOutcome::StaleAccessPolicy;
            if (ErrorChainContains(error, PlatformSessionErrors::ConsentRequired.domain, PlatformSessionErrors::ConsentRequired.code))
                return Detail::PlatformSessionMetricOutcome::ConsentRequired;
            if (ErrorChainContains(error, PlatformSessionErrors::AccessDenied.domain, PlatformSessionErrors::AccessDenied.code))
                return Detail::PlatformSessionMetricOutcome::AccessDenied;
            if (ErrorChainContains(error, PlatformSessionErrors::AccessRestricted.domain, PlatformSessionErrors::AccessRestricted.code))
                return Detail::PlatformSessionMetricOutcome::AccessRestricted;
            if (ErrorChainContains(error, PlatformSessionErrors::AccessRevoked.domain, PlatformSessionErrors::AccessRevoked.code))
                return Detail::PlatformSessionMetricOutcome::AccessRevoked;
            if (ErrorChainContains(error, PlatformSessionErrors::NoSubject.domain, PlatformSessionErrors::NoSubject.code) ||
                ErrorChainContains(error, PlatformSessionErrors::Authenticating.domain, PlatformSessionErrors::Authenticating.code) ||
                ErrorChainContains(error, PlatformSessionErrors::Closing.domain, PlatformSessionErrors::Closing.code) ||
                ErrorChainContains(error, PlatformSessionErrors::Failed.domain, PlatformSessionErrors::Failed.code))
                return Detail::PlatformSessionMetricOutcome::Inactive;
            return Detail::PlatformSessionMetricOutcome::AccessUnavailable;
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
        Detail::RegisterPlatformServicesMetricDescriptors();
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
            return RejectRequest<void>(MakeError(FrontendErrors::InvalidRequest));
        if (const auto valid = ValidateSubjectService(PlatformServiceKind::Achievements, request.subject); valid.HasError())
            return RejectRequest<void>(valid.ErrorValue());
        return RecordDispatch(backend_->UnlockAchievement(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::SubmitScore */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::SubmitScore(LeaderboardScoreRequest request) const {
        if (const auto valid = ValidateLeaderboardOrStat(request.leaderboard.IsValid(), request.subject); valid.HasError())
            return RejectRequest<void>(valid.ErrorValue());
        return RecordDispatch(backend_->SubmitScore(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::QueryRankedLeaderboard */
    Result<PlatformRequestHandle<LeaderboardEntriesPage>> PlatformServicesFrontend::QueryRankedLeaderboard(
        LeaderboardRankedQuery query) const {
        if (!query.leaderboard.IsValid())
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(FrontendErrors::InvalidRequest);
        const auto valid = ValidateSubjectService(PlatformServiceKind::LeaderboardsAndStats, query.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<LeaderboardEntriesPage>>::Failure(valid.ErrorValue());
        if (!valid.Value()->leaderboardQueries.Supports(LeaderboardQueryKind::Ranked))
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(BackendErrors::UnsupportedOperation);
        if (query.pageSize == 0 || query.pageSize > valid.Value()->limits.maxPageEntries ||
            query.startIndex > std::numeric_limits<std::uint32_t>::max() - query.pageSize)
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->QueryRankedLeaderboard(std::move(query)));
    }

    /** @copydoc PlatformServicesFrontend::QueryLeaderboardAroundSubject */
    Result<PlatformRequestHandle<LeaderboardAroundSubjectResult>> PlatformServicesFrontend::QueryLeaderboardAroundSubject(
        LeaderboardAroundSubjectQuery query) const {
        if (!query.leaderboard.IsValid())
            return Failure<PlatformRequestHandle<LeaderboardAroundSubjectResult>>(FrontendErrors::InvalidRequest);
        const auto valid = ValidateSubjectService(PlatformServiceKind::LeaderboardsAndStats, query.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<LeaderboardAroundSubjectResult>>::Failure(valid.ErrorValue());
        if (!valid.Value()->leaderboardQueries.Supports(LeaderboardQueryKind::AroundSubject))
            return Failure<PlatformRequestHandle<LeaderboardAroundSubjectResult>>(BackendErrors::UnsupportedOperation);
        if (const auto entryLimit = static_cast<std::uint64_t>(query.entriesBefore) + query.entriesAfter + 1U;
            entryLimit > valid.Value()->limits.maxPageEntries)
            return Failure<PlatformRequestHandle<LeaderboardAroundSubjectResult>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->QueryLeaderboardAroundSubject(std::move(query)));
    }

    /** @copydoc PlatformServicesFrontend::QueryFriendsLeaderboard */
    Result<PlatformRequestHandle<LeaderboardEntriesPage>> PlatformServicesFrontend::QueryFriendsLeaderboard(
        LeaderboardFriendsQuery query) const {
        if (!query.leaderboard.IsValid())
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(FrontendErrors::InvalidRequest);
        const auto valid = ValidateSubjectService(PlatformServiceKind::LeaderboardsAndStats, query.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<LeaderboardEntriesPage>>::Failure(valid.ErrorValue());
        if (policy_.deniedServices[static_cast<std::size_t>(PlatformServiceKind::Friends)])
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(FrontendErrors::OperationDenied);
        if (const auto socialAccess =
                ValidatePlatformSessionAccess(session_, query.subject, session_.AccessRevision(), PlatformServiceKind::Friends);
            socialAccess.HasError())
            return Result<PlatformRequestHandle<LeaderboardEntriesPage>>::Failure(socialAccess.ErrorValue());
        if (!valid.Value()->leaderboardQueries.Supports(LeaderboardQueryKind::Friends))
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(BackendErrors::UnsupportedOperation);
        if (query.pageSize == 0 || query.pageSize > valid.Value()->limits.maxPageEntries ||
            query.startIndex > std::numeric_limits<std::uint32_t>::max() - query.pageSize)
            return Failure<PlatformRequestHandle<LeaderboardEntriesPage>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->QueryFriendsLeaderboard(std::move(query)));
    }

    /** @copydoc PlatformServicesFrontend::WriteStat */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::WriteStat(StatWriteRequest request) const {
        if (const auto valid = ValidateLeaderboardOrStat(request.stat.IsValid(), request.subject); valid.HasError())
            return RejectRequest<void>(valid.ErrorValue());
        return RecordDispatch(backend_->WriteStat(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::ReadCloudObject */
    Result<PlatformRequestHandle<CloudReadResult>> PlatformServicesFrontend::ReadCloudObject(CloudReadRequest request) const {
        if (!request.object.IsValid())
            return RejectRequest<CloudReadResult>(MakeError(FrontendErrors::InvalidRequest));
        if (const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject); valid.HasError())
            return RejectRequest<CloudReadResult>(valid.ErrorValue());
        return RecordDispatch(backend_->ReadCloudObject(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::ListCloudObjects */
    Result<PlatformRequestHandle<CloudObjectPage>> PlatformServicesFrontend::ListCloudObjects(CloudListRequest request) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<CloudObjectPage>>::Failure(valid.ErrorValue());
        if (const CloudObjectContractLimits limits{.maxPageEntries = valid.Value()->limits.maxPageEntries,
                                                   .maxObjectBytes = valid.Value()->limits.maxPayloadBytes};
            ValidateCloudListRequest(request, limits).HasError())
            return Failure<PlatformRequestHandle<CloudObjectPage>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->ListCloudObjects(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::ReadCloudObject */
    Result<PlatformRequestHandle<CloudBlobReadResult>> PlatformServicesFrontend::ReadCloudObject(CloudBlobReadRequest request) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<CloudBlobReadResult>>::Failure(valid.ErrorValue());
        if (const CloudObjectContractLimits limits{.maxObjectBytes = valid.Value()->limits.maxPayloadBytes};
            ValidateCloudBlobReadRequest(request, limits).HasError())
            return Failure<PlatformRequestHandle<CloudBlobReadResult>>(FrontendErrors::InvalidRequest);
        return ValidatedDispatch(backend_->ReadCloudObject(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::WriteCloudObject */
    Result<PlatformRequestHandle<CloudMutationResult>> PlatformServicesFrontend::WriteCloudObject(CloudBlobWriteRequest request) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<CloudMutationResult>>::Failure(valid.ErrorValue());
        if (!valid.Value()->cloudMutation)
            return Failure<PlatformRequestHandle<CloudMutationResult>>(CloudObjectErrors::UnsupportedCapability);
        const CloudObjectContractLimits limits{.maxPageEntries = valid.Value()->limits.maxPageEntries,
                                               .maxObjectBytes = valid.Value()->limits.maxPayloadBytes};
        if (const auto requestValid = ValidateCloudBlobWriteRequest(request, *valid.Value()->cloudMutation, limits);
            requestValid.HasError())
            return Result<PlatformRequestHandle<CloudMutationResult>>::Failure(requestValid.ErrorValue());
        return ValidatedDispatch(backend_->WriteCloudObject(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::DeleteCloudObject */
    Result<PlatformRequestHandle<CloudMutationResult>> PlatformServicesFrontend::DeleteCloudObject(CloudBlobDeleteRequest request) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, request.subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<CloudMutationResult>>::Failure(valid.ErrorValue());
        if (!valid.Value()->cloudMutation)
            return Failure<PlatformRequestHandle<CloudMutationResult>>(CloudObjectErrors::UnsupportedCapability);
        const CloudObjectContractLimits limits{.maxPageEntries = valid.Value()->limits.maxPageEntries,
                                               .maxObjectBytes = valid.Value()->limits.maxPayloadBytes};
        if (const auto requestValid = ValidateCloudBlobDeleteRequest(request, *valid.Value()->cloudMutation, limits);
            requestValid.HasError())
            return Result<PlatformRequestHandle<CloudMutationResult>>::Failure(requestValid.ErrorValue());
        return ValidatedDispatch(backend_->DeleteCloudObject(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::QueryCloudQuota */
    Result<PlatformRequestHandle<CloudQuotaObservation>> PlatformServicesFrontend::QueryCloudQuota(PlatformSubjectHandle subject) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Cloud, subject);
        if (valid.HasError())
            return Result<PlatformRequestHandle<CloudQuotaObservation>>::Failure(valid.ErrorValue());
        if (!valid.Value()->cloudMutation)
            return Failure<PlatformRequestHandle<CloudQuotaObservation>>(CloudObjectErrors::UnsupportedCapability);
        return ValidatedDispatch(backend_->QueryCloudQuota(std::move(subject)));
    }

    /** @copydoc PlatformServicesFrontend::SetPresence */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::SetPresence(PresenceUpdateRequest request) const {
        if (!request.status.IsValid())
            return RejectRequest<void>(MakeError(FrontendErrors::InvalidRequest));
        const auto valid = ValidateSubjectService(PlatformServiceKind::Presence, request.subject);
        if (valid.HasError())
            return RejectRequest<void>(valid.ErrorValue());
        if (std::cmp_greater(request.detail.size(), valid.Value()->limits.maxPayloadBytes))
            return RejectRequest<void>(MakeError(FrontendErrors::InvalidRequest));
        return RecordDispatch(backend_->SetPresence(std::move(request)));
    }

    /** @copydoc PlatformServicesFrontend::ClearPresence */
    Result<PlatformRequestHandle<void>> PlatformServicesFrontend::ClearPresence(PlatformSubjectHandle subject) const {
        if (const auto valid = ValidateSubjectService(PlatformServiceKind::Presence, subject); valid.HasError())
            return RejectRequest<void>(valid.ErrorValue());
        return RecordDispatch(backend_->ClearPresence(std::move(subject)));
    }

    /** @copydoc PlatformServicesFrontend::QueryFriends */
    Result<PlatformRequestHandle<FriendsPage>> PlatformServicesFrontend::QueryFriends(FriendsQuery query) const {
        const auto valid = ValidateSubjectService(PlatformServiceKind::Friends, query.subject);
        if (valid.HasError())
            return RejectRequest<FriendsPage>(valid.ErrorValue());
        if (query.pageSize == 0 || query.pageSize > valid.Value()->limits.maxPageEntries)
            return RejectRequest<FriendsPage>(MakeError(FrontendErrors::InvalidRequest));
        return RecordDispatch(backend_->QueryFriends(std::move(query)));
    }

    /** @copydoc PlatformServicesFrontend::QueryCurrentSession */
    Result<PlatformRequestHandle<PlatformSessionSnapshot>> PlatformServicesFrontend::QueryCurrentSession() const {
        if (const auto valid = ValidateService(PlatformServiceKind::Session); valid.HasError())
            return RejectRequest<PlatformSessionSnapshot>(valid.ErrorValue());
        return RecordDispatch(backend_->QueryCurrentSession());
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
        Detail::RecordPlatformShutdownMetric(result.HasValue() ? Detail::PlatformShutdownMetricOutcome::Succeeded
                                                               : Detail::PlatformShutdownMetricOutcome::Failed);
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
        if (!open_) {
            Detail::RecordPlatformCapabilityMetric(service, Detail::PlatformCapabilityMetricOutcome::FrontendClosed);
            return Failure<const PlatformServiceCapability *>(FrontendErrors::Unavailable);
        }
        if (service >= PlatformServiceKind::Count) {
            Detail::RecordPlatformCapabilityMetric(service, Detail::PlatformCapabilityMetricOutcome::InvalidService);
            return Failure<const PlatformServiceCapability *>(FrontendErrors::InvalidRequest);
        }
        if (const auto index = static_cast<std::size_t>(service); policy_.deniedServices[index]) {
            Detail::RecordPlatformCapabilityMetric(service, Detail::PlatformCapabilityMetricOutcome::PolicyDenied);
            return Failure<const PlatformServiceCapability *>(FrontendErrors::OperationDenied);
        }
        const auto *capability = FindCapability(capabilities_, service);
        if (capability == nullptr || capability->availability != PlatformServiceAvailability::Available) {
            if (capability != nullptr && capability->unavailableReason == PlatformServiceUnavailableReason::NullProviderSelected) {
                Detail::RecordPlatformCapabilityMetric(service, Detail::PlatformCapabilityMetricOutcome::NullProvider);
                return Failure<const PlatformServiceCapability *>(FrontendErrors::NullProvider);
            }
            Detail::RecordPlatformCapabilityMetric(service, Detail::PlatformCapabilityMetricOutcome::Unavailable);
            return Failure<const PlatformServiceCapability *>(BackendErrors::ServiceUnavailable);
        }
        Detail::RecordPlatformCapabilityMetric(service, Detail::PlatformCapabilityMetricOutcome::Available);
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
        if (const auto access = ValidatePlatformSessionAccess(session_, subject, session_.AccessRevision(), service); access.HasError()) {
            Detail::RecordPlatformSessionMetric(service, SessionMetricOutcome(access.ErrorValue()));
            return Result<const PlatformServiceCapability *>::Failure(access.ErrorValue());
        }
        Detail::RecordPlatformSessionMetric(service, Detail::PlatformSessionMetricOutcome::Allowed);
        return capability;
    }
}  // namespace Horo::PlatformServices
