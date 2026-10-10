#include "Horo/Navigation/NavigationRuntimeQueues.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationBoundedQueue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using Detail::BoundedMpmcQueue;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsValidPathRequest(const NavigationPathRequest &request) noexcept {
            return request.world.IsValid() && request.topology.IsValid() && Math::IsFinite(request.start) &&
                   Math::IsFinite(request.destination) && request.filter.IsValid() &&
                   request.coveragePolicy < NavigationPathCoveragePolicy::Count && request.requirement.query == NavigationQueryKind::Path &&
                   request.requirement.quality < NavigationQualityLevel::Count && request.requirement.limits.maximumNodeExpansions > 0 &&
                   request.requirement.limits.maximumResultPoints >= 2 &&
                   std::isfinite(request.requirement.limits.maximumSearchDistanceMeters) &&
                   request.requirement.limits.maximumSearchDistanceMeters > 0.0F;
        }

        [[nodiscard]] bool IsValidCommand(const NavigationRuntimeCommand &command) noexcept {
            return std::visit([]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, NavigationSubmitPathCommand>)
                    return value.sequence != 0 && IsValidPathRequest(value.request);
                else if constexpr (std::is_same_v<Value, NavigationCancelRequestCommand>)
                    return value.sequence != 0 && value.world.IsValid() && value.handle.IsValid() && value.handle.world == value.world;
                else
                    return false;
            }, command);
        }

        [[nodiscard]] bool IsValidQuery(const NavigationQueuedQuery &query) noexcept {
            if (query.acceptedSequence == 0 || !query.handle.IsValid() || !query.worldLease.IsValid() || !IsValidPathRequest(query.request))
                return false;
            const NavigationWorldActivationDescriptor &descriptor = query.worldLease.Descriptor();
            return query.handle.world == query.request.world && descriptor.world == query.request.world &&
                   descriptor.topology == query.request.topology && !query.worldLease.IsRevoked();
        }

        [[nodiscard]] bool IsValidCompletion(const NavigationQueuedCompletion &completion) noexcept {
            return completion.acceptedSequence != 0 && completion.handle.IsValid() && completion.scene.IsValid() &&
                   completion.sceneGeneration.IsValid() && completion.world.IsValid() && completion.topology.IsValid() &&
                   completion.handle.world == completion.world;
        }

        [[nodiscard]] std::optional<std::size_t> RequiredStorage(const NavigationRuntimeQueueDescriptor &descriptor) noexcept {
            const std::array storage{
                BoundedMpmcQueue<NavigationRuntimeCommand>::StorageBytes(descriptor.commandSlots),
                BoundedMpmcQueue<NavigationQueuedQuery>::StorageBytes(descriptor.querySlots),
                BoundedMpmcQueue<NavigationQueuedCompletion>::StorageBytes(descriptor.completionSlots),
            };
            if (std::ranges::any_of(storage, [](const auto &value) {
                return !value.has_value();
            }))
                return std::nullopt;
            std::size_t total{};
            for (const auto bytes : storage) {
                if (*bytes > std::numeric_limits<std::size_t>::max() - total)
                    return std::nullopt;
                total += *bytes;
            }
            return total;
        }
    }  // namespace

    struct NavigationRuntimeQueues::State final {
        explicit State(const NavigationRuntimeQueueDescriptor &descriptor)
            : commands(descriptor.commandSlots), queries(descriptor.querySlots), completions(descriptor.completionSlots) {}

        BoundedMpmcQueue<NavigationRuntimeCommand> commands;
        BoundedMpmcQueue<NavigationQueuedQuery> queries;
        BoundedMpmcQueue<NavigationQueuedCompletion> completions;
    };

    NavigationRuntimeQueues::NavigationRuntimeQueues(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc NavigationRuntimeQueues::Create */
    Result<NavigationRuntimeQueues> NavigationRuntimeQueues::Create(const NavigationRuntimeQueueDescriptor &descriptor) {
        if (const auto required = RequiredStorage(descriptor); !required.has_value() || descriptor.maximumOwnedBytes < *required)
            return Failure<NavigationRuntimeQueues>(NavigationErrors::CapacityExceeded);
        try {
            return Result<NavigationRuntimeQueues>::Success(NavigationRuntimeQueues{std::make_unique<State>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<NavigationRuntimeQueues>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc NavigationRuntimeQueues::RequiredStorageBytes */
    Result<std::size_t> NavigationRuntimeQueues::RequiredStorageBytes(const NavigationRuntimeQueueDescriptor &descriptor) {
        const auto required = RequiredStorage(descriptor);
        if (!required.has_value())
            return Failure<std::size_t>(NavigationErrors::CapabilityDescriptorInvalid);
        return Result<std::size_t>::Success(*required);
    }

    /** @copydoc NavigationRuntimeQueues::NavigationRuntimeQueues(NavigationRuntimeQueues&&) */
    NavigationRuntimeQueues::NavigationRuntimeQueues(NavigationRuntimeQueues &&other) noexcept = default;
    /** @copydoc NavigationRuntimeQueues::~NavigationRuntimeQueues */
    NavigationRuntimeQueues::~NavigationRuntimeQueues() = default;

    /** @copydoc NavigationRuntimeQueues::TryEnqueueCommand */
    NavigationQueueEnqueueResult NavigationRuntimeQueues::TryEnqueueCommand(NavigationRuntimeCommand &command) noexcept {
        if (!state_ || !IsValidCommand(command))
            return NavigationQueueEnqueueResult::InvalidRecord;
        return state_->commands.TryPush(command);
    }

    /** @copydoc NavigationRuntimeQueues::TryDequeueCommand */
    std::optional<NavigationRuntimeCommand> NavigationRuntimeQueues::TryDequeueCommand() noexcept {
        return state_ ? state_->commands.TryPop() : std::nullopt;
    }

    /** @copydoc NavigationRuntimeQueues::TryEnqueueQuery */
    NavigationQueueEnqueueResult NavigationRuntimeQueues::TryEnqueueQuery(NavigationQueuedQuery &query) noexcept {
        if (!state_ || !IsValidQuery(query))
            return NavigationQueueEnqueueResult::InvalidRecord;
        return state_->queries.TryPush(query);
    }

    /** @copydoc NavigationRuntimeQueues::TryDequeueQuery */
    std::optional<NavigationQueuedQuery> NavigationRuntimeQueues::TryDequeueQuery() noexcept {
        return state_ ? state_->queries.TryPop() : std::nullopt;
    }

    /** @copydoc NavigationRuntimeQueues::TryEnqueueCompletion */
    NavigationQueueEnqueueResult NavigationRuntimeQueues::TryEnqueueCompletion(NavigationQueuedCompletion &completion) noexcept {
        if (!state_ || !IsValidCompletion(completion))
            return NavigationQueueEnqueueResult::InvalidRecord;
        return state_->completions.TryPush(completion);
    }

    /** @copydoc NavigationRuntimeQueues::TryDequeueCompletion */
    std::optional<NavigationQueuedCompletion> NavigationRuntimeQueues::TryDequeueCompletion() noexcept {
        return state_ ? state_->completions.TryPop() : std::nullopt;
    }

    /** @copydoc NavigationRuntimeQueues::CloseAdmission */
    void NavigationRuntimeQueues::CloseAdmission() noexcept {
        if (!state_)
            return;
        state_->commands.Close();
        state_->queries.Close();
    }

    /** @copydoc NavigationRuntimeQueues::CloseCompletions */
    void NavigationRuntimeQueues::CloseCompletions() noexcept {
        if (state_)
            state_->completions.Close();
    }

    /** @copydoc NavigationRuntimeQueues::IsDrained */
    bool NavigationRuntimeQueues::IsDrained() const noexcept {
        return !state_ ||
               (state_->commands.IsClosedAndEmpty() && state_->queries.IsClosedAndEmpty() && state_->completions.IsClosedAndEmpty());
    }

    /** @copydoc NavigationRuntimeQueues::Stats */
    NavigationRuntimeQueueStats NavigationRuntimeQueues::Stats() const noexcept {
        if (!state_)
            return {};
        return {
            .commands = state_->commands.Stats(),
            .queries = state_->queries.Stats(),
            .completions = state_->completions.Stats(),
        };
    }
}  // namespace Horo::Navigation
