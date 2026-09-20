#include "NavigationDynamicRegistryInternal.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>
#include <variant>

namespace Horo::Navigation {
    namespace {
        [[nodiscard]] Result<void> ValidateCommitAdmission(const Detail::NavigationDynamicRegistryState &state,
                                                           const NavigationDynamicRegistryRevision revision,
                                                           const NavigationDynamicSceneBinding &binding, const std::uint64_t targetTick,
                                                           const std::size_t pendingCount) {
            if (state.bound && state.binding != binding)
                return Detail::Failure<void>(NavigationErrors::DynamicRegistryStale);
            if (state.bound && state.hasPublicationTick && targetTick < state.lastPublicationTick)
                return Detail::Failure<void>(NavigationErrors::DynamicRegistryStale);
            if (pendingCount > state.limits.maximumMutationsPerCommit)
                return Detail::Failure<void>(NavigationErrors::DynamicRegistryCapacityExceeded);
            const auto pending = std::span{state.pending}.first(pendingCount);
            if (!std::ranges::all_of(pending, [&binding, targetTick](const auto &command) {
                return Detail::CommandMatchesBinding(command, binding, targetTick);
            }))
                return Detail::Failure<void>(NavigationErrors::DynamicRegistryStale);
            if (std::ranges::any_of(pending, [](const auto &command) {
                return !Detail::CommandRevisionWillAdvance(command);
            }))
                return Detail::Failure<void>(NavigationErrors::GenerationExhausted);
            if (state.bound && pendingCount != 0 && !Detail::RevisionWillAdvance(revision))
                return Detail::Failure<void>(NavigationErrors::GenerationExhausted);
            if (std::ranges::any_of(pending, [](const auto &command) {
                return std::visit([]<typename Value>(const Value &value) {
                    using Type = std::remove_cvref_t<Value>;
                    if constexpr (std::is_same_v<Type, Detail::PendingObstacleRemoval> ||
                                  std::is_same_v<Type, Detail::PendingModifierRemoval>)
                        return value.handle.slot.generation == std::numeric_limits<std::uint32_t>::max();
                    return false;
                }, command);
            }))
                return Detail::Failure<void>(NavigationErrors::GenerationExhausted);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NavigationDynamicRegistry::CommitAtSafePoint */
    Result<NavigationDynamicCommitResult> NavigationDynamicRegistry::CommitAtSafePoint(
        const NavigationWorldActivationDescriptor &activation, const std::uint64_t targetTick) {
        if (shutdown_ || !state_)
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::DynamicRegistryShuttingDown);
        const auto binding = Detail::Binding(activation);
        if (!activation.IsValid() || !binding.IsValid() || targetTick == 0)
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::DynamicRegistryInvalid);
        if (auto admission = ValidateCommitAdmission(*state_, revision_, binding, targetTick, pendingCount_); admission.HasError())
            return Result<NavigationDynamicCommitResult>::Failure(std::move(admission).ErrorValue());

        if (!state_->bound || pendingCount_ != 0)
            state_->cachedSnapshot.reset();
        NavigationDynamicCommitResult result{
            .binding = binding,
            .revision = revision_,
        };
        if (!state_->bound)
            revision_ = NavigationDynamicRegistryRevision::Create(1).Value();
        else if (pendingCount_ != 0)
            revision_ = NavigationDynamicRegistryRevision::Create(revision_.Value() + 1U).Value();
        result.revision = revision_;

        const auto pending = std::span{state_->pending}.first(pendingCount_);
        for (const auto &command : pending) {
            std::visit([&](const auto &value) {
                Detail::ApplyPendingCommand(*state_, value, result);
            }, command);
        }

        state_->binding = binding;
        state_->bound = true;
        state_->hasPublicationTick = true;
        state_->lastPublicationTick = targetTick;
        for (std::size_t index = 0; index < pendingCount_; ++index)
            state_->pending[index] = Detail::PendingObstacleRegistration{};
        pendingCount_ = 0;
        return Result<NavigationDynamicCommitResult>::Success(result);
    }

    /** @copydoc NavigationDynamicRegistry::ReplaceSceneAtSafePoint */
    Result<NavigationDynamicCommitResult> NavigationDynamicRegistry::ReplaceSceneAtSafePoint(
        const NavigationWorldActivationDescriptor &activation, const std::uint64_t targetTick) {
        if (shutdown_ || !state_)
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::DynamicRegistryShuttingDown);
        const auto binding = Detail::Binding(activation);
        if (!activation.IsValid() || !binding.IsValid() || targetTick == 0)
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::DynamicRegistryInvalid);
        if (state_->bound && !Detail::RevisionWillAdvance(revision_))
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::GenerationExhausted);
        if (state_->bound && state_->hasPublicationTick && targetTick < state_->lastPublicationTick)
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::DynamicRegistryStale);
        if (Detail::HasActiveGenerationAtLimit(state_->obstacles) || Detail::HasActiveGenerationAtLimit(state_->modifiers))
            return Detail::Failure<NavigationDynamicCommitResult>(NavigationErrors::GenerationExhausted);

        ClearStaged();
        Detail::ClearActiveSlots(state_->obstacles);
        Detail::ClearActiveSlots(state_->modifiers);
        revision_ = state_->bound ? NavigationDynamicRegistryRevision::Create(revision_.Value() + 1U).Value()
                                  : NavigationDynamicRegistryRevision::Create(1).Value();
        state_->cachedSnapshot.reset();
        state_->binding = binding;
        state_->bound = true;
        state_->hasPublicationTick = true;
        state_->lastPublicationTick = targetTick;
        return Result<NavigationDynamicCommitResult>::Success(NavigationDynamicCommitResult{.binding = binding, .revision = revision_});
    }

    /** @copydoc NavigationDynamicRegistry::ClearStaged */
    void NavigationDynamicRegistry::ClearStaged() noexcept {
        if (!state_)
            return;
        for (std::size_t index = 0; index < pendingCount_; ++index) {
            std::visit([this](const auto &value) {
                Detail::ReleasePendingRegistration(*state_, value);
            }, state_->pending[index]);
            state_->pending[index] = Detail::PendingObstacleRegistration{};
        }
        pendingCount_ = 0;
    }
}  // namespace Horo::Navigation
