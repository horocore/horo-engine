#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include "Horo/WorldStreaming/WorldPartitionRegistry.h"
#include "WorldStreamingInternal.h"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using Internal::Failure;

        /** @brief Checks whether a service-role value belongs to the supported composition contract. */
        [[nodiscard]] bool KnownRole(const StreamingRuntimeServiceRole role) noexcept {
            using enum StreamingRuntimeServiceRole;
            return role == Planner || role == AssetProvider || role == SceneRuntime || role == FeatureAdapter;
        }

        /** @brief Validates service roles, identities and configured binding ceilings without retaining caller storage. */
        [[nodiscard]] Result<void> ValidateBindings(const std::span<const StreamingRuntimeServiceBinding> services,
                                                    const std::uint32_t maximumFeatureAdapters) {
            if (const std::size_t maximumServices = static_cast<std::size_t>(maximumFeatureAdapters) + 3U;
                services.size() > maximumServices)
                return Failure<void>(WorldStreamingErrors::RuntimeCompositionCapacityExceeded);

            std::array<std::size_t, 4> roleCounts{};
            for (const auto &service : services) {
                if (!service.IsValid())
                    return Failure<void>(WorldStreamingErrors::RuntimeCompositionInvalid);
                ++roleCounts[static_cast<std::size_t>(service.role)];
            }
            if (roleCounts[0] != 1 || roleCounts[1] != 1 || roleCounts[2] != 1 || roleCounts[3] == 0)
                return Failure<void>(WorldStreamingErrors::RuntimeCompositionInvalid);
            if (roleCounts[3] > maximumFeatureAdapters)
                return Failure<void>(WorldStreamingErrors::RuntimeCompositionCapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Copies validated bindings, rejects duplicate identities and produces canonical role/identity order. */
        [[nodiscard]] Result<std::vector<StreamingRuntimeServiceBinding>> CopyCanonicalBindings(
            const std::span<const StreamingRuntimeServiceBinding> services) {
            try {
                std::vector<StreamingRuntimeServiceBinding> copy{services.begin(), services.end()};
                std::ranges::sort(copy, {}, &StreamingRuntimeServiceBinding::id);
                if (std::ranges::adjacent_find(copy, [](const auto &left, const auto &right) {
                    return left.id == right.id;
                }) != copy.end())
                    return Failure<std::vector<StreamingRuntimeServiceBinding>>(WorldStreamingErrors::RuntimeCompositionIdentityConflict);
                std::ranges::sort(copy, [](const auto &left, const auto &right) {
                    return std::pair{left.role, left.id.Value()} < std::pair{right.role, right.id.Value()};
                });
                return Result<std::vector<StreamingRuntimeServiceBinding>>::Success(std::move(copy));
            } catch (const std::bad_alloc &) {
                return Failure<std::vector<StreamingRuntimeServiceBinding>>(WorldStreamingErrors::RuntimeCompositionCapacityExceeded);
            } catch (const std::length_error &) {
                return Failure<std::vector<StreamingRuntimeServiceBinding>>(WorldStreamingErrors::RuntimeCompositionCapacityExceeded);
            }
        }

        /** @brief Validates and copies a complete binding set without publishing partial state. */
        [[nodiscard]] Result<std::vector<StreamingRuntimeServiceBinding>> ValidateAndCopyBindings(
            const std::span<const StreamingRuntimeServiceBinding> services, const std::uint32_t maximumFeatureAdapters) {
            if (const auto validation = ValidateBindings(services, maximumFeatureAdapters); validation.HasError())
                return Result<std::vector<StreamingRuntimeServiceBinding>>::Failure(validation.ErrorValue());
            return CopyCanonicalBindings(services);
        }
    }  // namespace

    /** @copydoc StreamingRuntimeServiceBinding::IsValid */
    bool StreamingRuntimeServiceBinding::IsValid() const noexcept {
        return id.IsValid() && revision.IsValid() && KnownRole(role) && instance != nullptr;
    }

    /** @copydoc StreamingRuntimeOwnerToken::IsValid */
    bool StreamingRuntimeOwnerToken::IsValid() const noexcept {
        return partition.IsValid() && epoch.IsValid() && owner.IsValid();
    }

    /** @copydoc WorldStreamingRuntimeCompositionConfig::IsValid */
    bool WorldStreamingRuntimeCompositionConfig::IsValid() const noexcept {
        return owner.IsValid() && revision.IsValid() && schedulerOwner.IsValid() && schedulerLimits.IsValid() &&
               maximumFeatureAdapters > 0 && maximumFeatureAdapters <= MaximumFeatureAdapters;
    }

    WorldStreamingRuntimeComposition::WorldStreamingRuntimeComposition(const WorldStreamingRuntimeCompositionConfig &config,
                                                                       StreamingSchedulerAdmissionLedger scheduler,
                                                                       std::vector<StreamingRuntimeServiceBinding> services) noexcept
        : config_(config), scheduler_(std::move(scheduler)), services_(std::move(services)) {}

    /** @copydoc WorldStreamingRuntimeComposition::WorldStreamingRuntimeComposition */
    WorldStreamingRuntimeComposition::WorldStreamingRuntimeComposition(WorldStreamingRuntimeComposition &&other) noexcept
        : config_(other.config_), scheduler_(std::move(other.scheduler_)), services_(std::move(other.services_)), state_(other.state_),
          origin_(std::move(other.origin_)), lastOriginTransaction_(other.lastOriginTransaction_),
          pendingOrigin_(std::move(other.pendingOrigin_)) {
        other.scheduler_.BeginShutdown();
        other.state_ = WorldStreamingRuntimeCompositionState::Closed;
    }

    /** @copydoc WorldStreamingRuntimeComposition::Create */
    Result<WorldStreamingRuntimeComposition> WorldStreamingRuntimeComposition::Create(
        const WorldStreamingRuntimeCompositionConfig &config, const std::span<const StreamingRuntimeServiceBinding> services) {
        if (!config.IsValid())
            return Failure<WorldStreamingRuntimeComposition>(WorldStreamingErrors::RuntimeCompositionInvalid);
        auto bindings = ValidateAndCopyBindings(services, config.maximumFeatureAdapters);
        if (bindings.HasError())
            return Result<WorldStreamingRuntimeComposition>::Failure(bindings.ErrorValue());
        auto scheduler = StreamingSchedulerAdmissionLedger::Create(config.schedulerOwner, config.schedulerLimits);
        if (scheduler.HasError())
            return Result<WorldStreamingRuntimeComposition>::Failure(scheduler.ErrorValue());
        return Result<WorldStreamingRuntimeComposition>::Success(
            WorldStreamingRuntimeComposition{config, std::move(scheduler).Value(), std::move(bindings).Value()});
    }

    /** @copydoc WorldStreamingRuntimeComposition::CreateWithOrigin */
    Result<WorldStreamingRuntimeComposition> WorldStreamingRuntimeComposition::CreateWithOrigin(
        const WorldStreamingRuntimeCompositionConfig &config, const std::span<const StreamingRuntimeServiceBinding> services,
        const WorldPartitionRegistry &registry, OriginFrame initial) {
        if (!config.IsValid())
            return Failure<WorldStreamingRuntimeComposition>(WorldStreamingErrors::RuntimeCompositionInvalid);
        const auto mounted = registry.Snapshot();
        if (mounted.HasError())
            return Result<WorldStreamingRuntimeComposition>::Failure(mounted.ErrorValue());
        if (mounted.Value().Binding().owner != config.owner)
            return Failure<WorldStreamingRuntimeComposition>(WorldStreamingErrors::RuntimeCompositionRevisionStale);

        auto composition = Create(config, services);
        if (composition.HasError())
            return composition;
        auto origin = OriginFrameOwner::Create(std::move(initial));
        if (origin.HasError())
            return Result<WorldStreamingRuntimeComposition>::Failure(origin.ErrorValue());
        auto mountedComposition = std::move(composition).Value();
        mountedComposition.origin_ = std::move(origin).Value();
        return Result<WorldStreamingRuntimeComposition>::Success(std::move(mountedComposition));
    }

    /** @copydoc WorldStreamingRuntimeComposition::Origin */
    const OriginFrameOwner *WorldStreamingRuntimeComposition::Origin() const noexcept {
        return state_ == WorldStreamingRuntimeCompositionState::Active ? origin_.get() : nullptr;
    }

    /** @copydoc WorldStreamingRuntimeComposition::PrepareOriginRebase */
    Result<void> WorldStreamingRuntimeComposition::PrepareOriginRebase(const StreamingRuntimeOwnerToken &owner,
                                                                       const StreamingRuntimeCompositionRevision revision,
                                                                       const OriginRebaseTransactionContext &context,
                                                                       const std::span<const OriginRebaseParticipantRequirement> required,
                                                                       const std::span<IOriginRebaseParticipant *const> participants) {
        if (owner != config_.owner || revision != config_.revision)
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionRevisionStale);
        if (!Origin() || pendingOrigin_)
            return Failure<void>(WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
        if (!context.transaction.IsValid())
            return Failure<void>(WorldStreamingErrors::OriginRebaseInvalid);
        if (context.transaction.Value() <= lastOriginTransaction_.Value() || !origin_->Publishes(context.activeFrame))
            return Failure<void>(WorldStreamingErrors::OriginRebaseStale);
        auto prepared = OriginRebaseTransaction::Prepare(context, required, participants);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        pendingOrigin_.emplace(std::move(prepared).Value());
        lastOriginTransaction_ = context.transaction;
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingRuntimeComposition::CommitOriginRebase */
    Result<void> WorldStreamingRuntimeComposition::CommitOriginRebase(const StreamingRuntimeOwnerToken &owner,
                                                                      const StreamingRuntimeCompositionRevision revision,
                                                                      const OriginRebaseTransactionId transaction,
                                                                      const OriginRebaseCommitPoint point) {
        if (owner != config_.owner || revision != config_.revision)
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionRevisionStale);
        if (!Origin() || !pendingOrigin_)
            return Failure<void>(WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
        if (pendingOrigin_->Id() != transaction)
            return Failure<void>(WorldStreamingErrors::OriginRebaseStale);
        auto committed = pendingOrigin_->Commit(*origin_, point, OriginRebaseLifecycle::Active);
        if (pendingOrigin_->State() != OriginRebaseTransactionState::Prepared)
            pendingOrigin_.reset();
        return committed;
    }

    /** @copydoc WorldStreamingRuntimeComposition::CancelOriginRebase */
    Result<void> WorldStreamingRuntimeComposition::CancelOriginRebase(const StreamingRuntimeOwnerToken &owner,
                                                                      const StreamingRuntimeCompositionRevision revision,
                                                                      const OriginRebaseTransactionId transaction) {
        if (owner != config_.owner || revision != config_.revision || !pendingOrigin_ || pendingOrigin_->Id() != transaction)
            return Failure<void>(WorldStreamingErrors::OriginRebaseStale);
        pendingOrigin_.reset();
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingRuntimeComposition::Replace */
    Result<void> WorldStreamingRuntimeComposition::Replace(const StreamingRuntimeOwnerToken &owner,
                                                           const StreamingRuntimeCompositionRevision revision,
                                                           const std::span<const StreamingRuntimeServiceBinding> services) {
        if (owner != config_.owner || !revision.IsValid() || revision.Value() <= config_.revision.Value())
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionRevisionStale);
        if (State() != WorldStreamingRuntimeCompositionState::Active || scheduler_.ReservedCount() != 0 || pendingOrigin_)
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionLifecycleUnavailable);
        auto bindings = ValidateAndCopyBindings(services, config_.maximumFeatureAdapters);
        if (bindings.HasError())
            return Result<void>::Failure(bindings.ErrorValue());
        services_ = std::move(bindings).Value();
        config_.revision = revision;
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingRuntimeComposition::RequestCancellation */
    Result<void> WorldStreamingRuntimeComposition::RequestCancellation(const StreamingRuntimeOwnerToken &owner,
                                                                       const StreamingRuntimeCompositionRevision revision) noexcept {
        using enum WorldStreamingRuntimeCompositionState;
        if (owner != config_.owner || revision != config_.revision)
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionRevisionStale);
        if (state_ == Closed || state_ == Draining)
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionLifecycleUnavailable);
        if (state_ == Active) {
            scheduler_.BeginShutdown();
            state_ = Cancelling;
            pendingOrigin_.reset();
            if (origin_)
                origin_->Shutdown();
        }
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingRuntimeComposition::BeginShutdown */
    Result<void> WorldStreamingRuntimeComposition::BeginShutdown(const StreamingRuntimeOwnerToken &owner) noexcept {
        if (owner != config_.owner)
            return Failure<void>(WorldStreamingErrors::RuntimeCompositionRevisionStale);
        if (State() == WorldStreamingRuntimeCompositionState::Closed)
            return Result<void>::Success();
        scheduler_.BeginShutdown();
        state_ = scheduler_.State() == StreamingSchedulerAdmissionState::Closed ? WorldStreamingRuntimeCompositionState::Closed
                                                                                : WorldStreamingRuntimeCompositionState::Draining;
        pendingOrigin_.reset();
        if (origin_)
            origin_->Shutdown();
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingRuntimeComposition::Resolve */
    Result<StreamingRuntimeServiceBinding> WorldStreamingRuntimeComposition::Resolve(const StreamingRuntimeServiceId id) const {
        if (!id.IsValid())
            return Failure<StreamingRuntimeServiceBinding>(WorldStreamingErrors::RuntimeCompositionInvalid);
        const auto found = std::ranges::find(services_, id, &StreamingRuntimeServiceBinding::id);
        return found == services_.end() ? Failure<StreamingRuntimeServiceBinding>(WorldStreamingErrors::RuntimeCompositionRevisionStale)
                                        : Result<StreamingRuntimeServiceBinding>::Success(*found);
    }

    /** @copydoc WorldStreamingRuntimeComposition::Owner */
    const StreamingRuntimeOwnerToken &WorldStreamingRuntimeComposition::Owner() const noexcept {
        return config_.owner;
    }

    /** @copydoc WorldStreamingRuntimeComposition::Revision */
    StreamingRuntimeCompositionRevision WorldStreamingRuntimeComposition::Revision() const noexcept {
        return config_.revision;
    }

    /** @copydoc WorldStreamingRuntimeComposition::Services */
    std::span<const StreamingRuntimeServiceBinding> WorldStreamingRuntimeComposition::Services() const noexcept {
        return services_;
    }

    /** @copydoc WorldStreamingRuntimeComposition::State */
    WorldStreamingRuntimeCompositionState WorldStreamingRuntimeComposition::State() const noexcept {
        if (state_ == WorldStreamingRuntimeCompositionState::Draining && scheduler_.State() == StreamingSchedulerAdmissionState::Closed)
            return WorldStreamingRuntimeCompositionState::Closed;
        return state_;
    }

    /** @copydoc WorldStreamingRuntimeComposition::Scheduler */
    StreamingSchedulerAdmissionLedger &WorldStreamingRuntimeComposition::Scheduler() noexcept {
        return scheduler_;
    }

    /** @copydoc WorldStreamingRuntimeComposition::Scheduler */
    const StreamingSchedulerAdmissionLedger &WorldStreamingRuntimeComposition::Scheduler() const noexcept {
        return scheduler_;
    }
}  // namespace Horo::WorldStreaming
