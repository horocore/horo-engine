#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "PlatformProviderCandidateState.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    namespace PlatformProviderLifecycleErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.lifecycle"};
        }

        const ErrorCodeDescriptor InvalidSelection{Domain,
                                                   ErrorCode{"platform.lifecycle.invalid_selection"},
                                                   ErrorSeverity::Error,
                                                   "The exact configured platform provider is unavailable.",
                                                   "Install and admit the selected provider generation.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor UnsupportedProfile{Domain,
                                                     ErrorCode{"platform.lifecycle.unsupported_profile"},
                                                     ErrorSeverity::Error,
                                                     "The selected provider has no versioned operation profile.",
                                                     "Use a provider implementing the version-2 operation profile.",
                                                     false,
                                                     false};
        const ErrorCodeDescriptor InitializationFailed{Domain,
                                                       ErrorCode{"platform.lifecycle.initialization_failed"},
                                                       ErrorSeverity::Error,
                                                       "Platform provider initialization failed.",
                                                       "Inspect the selected provider and its service/session/ingress stage.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidOperation{Domain,
                                                   ErrorCode{"platform.lifecycle.invalid_operation"},
                                                   ErrorSeverity::Error,
                                                   "Platform provider operation is invalid or unavailable.",
                                                   "Use an advertised service and a bounded Horo operation.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor DrainBusy{Domain,
                                            ErrorCode{"platform.lifecycle.drain_busy"},
                                            ErrorSeverity::Error,
                                            "Platform provider work or callbacks are still draining.",
                                            "Retry shutdown on the provider owner lane; keep the code lease alive.",
                                            false,
                                            true};
        const ErrorCodeDescriptor ShutdownFailed{Domain,
                                                 ErrorCode{"platform.lifecycle.shutdown_failed"},
                                                 ErrorSeverity::Error,
                                                 "Platform provider shutdown failed.",
                                                 "Retain the provider generation and inspect its shutdown stage.",
                                                 false,
                                                 false};
    }  // namespace PlatformProviderLifecycleErrors

    struct PlatformProviderLifecycleState final {
        static constexpr std::size_t MaximumRequests = 64;
        static constexpr std::size_t MaximumPayloadBytes = 4096;

        struct Completion final {
            std::uint64_t requestId{};
            std::uint64_t requestGeneration{};
            std::uint64_t sessionRevision{};
            std::uint32_t service{};
            std::uint32_t operation{};
            std::uint32_t resultCode{};
            std::uint32_t size{};
            std::array<std::byte, MaximumPayloadBytes> payload{};
        };

        struct InFlight final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
            std::uint64_t sessionRevision{};
            PlatformServiceKind service;
            std::uint32_t operation{};
            PlatformProviderRequestLease lease;

            InFlight(PlatformRequestId requestId, PlatformRequestGeneration requestGeneration, std::uint64_t revision,
                     PlatformServiceKind requestService, std::uint32_t requestOperation, PlatformProviderRequestLease requestLease)
                : id(requestId), generation(requestGeneration), sessionRevision(revision), service(requestService),
                  operation(requestOperation), lease(std::move(requestLease)) {}
        };

        explicit PlatformProviderLifecycleState(const PlatformRequestGeneration generation)
            : requests({.activeCapacity = MaximumRequests,
                        .terminalCapacity = MaximumRequests,
                        .observerCapacity = MaximumRequests,
                        .generation = generation}) {
            inFlight.reserve(MaximumRequests);
        }

        struct LifecycleStages final {
            bool servicesAttempted{};
            bool sessionAttempted{};
            bool ingressAttempted{};
            bool admissionClosed{};
            bool closing{};
            bool ingressClosed{};
            bool drained{};
            bool sessionStopped{};
            bool servicesStopped{};
            bool closed{};
        };

        std::mutex mutex;  // Protects callback ingress and session evidence; native calls run without this lock.
        std::array<Completion, MaximumRequests> completions{};
        std::size_t completionHead{};
        std::size_t completionCount{};
        PlatformProviderSessionObservation session{};
        bool callbackOpen{true};
        HoroPlatformProviderSink sink{};
        HoroPlatformProviderOperations operations{};
        void *candidate{};  // NOSONAR(cpp:S5008) The versioned provider C ABI defines candidates as opaque void pointers.
        std::optional<PlatformProviderCandidateLease> lease;
        PlatformRequestStore requests;
        std::vector<InFlight> inFlight;
        std::uint32_t availableServices{};
        LifecycleStages stages;
        std::shared_ptr<PlatformProviderLifecycleState> quarantine;  // BUSY teardown retains the callback context and code.
    };

    namespace {
        [[nodiscard]] HoroExtensionStatus ObserveSession(
            void *context, const std::uint64_t revision,  // NOSONAR(cpp:S5008) The callback context is opaque by C ABI contract.
            const std::uint32_t phase) noexcept {
            auto &state = *static_cast<PlatformProviderLifecycleState *>(context);
            std::scoped_lock lock{state.mutex};
            if (!state.callbackOpen || revision == 0 || revision <= state.session.revision || phase > 4)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            state.session = {.revision = revision, .phase = phase};
            return HORO_EXTENSION_SUCCESS;
        }

        [[nodiscard]] HoroExtensionStatus ReceiveCompletion(
            void *context,  // NOSONAR(cpp:S5008) The callback context is opaque by C ABI contract.
            const HoroPlatformProviderCompletion *completion) noexcept {
            if (completion == nullptr || completion->structSize != sizeof(HoroPlatformProviderCompletion) || completion->requestId == 0 ||
                completion->requestGeneration == 0 || completion->sessionRevision == 0 ||
                completion->service >= static_cast<std::uint32_t>(PlatformServiceKind::Count) ||
                completion->payloadSize > PlatformProviderLifecycleState::MaximumPayloadBytes ||
                (completion->payloadSize != 0 && completion->payload == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto &state = *static_cast<PlatformProviderLifecycleState *>(context);
            std::scoped_lock lock{state.mutex};
            if (!state.callbackOpen || state.completionCount == state.completions.size())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            auto &slot = state.completions[(state.completionHead + state.completionCount) % state.completions.size()];
            slot.requestId = completion->requestId;
            slot.requestGeneration = completion->requestGeneration;
            slot.sessionRevision = completion->sessionRevision;
            slot.service = completion->service;
            slot.operation = completion->operation;
            slot.resultCode = completion->resultCode;
            slot.size = completion->resultCode == HORO_PLATFORM_PROVIDER_SUCCESS ? completion->payloadSize : 0;
            if (slot.size != 0)
                std::ranges::copy(std::as_bytes(std::span{completion->payload, static_cast<std::size_t>(slot.size)}), slot.payload.begin());
            ++state.completionCount;
            return HORO_EXTENSION_SUCCESS;
        }

        template <typename Callback, typename... Arguments>
        [[nodiscard]] HoroExtensionStatus InvokeProvider(Callback callback, Arguments... arguments) noexcept {
            try {
                return callback(arguments...);
            } catch (...) {  // NOSONAR: no C++ exception may cross a provider ABI boundary.
                return HORO_EXTENSION_ERROR_INIT_FAILED;
            }
        }

        [[nodiscard]] PlatformProviderFailureCategory ProviderCategory(const std::uint32_t code) noexcept {
            using enum PlatformProviderFailureCategory;
            switch (code) {
                case HORO_PLATFORM_PROVIDER_OFFLINE:
                    return Offline;
                case HORO_PLATFORM_PROVIDER_NOT_SIGNED_IN:
                    return NotSignedIn;
                case HORO_PLATFORM_PROVIDER_FORBIDDEN:
                    return Forbidden;
                case HORO_PLATFORM_PROVIDER_RATE_LIMITED:
                    return RateLimited;
                case HORO_PLATFORM_PROVIDER_PRECONDITION_FAILED:
                    return PreconditionFailed;
                case HORO_PLATFORM_PROVIDER_QUOTA_EXCEEDED:
                    return QuotaExceeded;
                case HORO_PLATFORM_PROVIDER_INVALID_RESPONSE:
                    return InvalidResponse;
                case HORO_PLATFORM_PROVIDER_TRANSIENT_FAILURE:
                    return TransientFailure;
                case HORO_PLATFORM_PROVIDER_PERMANENT_FAILURE:
                    return PermanentFailure;
                default:
                    return Unknown;
            }
        }

        [[nodiscard]] std::uint32_t RequiredMask(const PlatformProjectConfiguration &configuration) noexcept {
            std::uint32_t mask{};
            const auto services = configuration.ServiceRequirements();
            for (std::size_t index = 0; index < services.size(); ++index)
                if (services[index] == PlatformServiceRequirement::Required)
                    mask |= 1U << index;
            return mask;
        }

        [[nodiscard]] std::uint32_t EnabledMask(const PlatformProjectConfiguration &configuration) noexcept {
            std::uint32_t mask{};
            const auto services = configuration.ServiceRequirements();
            for (std::size_t index = 0; index < services.size(); ++index)
                if (services[index] != PlatformServiceRequirement::Disabled)
                    mask |= 1U << index;
            return mask;
        }

        [[nodiscard]] std::uint32_t ClaimedMask(const PlatformProviderContributionDescriptor &descriptor) noexcept {
            std::uint32_t mask{};
            for (std::size_t index = 0; index < descriptor.services.size(); ++index)
                if (descriptor.services[index])
                    mask |= 1U << index;
            return mask;
        }

        /** @brief Opens the selected provider in services, session, then ingress order. */
        [[nodiscard]] bool StartNativeLifecycle(PlatformProviderLifecycleState &state,
                                                const PlatformProviderContributionDescriptor &descriptor,
                                                const PlatformProjectConfiguration &configuration,
                                                const std::uint32_t requiredMask) noexcept {
            state.stages.servicesAttempted = true;
            std::uint32_t availableMask{};
            if (InvokeProvider(state.operations.initializeServices, state.candidate, requiredMask, &availableMask) !=
                    HORO_EXTENSION_SUCCESS ||
                (availableMask & ~ClaimedMask(descriptor)) != 0 || (requiredMask & ~availableMask) != 0)
                return false;
            state.availableServices = availableMask & EnabledMask(configuration);
            state.stages.sessionAttempted = true;
            if (InvokeProvider(state.operations.beginSession, state.candidate, &state.sink) != HORO_EXTENSION_SUCCESS)
                return false;
            state.stages.ingressAttempted = true;
            return InvokeProvider(state.operations.openIngress, state.candidate, &state.sink) == HORO_EXTENSION_SUCCESS;
        }

        /** @brief Closes admission once and retains failed native teardown for an owner-thread retry. */
        [[nodiscard]] Result<void> CloseNativeAdmission(const std::shared_ptr<PlatformProviderLifecycleState> &state) {
            if (state->stages.admissionClosed)
                return Result<void>::Success();
            if (state->stages.servicesAttempted &&
                InvokeProvider(state->operations.closeAdmission, state->candidate) != HORO_EXTENSION_SUCCESS) {
                state->quarantine = state;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state->stages.admissionClosed = true;
            return Result<void>::Success();
        }

        /** @brief Sends cancellation to every admitted in-flight request before provider drain. */
        void CancelNativeInflight(PlatformProviderLifecycleState &state) {
            for (const auto &request : state.inFlight) {
                static_cast<void>(state.requests.RequestCancel(request.id, request.generation));
                static_cast<void>(InvokeProvider(state.operations.cancel, state.candidate, request.id.value, request.generation.value));
            }
        }

        /** @brief Stops a begun provider session exactly once after requests have drained. */
        [[nodiscard]] Result<void> StopNativeSession(const std::shared_ptr<PlatformProviderLifecycleState> &state) {
            if (state->stages.sessionStopped || !state->stages.sessionAttempted)
                return Result<void>::Success();
            if (InvokeProvider(state->operations.stopSession, state->candidate) != HORO_EXTENSION_SUCCESS) {
                state->quarantine = state;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state->stages.sessionStopped = true;
            return Result<void>::Success();
        }
    }  // namespace

    PlatformProviderLifecycleHost::PlatformProviderLifecycleHost(std::shared_ptr<PlatformProviderLifecycleState> state) noexcept
        : state_(std::move(state)) {}

    PlatformProviderLifecycleHost::~PlatformProviderLifecycleHost() {
        try {
            static_cast<void>(Close());
        } catch (...) {  // NOSONAR: retain callback context and native code if typed teardown cannot be represented.
            if (state_)
                state_->quarantine = state_;
        }
    }

    /** @copydoc PlatformProviderLifecycleHost::Start */
    Result<std::unique_ptr<PlatformProviderLifecycleHost>> PlatformProviderLifecycleHost::Start(
        const PlatformProviderLifecycleStartContext &context) {
        using HostResult = Result<std::unique_ptr<PlatformProviderLifecycleHost>>;
        const auto &configuration = context.configuration;
        const auto &admission = context.admission;
        const auto &identity = context.identity;
        const auto &authority = context.authority;
        const auto &versions = context.versions;
        if (configuration.Profile() != admission.profile_ || configuration.UsesNullProvider() || !configuration.SelectedProvider() ||
            !configuration.SelectedModule() || identity.moduleId != configuration.SelectedModule()->value ||
            identity.providerId != configuration.SelectedProviderKey())
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidSelection));
        auto created = admission.CreateExact(identity, authority, versions, context.consumerExtensionId, context.consumerModuleId,
                                             context.consumerGeneration);
        if (created.HasError())
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidSelection));
        auto candidate = std::move(created).Value();
        const auto &descriptor = candidate.Descriptor();
        const auto requiredMask = RequiredMask(configuration);
        if (descriptor.provider != *configuration.SelectedProvider() || descriptor.owner != identity ||
            (static_cast<std::uint32_t>(descriptor.profiles) & (1U << static_cast<std::uint32_t>(configuration.Profile()))) == 0 ||
            (requiredMask & ~ClaimedMask(descriptor)) != 0)
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidSelection));
        auto &native = *candidate.state_;
        if (native.operations.version != HORO_PLATFORM_SERVICES_PROVIDER_OPERATIONS_VERSION ||
            native.operations.structSize != sizeof(HoroPlatformProviderOperations))
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::UnsupportedProfile));
        auto state = std::make_shared<PlatformProviderLifecycleState>(PlatformRequestGeneration{identity.generation});
        state->operations = native.operations;
        state->candidate = native.candidate;
        state->lease.emplace(std::move(candidate));
        state->sink = {.structSize = sizeof(HoroPlatformProviderSink),
                       .context = state.get(),
                       .sessionChanged = ObserveSession,
                       .complete = ReceiveCompletion};
        // The private constructor is inaccessible to make_unique.
        auto host = std::unique_ptr<PlatformProviderLifecycleHost>(  // NOSONAR(cpp:S5950)
            new PlatformProviderLifecycleHost(state));
        if (!StartNativeLifecycle(*state, descriptor, configuration, requiredMask)) {
            static_cast<void>(host->Close());
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InitializationFailed));
        }
        return HostResult::Success(std::move(host));
    }

    /** @copydoc PlatformProviderLifecycleHost::UnlockAchievement */
    Result<PlatformProviderLifecycleHost::RequestHandle> PlatformProviderLifecycleHost::UnlockAchievement(const AchievementId achievement) {
        if (!achievement.IsValid())
            return Result<RequestHandle>::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidOperation));
        std::array<std::byte, sizeof(std::uint64_t)> payload{};
        for (std::size_t index = 0; index < payload.size(); ++index)
            payload[index] = std::byte{static_cast<std::uint8_t>(achievement.value >> (8U * index))};
        return Submit(PlatformServiceKind::Achievements, HORO_PLATFORM_OPERATION_ACHIEVEMENT_UNLOCK, payload);
    }

    /** @brief Sends one validated Horo operation through the selected native candidate. */
    Result<PlatformProviderLifecycleHost::RequestHandle> PlatformProviderLifecycleHost::Submit(  // NOSONAR(cpp:S5817) Mutates shared
                                                                                                 // request state.
        const PlatformServiceKind service, const std::uint32_t operation, const std::span<const std::byte> payload) {
        using SubmitResult = Result<RequestHandle>;
        auto &state = *state_;
        const auto serviceIndex = static_cast<std::uint32_t>(service);
        if (state.stages.closing || state.stages.closed || !state.lease)
            return SubmitResult::Failure(MakeError(FrontendErrors::Unavailable));
        std::uint64_t sessionRevision{};
        {
            std::scoped_lock lock{state.mutex};
            if (state.session.phase != static_cast<std::uint32_t>(PlatformSessionPhase::Active))
                return SubmitResult::Failure(MakeError(PlatformSessionErrors::NoSubject));
            sessionRevision = state.session.revision;
        }
        if (service != PlatformServiceKind::Achievements || operation != HORO_PLATFORM_OPERATION_ACHIEVEMENT_UNLOCK ||
            payload.size() != sizeof(std::uint64_t) || (state.availableServices & (1U << serviceIndex)) == 0)
            return SubmitResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidOperation));
        if (state.inFlight.size() == PlatformProviderLifecycleState::MaximumRequests)
            return SubmitResult::Failure(MakeError(RequestErrors::CapacityExceeded));
        auto nativeLease = state.lease->AcquireRequestLease();
        if (nativeLease.HasError())
            return SubmitResult::Failure(nativeLease.ErrorValue());
        auto admitted = state.requests.Admit<void>();
        if (admitted.HasError())
            return SubmitResult::Failure(admitted.ErrorValue());
        auto handle = std::move(admitted).Value();
        state.inFlight.emplace_back(handle.Id(), handle.Generation(), sessionRevision, service, operation, std::move(nativeLease).Value());
        static_cast<void>(state.requests.MarkRunning(handle));
        const HoroPlatformProviderOperation input{.structSize = sizeof(HoroPlatformProviderOperation),
                                                  .requestId = handle.Id().value,
                                                  .requestGeneration = handle.Generation().value,
                                                  .sessionRevision = sessionRevision,
                                                  .service = serviceIndex,
                                                  .operation = operation,
                                                  // The public C ABI requires a uint8_t byte span.
                                                  .payload = reinterpret_cast<const std::uint8_t *>(  // NOSONAR(cpp:S6022)
                                                      payload.data()),
                                                  .payloadSize = static_cast<std::uint32_t>(payload.size())};
        if (const HoroExtensionStatus submission = InvokeProvider(state.operations.submit, state.candidate, &input);
            submission != HORO_EXTENSION_SUCCESS) {
            if (submission == HORO_EXTENSION_ERROR_CANCELLED) {
                static_cast<void>(state.requests.RequestCancel(handle));
                static_cast<void>(state.requests.CompleteCancelled(handle, MakeError(RequestErrors::Cancelled)));
            } else {
                static_cast<void>(
                    state.requests.CompleteFailure(handle, MakePlatformProviderError(PlatformProviderFailureCategory::Unknown,
                                                                                     handle.Id().value, handle.Generation().value)));
            }
            // An adapter may have started native work before reporting failure; its lease retires on callback or drain.
        }
        return SubmitResult::Success(std::move(handle));
    }

    /** @copydoc PlatformProviderLifecycleHost::DispatchCompletions */
    std::size_t PlatformProviderLifecycleHost::DispatchCompletions(  // NOSONAR(cpp:S5817) Mutates shared lifecycle state.
        const std::size_t maximum) {
        auto &state = *state_;
        std::size_t processed{};
        while (processed < maximum) {
            PlatformProviderLifecycleState::Completion completion;
            {
                std::scoped_lock lock{state.mutex};
                if (state.completionCount == 0)
                    break;
                completion = state.completions[state.completionHead];
                state.completionHead = (state.completionHead + 1) % state.completions.size();
                --state.completionCount;
            }
            if (const auto found = std::ranges::find_if(state.inFlight,
                                                        [&](const auto &entry) {
                return entry.id.value == completion.requestId && entry.generation.value == completion.requestGeneration &&
                       static_cast<std::uint32_t>(entry.service) == completion.service && entry.operation == completion.operation;
            });
                found != state.inFlight.end()) {
                RequestHandle handle{found->id, found->generation};
                std::uint64_t currentSessionRevision{};
                {
                    std::scoped_lock lock{state.mutex};
                    currentSessionRevision = state.session.revision;
                }
                if (completion.sessionRevision != found->sessionRevision || currentSessionRevision != found->sessionRevision)
                    static_cast<void>(state.requests.CompleteFailure(handle, MakeError(PlatformSessionErrors::StaleSession)));
                else if (completion.resultCode == HORO_PLATFORM_PROVIDER_SUCCESS)
                    static_cast<void>(state.requests.CompleteSuccess(handle));
                else if (completion.resultCode == HORO_PLATFORM_PROVIDER_CANCELLED) {
                    static_cast<void>(state.requests.RequestCancel(handle));
                    static_cast<void>(state.requests.CompleteCancelled(handle, MakeError(RequestErrors::Cancelled)));
                } else if (completion.resultCode == HORO_PLATFORM_PROVIDER_TIMED_OUT)
                    static_cast<void>(state.requests.CompleteTimedOut(handle, MakeError(RequestErrors::TimedOut)));
                else if (completion.resultCode == HORO_PLATFORM_PROVIDER_CAPABILITY_UNAVAILABLE)
                    static_cast<void>(state.requests.CompleteFailure(handle, MakeError(BackendErrors::ServiceUnavailable)));
                else
                    static_cast<void>(
                        state.requests.CompleteFailure(handle, MakePlatformProviderError(ProviderCategory(completion.resultCode),
                                                                                         handle.Id().value, handle.Generation().value)));
                state.inFlight.erase(found);
            }
            ++processed;
        }
        static_cast<void>(state.requests.DispatchCompletions(maximum));
        return processed;
    }

    /** @copydoc PlatformProviderLifecycleHost::Query */
    Result<PlatformRequestSnapshot<void>> PlatformProviderLifecycleHost::Query(const RequestHandle &request) const {
        return state_->requests.Query(request);
    }

    /** @copydoc PlatformProviderLifecycleHost::OnComplete */
    Result<PlatformRequestSubscription> PlatformProviderLifecycleHost::OnComplete(  // NOSONAR(cpp:S5817) Mutates owned request store.
        const RequestHandle &request, std::function<void(const PlatformRequestSnapshot<void> &)> observer) {
        return state_->requests.OnComplete(request, std::move(observer));
    }

    /** @copydoc PlatformProviderLifecycleHost::Session */
    PlatformProviderSessionObservation PlatformProviderLifecycleHost::Session() const noexcept {
        std::scoped_lock lock{state_->mutex};
        return state_->session;
    }

    /** @copydoc PlatformProviderLifecycleHost::Close */
    Result<void> PlatformProviderLifecycleHost::Close() {  // NOSONAR(cpp:S5817) Close is a lifecycle transition on the shared host state.
        auto &state = *state_;
        if (state.stages.closed)
            return Result<void>::Success();
        state.stages.closing = true;
        if (auto closed = CloseNativeAdmission(state_); closed.HasError())
            return closed;
        if (!state.stages.ingressClosed) {
            {
                std::scoped_lock lock{state.mutex};
                state.callbackOpen = false;
                state.completionCount = 0;
            }
            if (state.stages.ingressAttempted && InvokeProvider(state.operations.closeIngress, state.candidate) != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.stages.ingressClosed = true;
        }
        CancelNativeInflight(state);
        if (!state.stages.drained && (state.stages.sessionAttempted || state.stages.ingressAttempted)) {
            if (const auto status = InvokeProvider(state.operations.drain, state.candidate); status != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(status == HORO_EXTENSION_ERROR_BUSY
                                                           ? PlatformProviderLifecycleErrors::DrainBusy
                                                           : PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.stages.drained = true;
        }
        state.inFlight.clear();
        state.requests.Shutdown();
        if (auto stopped = StopNativeSession(state_); stopped.HasError())
            return stopped;
        if (!state.stages.servicesStopped && state.stages.servicesAttempted) {
            if (InvokeProvider(state.operations.shutdownServices, state.candidate) != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.stages.servicesStopped = true;
        }
        state.lease.reset();
        state.stages.closed = true;
        state.quarantine.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformServices
