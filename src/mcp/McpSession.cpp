#include "Horo/Mcp/McpSession.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"
#include "McpJsonBounds.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Mcp {
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
    }  // namespace

    struct McpSessionManager::State {
        explicit State(std::shared_ptr<IMcpRequestController> controllerValue, McpSessionLimits limitsValue)
            : controller(std::move(controllerValue)), limits(std::move(limitsValue)) {}

        std::mutex mutex;
        std::condition_variable drained;
        std::shared_ptr<IMcpRequestController> controller;
        McpSessionLimits limits;
        std::unordered_map<std::uint64_t, std::shared_ptr<SessionRecord>> sessions;
        std::uint64_t nextSessionId{1};
        std::size_t activeCallbacks{};
        bool stopping{};
    };

    /** @copydoc McpSessionManager::Create */
    Result<std::shared_ptr<McpSessionManager>> McpSessionManager::Create(std::shared_ptr<IMcpRequestController> controller,
                                                                         McpSessionLimits limits) {
        if (controller == nullptr || !ValidLimits(limits))
            return Result<std::shared_ptr<McpSessionManager>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        auto state = std::make_shared<State>(std::move(controller), std::move(limits));
        return Result<std::shared_ptr<McpSessionManager>>::Success(
            std::shared_ptr<McpSessionManager>(new McpSessionManager(std::move(state))));
    }

    /** @copydoc McpSessionManager::Open */
    Result<McpSessionHandle> McpSessionManager::Open(McpSessionAdmission admission) {
        if (!ValidAdmission(admission, state_->limits))
            return Result<McpSessionHandle>::Failure(MakeError(McpErrors::AdmissionInvalid));
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
        state_->sessions.emplace(handle.id, std::move(record));
        return Result<McpSessionHandle>::Success(handle);
    }

    /** @copydoc McpSessionManager::Dispatch */
    Result<nlohmann::json> McpSessionManager::Dispatch(const McpSessionHandle session, const McpRequest &request) {
        if (!ValidRequestId(request.id, state_->limits) || !ValidMethod(request.method, state_->limits))
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));
        if (!JsonWithinBounds(request.params, state_->limits, state_->limits.maximumInputBytes))
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::InputCapacityExceeded));
        if (!request.params.is_object() && !request.params.is_array())
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));

        const std::string requestKey = request.id.dump();
        std::shared_ptr<SessionRecord> record;
        McpRequestContext context;
        const auto key = std::pair{session.generation, requestKey};
        {
            std::lock_guard lock{state_->mutex};
            if (state_->stopping)
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::ShuttingDown));
            const auto found = state_->sessions.find(session.id);
            if (found == state_->sessions.end() || found->second->handle != session)
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::SessionUnavailable));
            record = found->second;
            if (record->inFlight.size() >= state_->limits.maximumInFlightPerSession)
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestCapacityExceeded));
            if (record->inFlight.contains(key))
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));

            context = {.session = session,
                       .clientIdentity = record->admission.clientIdentity,
                       .capabilities = record->admission.capabilities,
                       .projectIdentity = record->admission.projectIdentity,
                       .authorizationRevision = record->admission.authorizationRevision,
                       .registryRevision = record->admission.registryRevision};
            auto [entry, inserted] = record->inFlight.emplace(key, CancellationSource{record->cancellation.Token()});
            static_cast<void>(inserted);
            ++state_->activeCallbacks;
            context.cancellation = entry->second.Token();
            context.deadline = std::chrono::steady_clock::now() + state_->limits.requestTimeout;
        }

        const auto controller = state_->controller;
        auto outcome = [&]() -> Result<nlohmann::json> {
            try {
                auto result = controller->Dispatch(request, context);
                if (result.HasValue() && !JsonWithinBounds(result.Value(), state_->limits, state_->limits.maximumResultBytes))
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::ResultCapacityExceeded));
                return result;
            } catch (...) {
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed));
            }
        }();

        {
            std::lock_guard lock{state_->mutex};
            record->inFlight.erase(key);
            --state_->activeCallbacks;
            state_->drained.notify_all();
        }
        return outcome;
    }

    /** @copydoc McpSessionManager::Cancel */
    Result<void> McpSessionManager::Cancel(const McpSessionHandle session, const nlohmann::json &requestId) {
        if (!ValidRequestId(requestId, state_->limits))
            return Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
        const auto key = std::pair{session.generation, requestId.dump()};
        std::lock_guard lock{state_->mutex};
        const auto found = state_->sessions.find(session.id);
        if (found == state_->sessions.end() || found->second->handle != session)
            return Result<void>::Failure(MakeError(McpErrors::SessionUnavailable));
        const auto active = found->second->inFlight.find(key);
        if (active == found->second->inFlight.end())
            return Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
        active->second.RequestCancellation();
        return Result<void>::Success();
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
        CancellationSource replacement;
        auto &record = *found->second;
        record.cancellation.RequestCancellation();
        record.cancellation = std::move(replacement);
        record.admission.projectIdentity = std::move(projectIdentity);
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
