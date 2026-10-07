#include "Horo/Mcp/McpSession.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpErrors.h"
#include "McpJsonBounds.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Mcp {
    /** @copydoc McpRequestContext::IsStopRequested */
    bool McpRequestContext::IsStopRequested() const noexcept {
        return cancellation.IsCancellationRequested() || std::chrono::steady_clock::now() >= deadline || !authority ||
               authority->IsStopRequested();
    }

    /** @copydoc IMcpRequestController::CancelAccepted */
    Result<void> IMcpRequestController::CancelAccepted(const McpSessionHandle, const nlohmann::json &) {
        return Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
    }

    namespace {
        /** @brief Keeps host-configurable bounds finite even when configuration itself is untrusted. */
        [[nodiscard]] bool ValidLimits(const McpSessionLimits &limits) noexcept {
            return limits.maximumSessions > 0 && limits.maximumSessions <= 1024 && limits.maximumInFlightPerSession > 0 &&
                   limits.maximumInFlightPerSession <= 1024 && limits.maximumFrameBytes >= 256 && limits.maximumFrameBytes <= (8U << 20U) &&
                   limits.maximumInputBytes > 0 && limits.maximumInputBytes <= limits.maximumFrameBytes && limits.maximumResultBytes > 0 &&
                   limits.maximumResultBytes <= (1U << 20U) && limits.maximumDepth > 0 && limits.maximumDepth <= 128 &&
                   limits.maximumNodes > 0 && limits.maximumNodes <= 65536 && limits.maximumStringBytes > 0 &&
                   limits.maximumStringBytes <= limits.maximumInputBytes && limits.maximumIdentityBytes > 0 &&
                   limits.maximumIdentityBytes <= 4096 && limits.maximumCapabilities > 0 && limits.maximumCapabilities <= 1024 &&
                   limits.requestTimeout > std::chrono::milliseconds::zero() && limits.requestTimeout <= std::chrono::hours{24} &&
                   limits.shutdownDrainTimeout > std::chrono::milliseconds::zero() && limits.shutdownDrainTimeout <= std::chrono::hours{24};
        }

        /** @brief Rejects anonymous, duplicate, or unbounded host-supplied authority. */
        [[nodiscard]] bool ValidAdmission(const McpSessionAdmission &admission, const McpSessionLimits &limits) {
            if (admission.clientIdentity.empty() || admission.clientIdentity.size() > limits.maximumIdentityBytes ||
                !IsValidUtf8ScalarSequence(admission.clientIdentity) || admission.authorizationRevision == 0 ||
                admission.registryRevision == 0 || admission.capabilities.size() > limits.maximumCapabilities ||
                (admission.projectIdentity.has_value() &&
                 (admission.projectIdentity->empty() || admission.projectIdentity->size() > limits.maximumIdentityBytes ||
                  !IsValidUtf8ScalarSequence(*admission.projectIdentity))))
                return false;

            std::unordered_set<std::string_view> unique;
            unique.reserve(admission.capabilities.size());
            for (const std::string &capability : admission.capabilities) {
                if (capability.empty() || capability.size() > limits.maximumIdentityBytes || !IsValidUtf8ScalarSequence(capability) ||
                    !unique.insert(capability).second)
                    return false;
            }
            return true;
        }

        /** @brief Requires a bounded method token before reaching the injected controller. */
        [[nodiscard]] bool ValidMethod(const std::string_view method, const McpSessionLimits &limits) noexcept {
            return !method.empty() && method.size() <= limits.maximumIdentityBytes && IsValidUtf8ScalarSequence(method) &&
                   std::ranges::all_of(method, [](const unsigned char character) {
                return character >= 0x20U && character != 0x7fU;
            });
        }

        struct SessionRecord {
            McpSessionHandle handle;
            McpSessionAdmission admission;
            CancellationSource cancellation;
            std::map<std::pair<std::uint64_t, std::string>, CancellationSource> inFlight;
        };

        /** @brief Owns the record, exact active-request identity and immutable context across an unlocked callback. */
        struct AcceptedRequest {
            std::shared_ptr<SessionRecord> record;
            McpRequestContext context;
            std::pair<std::uint64_t, std::string> key;
        };

        static_assert(std::is_nothrow_move_constructible_v<Result<AcceptedRequest>>);

        /** @brief Checks the session envelope before active-request bookkeeping or controller execution. */
        Result<void> ValidateRequest(const McpRequest &request, const McpSessionLimits &limits) {
            if (!ValidRequestId(request.id, limits) || !ValidMethod(request.method, limits))
                return Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
            if (!JsonWithinBounds(request.params, limits, limits.maximumInputBytes))
                return Result<void>::Failure(MakeError(McpErrors::InputCapacityExceeded));
            return request.params.is_object() || request.params.is_array() ? Result<void>::Success()
                                                                           : Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
        }

        /** @brief Isolates controller execution, bounded result validation, and exception translation. */
        [[nodiscard]] Result<nlohmann::json> InvokeController(IMcpRequestController &controller, const McpRequest &request,
                                                              const McpRequestContext &context, const McpSessionLimits &limits) {
            try {
                auto result = controller.Dispatch(request, context);
                if (result.HasValue() && !JsonWithinBounds(result.Value(), limits, limits.maximumResultBytes))
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::ResultCapacityExceeded));
                return result;
            } catch (...) {
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed));
            }
        }
    }  // namespace

    struct McpSessionManager::State {
        explicit State(std::shared_ptr<IMcpRequestController> controllerValue, McpSessionLimits limitsValue)
            : controller(std::move(controllerValue)), limits(std::move(limitsValue)) {}

        std::mutex mutex;
        std::condition_variable drained;
        std::shared_ptr<IMcpRequestController> controller;
        McpSessionLimits limits;
        std::shared_ptr<McpAuthorization> authorization;
        std::unordered_map<std::uint64_t, std::shared_ptr<SessionRecord>> sessions;
        std::uint64_t nextSessionId{1};
        std::size_t activeCallbacks{};
        bool stopping{};

        /** @brief Admits and records one callback atomically; no callback executes while this lock is held. */
        Result<AcceptedRequest> BeginRequest(const McpSessionHandle session, const McpRequest &request) {
            std::lock_guard lock{mutex};
            if (stopping)
                return Result<AcceptedRequest>::Failure(MakeError(McpErrors::ShuttingDown));
            const auto found = sessions.find(session.id);
            if (found == sessions.end() || found->second->handle != session)
                return Result<AcceptedRequest>::Failure(MakeError(McpErrors::SessionUnavailable));
            const auto &record = found->second;
            if (!record->admission.authority)
                return Result<AcceptedRequest>::Failure(MakeError(McpErrors::AuthorizationDenied));
            if (record->inFlight.size() >= limits.maximumInFlightPerSession)
                return Result<AcceptedRequest>::Failure(MakeError(McpErrors::RequestCapacityExceeded));
            const auto key = std::pair{session.generation, request.id.dump()};
            if (record->inFlight.contains(key))
                return Result<AcceptedRequest>::Failure(MakeError(McpErrors::RequestInvalid));
            CancellationSource cancellation{record->cancellation.Token()};
            // Prepare every owned copy and the result before publishing callback bookkeeping.
            auto accepted = Result<AcceptedRequest>::Success({record, RequestContext(*record, request, cancellation.Token()), key});
            record->inFlight.try_emplace(key, std::move(cancellation));
            ++activeCallbacks;
            // NRVO or the statically checked nonthrowing result move follows publication; no copy is required.
            return accepted;
        }

        /** @brief Derives a request cancellation chain and expiry-bounded context under the admission lock. */
        McpRequestContext RequestContext(const SessionRecord &record, const McpRequest &request,
                                         const CancellationToken &cancellation) const {
            const auto &admission = record.admission;
            return {.session = record.handle,
                    .clientIdentity = admission.clientIdentity,
                    .capabilities = admission.capabilities,
                    .projectIdentity = admission.projectIdentity,
                    .authorizationRevision = admission.authorizationRevision,
                    .registryRevision = admission.registryRevision,
                    .cancellation = cancellation,
                    .deadline = std::min(std::chrono::steady_clock::now() + limits.requestTimeout, admission.authority->ExpiresAt()),
                    .authority = admission.authority,
                    .authorization = authorization,
                    .requestIdentity = request.id};
        }

        /** @brief Releases the exact accepted callback and wakes shutdown after controller execution. */
        void CompleteRequest(const AcceptedRequest &accepted) {
            std::lock_guard lock{mutex};
            accepted.record->inFlight.erase(accepted.key);
            --activeCallbacks;
            drained.notify_all();
        }

        /** @brief Releases accepted bookkeeping exactly once on normal completion or stack unwinding. */
        struct Completion final {
            State &state;
            const AcceptedRequest &accepted;
            bool active{true};

            ~Completion() {
                Release();
            }

            void Release() {
                if (active) {
                    active = false;
                    state.CompleteRequest(accepted);
                }
            }

            Completion(const Completion &) = delete;
            Completion &operator=(const Completion &) = delete;
            Completion(Completion &&) = delete;
            Completion &operator=(Completion &&) = delete;

            Completion(State &owner, const AcceptedRequest &request) noexcept : state(owner), accepted(request) {}
        };
    };

    /** @copydoc McpSessionManager::Create */
    Result<std::shared_ptr<McpSessionManager>> McpSessionManager::Create(std::shared_ptr<IMcpRequestController> controller,
                                                                         McpSessionLimits limits,
                                                                         std::shared_ptr<McpAuthorization> authorization) {
        if (controller == nullptr || !ValidLimits(limits) || !authorization)
            return Result<std::shared_ptr<McpSessionManager>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        auto state = std::make_shared<State>(std::move(controller), std::move(limits));
        state->authorization = std::move(authorization);
        return Result<std::shared_ptr<McpSessionManager>>::Success(
            std::shared_ptr<McpSessionManager>(new McpSessionManager(std::move(state))));
    }

    /** @copydoc McpSessionManager::Open */
    Result<McpSessionHandle> McpSessionManager::Open(McpSessionAdmission admission) {
        if (!ValidAdmission(admission, state_->limits))
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::AdmissionInvalid));
        const McpRequestContext authorityContext{.clientIdentity = admission.clientIdentity,
                                                 .capabilities = admission.capabilities,
                                                 .projectIdentity = admission.projectIdentity,
                                                 .authorizationRevision = admission.authorizationRevision,
                                                 .registryRevision = admission.registryRevision,
                                                 .authority = admission.authority};
        if (const auto authorized = state_->authorization->Validate(authorityContext); authorized.HasError())
            return Result<McpSessionHandle>::Failure(authorized.ErrorValue());
        std::lock_guard lock{state_->mutex};
        if (state_->stopping)
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::ShuttingDown));
        if (state_->sessions.size() >= state_->limits.maximumSessions)
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::SessionCapacityExceeded));
        if (state_->nextSessionId == std::numeric_limits<std::uint64_t>::max())
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::SessionCapacityExceeded));

        const McpSessionHandle handle{state_->nextSessionId++, 1};
        auto record = std::make_shared<SessionRecord>();
        record->handle = handle;
        record->admission = std::move(admission);
        record->cancellation = CancellationSource{record->admission.authority->Cancellation()};
        state_->sessions.try_emplace(handle.id, std::move(record));
        return Result<McpSessionHandle>::Success(handle);
    }

    /** @copydoc McpSessionManager::Dispatch */
    Result<nlohmann::json> McpSessionManager::Dispatch(const McpSessionHandle session, const McpRequest &request) {
        if (const auto validated = ValidateRequest(request, state_->limits); validated.HasError())
            return Result<nlohmann::json>::Failure(validated.ErrorValue());
        const auto state = state_;
        const auto admitted = state->BeginRequest(session, request);
        if (admitted.HasError())
            return Result<nlohmann::json>::Failure(admitted.ErrorValue());
        const auto &accepted = admitted.Value();
        State::Completion completion{*state, accepted};
        const auto &context = accepted.context;
        const auto controller = state->controller;
        const auto authorized = state->authorization->Validate(context);
        const auto outcome = authorized.HasValue() ? InvokeController(*controller, request, context, state->limits)
                                                   : Result<nlohmann::json>::Failure(authorized.ErrorValue());

        completion.Release();
        // A synchronous adapter may finish after close, project replacement or revocation.
        // Never publish its stale result to a caller whose authority has already ended.
        if (const auto current = state->authorization->ValidateActive(context); current.HasError()) {
            if (current.ErrorValue().code.Value() == McpErrors::RequestCancelled.code.Value() &&
                std::chrono::steady_clock::now() >= context.deadline)
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestTimedOut));
            return Result<nlohmann::json>::Failure(current.ErrorValue());
        }
        return outcome;
    }

    /** @copydoc McpSessionManager::Cancel */
    Result<void> McpSessionManager::Cancel(const McpSessionHandle session, const nlohmann::json &requestId) {
        if (!ValidRequestId(requestId, state_->limits))
            return Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
        const auto key = std::pair{session.generation, requestId.dump()};
        {
            std::lock_guard lock{state_->mutex};
            const auto found = state_->sessions.find(session.id);
            if (found == state_->sessions.end() || found->second->handle != session)
                return Result<void>::Failure(MakeError(McpErrors::SessionUnavailable));
            const auto active = found->second->inFlight.find(key);
            if (active != found->second->inFlight.end()) {
                active->second.RequestCancellation();
                return Result<void>::Success();
            }
        }
        return state_->controller->CancelAccepted(session, requestId);
    }

    /** @copydoc McpSessionManager::SwitchProject */
    Result<McpSessionHandle> McpSessionManager::SwitchProject(const McpSessionHandle session, std::optional<std::string> projectIdentity) {
        if (projectIdentity.has_value() && (projectIdentity->empty() || projectIdentity->size() > state_->limits.maximumIdentityBytes ||
                                            !IsValidUtf8ScalarSequence(*projectIdentity)))
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::AdmissionInvalid));
        std::lock_guard lock{state_->mutex};
        if (state_->stopping)
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::ShuttingDown));
        const auto found = state_->sessions.find(session.id);
        if (found == state_->sessions.end() || found->second->handle != session)
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::SessionUnavailable));
        if (session.generation == std::numeric_limits<std::uint64_t>::max())
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::SessionCapacityExceeded));
        // Project replacement never carries a credential or approvals into the new scope.
        // The host must authenticate again; the advanced handle exists only for lifecycle cleanup.
        CancellationSource replacement;
        auto &record = *found->second;
        record.cancellation.RequestCancellation();
        record.cancellation = std::move(replacement);
        record.admission.projectIdentity = std::move(projectIdentity);
        record.admission.authority.reset();
        ++record.handle.generation;
        return Result<McpSessionHandle>::Success(record.handle);
    }

    /** @copydoc McpSessionManager::Close */
    void McpSessionManager::Close(const McpSessionHandle session) noexcept {
        std::lock_guard lock{state_->mutex};
        const auto found = state_->sessions.find(session.id);
        if (found != state_->sessions.end() && found->second->handle == session) {
            found->second->cancellation.RequestCancellation();
            state_->sessions.erase(found);
        }
    }

    /** @copydoc McpSessionManager::Shutdown */
    Result<void> McpSessionManager::Shutdown() {
        std::unique_lock lock{state_->mutex};
        state_->stopping = true;
        for (const auto &[id, session] : state_->sessions) {
            static_cast<void>(id);
            session->cancellation.RequestCancellation();
        }
        state_->sessions.clear();
        if (!state_->drained.wait_for(lock, state_->limits.shutdownDrainTimeout, [this] {
            return state_->activeCallbacks == 0;
        }))
            return Result<void>::Failure(MakeError(McpErrors::DrainTimedOut));
        return Result<void>::Success();
    }

    /** @copydoc McpSessionManager::ActiveSessions */
    std::size_t McpSessionManager::ActiveSessions() const noexcept {
        std::lock_guard lock{state_->mutex};
        return state_->sessions.size();
    }

    /** @copydoc McpSessionManager::Limits */
    const McpSessionLimits &McpSessionManager::Limits() const noexcept {
        return state_->limits;
    }

    McpSessionManager::McpSessionManager(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
}  // namespace Horo::Mcp
