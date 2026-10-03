#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"
#include "PlatformProviderLifecycleState.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace Horo::PlatformServices {
    namespace {
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

        /** @brief Applies a copied completion to its exact live native request and retires the lease. */
        void ApplyCompletion(PlatformProviderLifecycleState &state, const PlatformProviderLifecycleState::Completion &completion,
                             const PlatformProviderLifecycleState::InFlight &entry,
                             const PlatformProviderLifecycleHost::RequestHandle &handle) {
            std::uint64_t currentSessionRevision{};
            {
                std::scoped_lock lock{state.mutex};
                currentSessionRevision = state.session.revision;
            }
            if (completion.receivedAt > entry.deadline)
                static_cast<void>(state.requests.CompleteTimedOut(handle, MakeError(RequestErrors::TimedOut)));
            else if (completion.sessionRevision != entry.sessionRevision || currentSessionRevision != entry.sessionRevision)
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
        }

        /** @brief Preserves a copied pre-deadline result while a completion budget defers it. */
        [[nodiscard]] bool HasEligibleIngress(PlatformProviderLifecycleState &state,
                                              const PlatformProviderLifecycleState::InFlight &entry) {
            std::scoped_lock lock{state.mutex};
            for (std::size_t index = 0; index < state.completionCount; ++index) {
                const auto &pending = state.completions[(state.completionHead + index) % state.completions.size()];
                if (pending.requestId == entry.id.value && pending.requestGeneration == entry.generation.value &&
                    pending.service == static_cast<std::uint32_t>(entry.service) && pending.operation == entry.operation &&
                    pending.receivedAt <= entry.deadline)
                    return true;
            }
            return false;
        }

        /** @brief Acquires a native lease and submits one queued request on the owner lane. */
        [[nodiscard]] bool StartQueued(PlatformProviderLifecycleState &state, PlatformProviderLifecycleState::InFlight &entry,
                                       const PlatformProviderLifecycleHost::RequestHandle &handle) {
            std::uint64_t currentSessionRevision{};
            {
                std::scoped_lock lock{state.mutex};
                currentSessionRevision = state.session.revision;
            }
            if (currentSessionRevision != entry.sessionRevision) {
                static_cast<void>(state.requests.CompleteFailure(handle, MakeError(PlatformSessionErrors::StaleSession)));
                return false;
            }
            auto lease = state.lease->AcquireRequestLease();
            if (lease.HasError()) {
                static_cast<void>(state.requests.CompleteFailure(handle, lease.ErrorValue()));
                return false;
            }
            entry.lease.emplace(std::move(lease).Value());
            static_cast<void>(state.requests.MarkRunning(handle));
            const HoroPlatformProviderOperation input{.structSize = sizeof(HoroPlatformProviderOperation),
                                                      .requestId = entry.id.value,
                                                      .requestGeneration = entry.generation.value,
                                                      .sessionRevision = entry.sessionRevision,
                                                      .service = static_cast<std::uint32_t>(entry.service),
                                                      .operation = entry.operation,
                                                      // The public C ABI requires a uint8_t byte span.
                                                      .payload = reinterpret_cast<const std::uint8_t *>(  // NOSONAR(cpp:S6022)
                                                          entry.payload.data()),
                                                      .payloadSize = static_cast<std::uint32_t>(entry.payload.size())};
            if (const auto submission = Detail::InvokeProvider(state.operations.submit, state.candidate, &input);
                submission == HORO_EXTENSION_ERROR_CANCELLED) {
                static_cast<void>(state.requests.RequestCancel(handle));
                static_cast<void>(state.requests.CompleteCancelled(handle, MakeError(RequestErrors::Cancelled)));
            } else if (submission != HORO_EXTENSION_SUCCESS)
                static_cast<void>(
                    state.requests.CompleteFailure(handle, MakePlatformProviderError(PlatformProviderFailureCategory::Unknown,
                                                                                     entry.id.value, entry.generation.value)));
            // A provider may have started work before reporting failure. Keep its lease until callback or drain.
            return true;
        }

        /** @brief Returns whether an in-flight record still needs native retirement. */
        [[nodiscard]] bool AdvanceRequest(PlatformProviderLifecycleState &state, PlatformProviderLifecycleState::InFlight &entry,
                                          const PlatformProviderLifecycleHost::RequestHandle &handle,
                                          const std::chrono::steady_clock::time_point now) {
            const auto snapshot = state.requests.Query(handle);
            if (const bool eligibleIngress = entry.lease && now >= entry.deadline && HasEligibleIngress(state, entry);
                now >= entry.deadline && !eligibleIngress) {
                static_cast<void>(state.requests.CompleteTimedOut(handle, MakeError(RequestErrors::TimedOut)));
                if (entry.lease && !entry.cancellationSent) {
                    entry.cancellationSent = true;
                    static_cast<void>(
                        Detail::InvokeProvider(state.operations.cancel, state.candidate, entry.id.value, entry.generation.value));
                }
                return entry.lease.has_value();
            }
            if (entry.lease && snapshot.HasValue() && snapshot.Value().cancellationRequested && !entry.cancellationSent) {
                entry.cancellationSent = true;
                static_cast<void>(Detail::InvokeProvider(state.operations.cancel, state.candidate, entry.id.value, entry.generation.value));
            } else if (!entry.lease && !state.stages.closing)
                return StartQueued(state, entry, handle);
            return true;
        }
    }  // namespace

    /** @copydoc PlatformProviderLifecycleHost::DispatchCompletions */
    std::size_t PlatformProviderLifecycleHost::DispatchCompletions(  // NOSONAR(cpp:S5817) Mutates shared lifecycle state.
        const std::size_t maximum, const std::chrono::steady_clock::time_point now) {
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
                found != state.inFlight.end() && found->lease) {
                RequestHandle handle{found->id, found->generation};
                ApplyCompletion(state, completion, *found, handle);
                state.inFlight.erase(found);
            }
            ++processed;
        }
        for (auto entry = state.inFlight.begin(); entry != state.inFlight.end();) {
            RequestHandle handle{entry->id, entry->generation};
            if (AdvanceRequest(state, *entry, handle, now))
                ++entry;
            else
                entry = state.inFlight.erase(entry);
        }
        static_cast<void>(state.requests.DispatchCompletions(maximum));
        return processed;
    }

}  // namespace Horo::PlatformServices
