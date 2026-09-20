#include "Horo/WorldStreaming/WorldEntityReferenceFixup.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const WorldDependencyKind kind) noexcept {
            return kind == WorldDependencyKind::Hard || kind == WorldDependencyKind::Soft;
        }

        [[nodiscard]] bool IsKnown(const WorldEntityReferenceFixupDisposition disposition) noexcept {
            return disposition <= WorldEntityReferenceFixupDisposition::Failed;
        }

        [[nodiscard]] bool IsKnown(const WorldEntityReferenceFixupMutation mutation) noexcept {
            return mutation <= WorldEntityReferenceFixupMutation::Failed;
        }

        [[nodiscard]] bool SameIdentity(const WorldDependencyCandidate &left, const WorldDependencyCandidate &right) noexcept {
            return left.source.address == right.source.address && left.target.address == right.target.address;
        }

        [[nodiscard]] bool LessRequest(const WorldEntityReferenceFixupRequest &left,
                                       const WorldEntityReferenceFixupRequest &right) noexcept {
            return std::pair{left.reference.source.address, left.reference.target.address} <
                   std::pair{right.reference.source.address, right.reference.target.address};
        }

        template <typename Pending> [[nodiscard]] auto FindPending(Pending &pending, const WorldEntityReferenceFixupRequest &request) {
            return std::ranges::find_if(pending, [&request](const auto &candidate) {
                return SameIdentity(candidate.reference, request.reference);
            });
        }

        [[nodiscard]] const WorldEntityReferenceBinding *FindBinding(const std::span<const WorldEntityReferenceBinding> bindings,
                                                                     const WorldAuthoringObjectAddress address) noexcept {
            const auto found = std::ranges::find_if(bindings, [address](const auto &binding) {
                return binding.endpoint.address == address;
            });
            return found == bindings.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<void> ValidateMappings(const std::span<const WorldEntityReferenceBinding> mappings,
                                                    const std::uint32_t maximumMappings) {
            if (mappings.size() > maximumMappings)
                return Failure<void>(WorldStreamingErrors::EntityFixupCapacityExceeded);
            try {
                std::vector<WorldAuthoringObjectAddress> endpoints;
                std::vector<WorldRuntimeEntityId> runtimes;
                endpoints.reserve(mappings.size());
                runtimes.reserve(mappings.size());
                for (const WorldEntityReferenceBinding &mapping : mappings) {
                    if (!mapping.IsValid())
                        return Failure<void>(WorldStreamingErrors::EntityFixupInvalid);
                    endpoints.push_back(mapping.endpoint.address);
                    runtimes.push_back(mapping.runtime);
                }
                std::ranges::sort(endpoints);
                std::ranges::sort(runtimes);
                if (std::ranges::adjacent_find(endpoints) != endpoints.end() || std::ranges::adjacent_find(runtimes) != runtimes.end())
                    return Failure<void>(WorldStreamingErrors::EntityFixupIdentityConflict);
            } catch (const std::bad_alloc &) {
                return Failure<void>(WorldStreamingErrors::EntityFixupCapacityExceeded);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateOwner(const StreamingRuntimeOwnerToken &owner, const StreamingRuntimeOwnerToken &expected) {
            if (!owner.IsValid() || !expected.IsValid())
                return Failure<void>(WorldStreamingErrors::EntityFixupInvalid);
            if (owner != expected)
                return Failure<void>(WorldStreamingErrors::EntityFixupStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const WorldEntityReferenceFixupRequest &request) {
            if (!request.reference.source.IsValid() || !request.reference.target.IsValid() ||
                request.reference.source.address == request.reference.target.address || !request.revision.IsValid() ||
                (request.expectedRevision.has_value() && !request.expectedRevision->IsValid()))
                return Failure<void>(WorldStreamingErrors::EntityFixupInvalid);
            if (!IsKnown(request.reference.kind))
                return Failure<void>(WorldStreamingErrors::EntityFixupUnsupported);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateActiveRequest(const StreamingRuntimeOwnerToken &owner,
                                                         const StreamingRuntimeOwnerToken &expectedOwner,
                                                         const WorldEntityReferenceFixupLedgerLifecycle lifecycle,
                                                         const WorldEntityReferenceFixupRequest &request) {
            if (const auto valid = ValidateOwner(owner, expectedOwner); valid.HasError())
                return valid;
            if (lifecycle != WorldEntityReferenceFixupLedgerLifecycle::Active)
                return Failure<void>(WorldStreamingErrors::EntityFixupLifecycleUnavailable);
            return ValidateRequest(request);
        }

        [[nodiscard]] Result<void> ValidateSoftKind(const WorldEntityReferenceFixupRequest &request) {
            if (request.reference.kind != WorldDependencyKind::Soft)
                return Failure<void>(WorldStreamingErrors::EntityFixupUnsupported);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSoftRequest(const WorldEntityReferenceFixupRequest &request) {
            if (const auto valid = ValidateRequest(request); valid.HasError())
                return valid;
            return ValidateSoftKind(request);
        }

        [[nodiscard]] Result<void> ValidateActiveSoftRequest(const StreamingRuntimeOwnerToken &owner,
                                                             const StreamingRuntimeOwnerToken &expectedOwner,
                                                             const WorldEntityReferenceFixupLedgerLifecycle lifecycle,
                                                             const WorldEntityReferenceFixupRequest &request) {
            if (const auto valid = ValidateActiveRequest(owner, expectedOwner, lifecycle, request); valid.HasError())
                return valid;
            return ValidateSoftKind(request);
        }

        [[nodiscard]] Result<WorldEntityReferenceFixupResult> MakeResult(const WorldEntityReferenceFixupRequest &request,
                                                                         const WorldEntityReferenceFixupDisposition disposition,
                                                                         const WorldEntityReferenceFixupMutation mutation,
                                                                         const std::optional<WorldRuntimeEntityId> runtime = std::nullopt) {
            WorldEntityReferenceFixupResult result{request, disposition, mutation, runtime};
            if (!result.IsValid())
                return Failure<WorldEntityReferenceFixupResult>(WorldStreamingErrors::EntityFixupInvalid);
            return Result<WorldEntityReferenceFixupResult>::Success(std::move(result));
        }

        [[nodiscard]] Result<std::size_t> FindExactPending(const std::vector<WorldEntityReferenceFixupRequest> &pending,
                                                           const WorldEntityReferenceFixupRequest &request) {
            const auto found = FindPending(pending, request);
            if (found == pending.end())
                return Failure<std::size_t>(WorldStreamingErrors::EntityFixupStale);
            if (found->reference != request.reference || found->revision != request.revision)
                return Failure<std::size_t>(WorldStreamingErrors::EntityFixupStale);
            return Result<std::size_t>::Success(static_cast<std::size_t>(std::distance(pending.begin(), found)));
        }

        struct SubmissionPlan final {
            std::optional<std::size_t> pendingIndex;
            std::optional<WorldRuntimeEntityId> targetRuntime{};
        };

        [[nodiscard]] bool IsResolvedMutation(const WorldEntityReferenceFixupMutation mutation) noexcept {
            using enum WorldEntityReferenceFixupMutation;
            return mutation == Resolved || mutation == Replaced || mutation == Activated;
        }

        [[nodiscard]] bool IsDeferredMutation(const WorldEntityReferenceFixupMutation mutation) noexcept {
            return mutation == WorldEntityReferenceFixupMutation::Deferred || mutation == WorldEntityReferenceFixupMutation::Replaced;
        }

        [[nodiscard]] bool IsResultPayloadValid(const WorldEntityReferenceFixupDisposition disposition,
                                                const WorldEntityReferenceFixupMutation mutation,
                                                const std::optional<WorldRuntimeEntityId> &runtime) noexcept {
            switch (disposition) {
                case WorldEntityReferenceFixupDisposition::Resolved:
                    return runtime.has_value() && runtime->IsValid() && IsResolvedMutation(mutation);
                case WorldEntityReferenceFixupDisposition::Deferred:
                    return !runtime.has_value() && IsDeferredMutation(mutation);
                case WorldEntityReferenceFixupDisposition::Cancelled:
                    return !runtime.has_value() && mutation == WorldEntityReferenceFixupMutation::Cancelled;
                case WorldEntityReferenceFixupDisposition::Failed:
                    return !runtime.has_value() && mutation == WorldEntityReferenceFixupMutation::Failed;
            }
            return false;
        }

        [[nodiscard]] Result<std::optional<WorldRuntimeEntityId>> ResolveActivationTarget(
            const WorldEntityReferenceFixupRequest &request, const std::span<const WorldEntityReferenceBinding> activeMappings,
            const std::uint32_t maximumActivationMappings) {
            if (const auto valid = ValidateMappings(activeMappings, maximumActivationMappings); valid.HasError())
                return Result<std::optional<WorldRuntimeEntityId>>::Failure(valid.ErrorValue());
            const auto *source = FindBinding(activeMappings, request.reference.source.address);
            if (source == nullptr)
                return Failure<std::optional<WorldRuntimeEntityId>>(WorldStreamingErrors::EntityFixupSourceUnavailable);
            if (source->endpoint.revision != request.reference.source.revision)
                return Failure<std::optional<WorldRuntimeEntityId>>(WorldStreamingErrors::EntityFixupStale);
            const auto *target = FindBinding(activeMappings, request.reference.target.address);
            if (target != nullptr && target->endpoint.revision != request.reference.target.revision)
                return Failure<std::optional<WorldRuntimeEntityId>>(WorldStreamingErrors::EntityFixupStale);
            return Result<std::optional<WorldRuntimeEntityId>>::Success(
                target == nullptr ? std::nullopt : std::optional<WorldRuntimeEntityId>{target->runtime});
        }

        [[nodiscard]] Result<std::optional<std::size_t>> ValidateRevisionAdmission(
            const std::vector<WorldEntityReferenceFixupRequest> &pending, const WorldEntityReferenceFixupRequest &request) {
            const auto existing = FindPending(pending, request);
            if (existing == pending.end()) {
                if (request.expectedRevision.has_value())
                    return Failure<std::optional<std::size_t>>(WorldStreamingErrors::EntityFixupStale);
                return Result<std::optional<std::size_t>>::Success(std::nullopt);
            }
            if (existing->reference.kind != request.reference.kind || !request.expectedRevision.has_value() ||
                *request.expectedRevision != existing->revision)
                return Failure<std::optional<std::size_t>>(WorldStreamingErrors::EntityFixupStale);
            const auto next = NextWorldEntityReferenceFixupRevision(existing->revision);
            if (next.HasError())
                return Result<std::optional<std::size_t>>::Failure(next.ErrorValue());
            if (request.revision != next.Value())
                return Failure<std::optional<std::size_t>>(WorldStreamingErrors::EntityFixupStale);
            return Result<std::optional<std::size_t>>::Success(static_cast<std::size_t>(std::distance(pending.begin(), existing)));
        }

        [[nodiscard]] Result<SubmissionPlan> PrepareSubmission(const WorldEntityReferenceFixupRequest &request,
                                                               const std::span<const WorldEntityReferenceBinding> activeMappings,
                                                               const std::vector<WorldEntityReferenceFixupRequest> &pending,
                                                               const WorldEntityReferenceFixupLimits limits) {
            auto target = ResolveActivationTarget(request, activeMappings, limits.maximumActivationMappings);
            if (target.HasError())
                return Result<SubmissionPlan>::Failure(target.ErrorValue());
            auto revision = ValidateRevisionAdmission(pending, request);
            if (revision.HasError())
                return Result<SubmissionPlan>::Failure(revision.ErrorValue());
            if (!target.Value().has_value() && request.reference.kind == WorldDependencyKind::Hard)
                return Failure<SubmissionPlan>(WorldStreamingErrors::EntityFixupTargetUnavailable);
            if (!target.Value().has_value() && !revision.Value().has_value() && pending.size() >= limits.maximumPendingReferences)
                return Failure<SubmissionPlan>(WorldStreamingErrors::EntityFixupCapacityExceeded);
            return Result<SubmissionPlan>::Success({revision.Value(), target.Value()});
        }
    }  // namespace

    /** @copydoc WorldEntityReferenceFixupRequest::IsValid */
    bool WorldEntityReferenceFixupRequest::IsValid() const noexcept {
        return reference.source.IsValid() && reference.target.IsValid() && reference.source.address != reference.target.address &&
               IsKnown(reference.kind) && revision.IsValid() && (!expectedRevision.has_value() || expectedRevision->IsValid());
    }

    /** @copydoc WorldEntityReferenceFixupResult::IsValid */
    bool WorldEntityReferenceFixupResult::IsValid() const noexcept {
        return request.IsValid() && IsKnown(disposition) && IsKnown(mutation) && IsResultPayloadValid(disposition, mutation, runtime);
    }

    WorldEntityReferenceFixupLedger::WorldEntityReferenceFixupLedger(const StreamingRuntimeOwnerToken &owner,
                                                                     const WorldEntityReferenceFixupLimits limits,
                                                                     std::vector<WorldEntityReferenceFixupRequest> pending) noexcept
        : owner_(owner), limits_(limits), pending_(std::move(pending)) {}

    /** @copydoc WorldEntityReferenceFixupLedger::WorldEntityReferenceFixupLedger */
    WorldEntityReferenceFixupLedger::WorldEntityReferenceFixupLedger(WorldEntityReferenceFixupLedger &&other) noexcept
        : owner_(other.owner_), limits_(other.limits_), pending_(std::move(other.pending_)), lifecycle_(other.lifecycle_) {
        other.lifecycle_ = WorldEntityReferenceFixupLedgerLifecycle::Closed;
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Create */
    Result<WorldEntityReferenceFixupLedger> WorldEntityReferenceFixupLedger::Create(const StreamingRuntimeOwnerToken &owner,
                                                                                    const WorldEntityReferenceFixupLimits limits) {
        if (!owner.IsValid() || !limits.IsValid())
            return Failure<WorldEntityReferenceFixupLedger>(WorldStreamingErrors::EntityFixupInvalid);
        try {
            std::vector<WorldEntityReferenceFixupRequest> pending;
            pending.reserve(limits.maximumPendingReferences);
            return Result<WorldEntityReferenceFixupLedger>::Success(WorldEntityReferenceFixupLedger{owner, limits, std::move(pending)});
        } catch (const std::bad_alloc &) {
            return Failure<WorldEntityReferenceFixupLedger>(WorldStreamingErrors::EntityFixupCapacityExceeded);
        } catch (const std::length_error &) {
            return Failure<WorldEntityReferenceFixupLedger>(WorldStreamingErrors::EntityFixupCapacityExceeded);
        }
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Submit */
    Result<WorldEntityReferenceFixupResult> WorldEntityReferenceFixupLedger::Submit(
        const StreamingRuntimeOwnerToken &owner, const WorldEntityReferenceFixupRequest &request,
        const std::span<const WorldEntityReferenceBinding> activeMappings) {
        if (const auto valid = ValidateActiveRequest(owner, owner_, Lifecycle(), request); valid.HasError())
            return Result<WorldEntityReferenceFixupResult>::Failure(valid.ErrorValue());
        auto plan = PrepareSubmission(request, activeMappings, pending_, limits_);
        if (plan.HasError())
            return Result<WorldEntityReferenceFixupResult>::Failure(plan.ErrorValue());
        if (!plan.Value().targetRuntime.has_value()) {
            auto published = request;
            published.expectedRevision.reset();
            if (plan.Value().pendingIndex.has_value())
                pending_[*plan.Value().pendingIndex] = published;
            else {
                const auto insertion = std::ranges::lower_bound(pending_, published, LessRequest);
                pending_.insert(insertion, published);
            }
            return MakeResult(request, WorldEntityReferenceFixupDisposition::Deferred,
                              plan.Value().pendingIndex.has_value() ? WorldEntityReferenceFixupMutation::Replaced
                                                                    : WorldEntityReferenceFixupMutation::Deferred);
        }

        if (plan.Value().pendingIndex.has_value())
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(*plan.Value().pendingIndex));
        return MakeResult(request, WorldEntityReferenceFixupDisposition::Resolved,
                          plan.Value().pendingIndex.has_value() ? WorldEntityReferenceFixupMutation::Replaced
                                                                : WorldEntityReferenceFixupMutation::Resolved,
                          plan.Value().targetRuntime);
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Activate */
    Result<WorldEntityReferenceFixupResult> WorldEntityReferenceFixupLedger::Activate(const StreamingRuntimeOwnerToken &owner,
                                                                                      const WorldEntityReferenceFixupRequest &request,
                                                                                      const WorldEntityReferenceBinding &target) {
        if (const auto valid = ValidateActiveSoftRequest(owner, owner_, Lifecycle(), request); valid.HasError())
            return Result<WorldEntityReferenceFixupResult>::Failure(valid.ErrorValue());
        if (!target.IsValid())
            return Failure<WorldEntityReferenceFixupResult>(WorldStreamingErrors::EntityFixupInvalid);
        if (target.endpoint.address != request.reference.target.address || target.endpoint.revision != request.reference.target.revision)
            return Failure<WorldEntityReferenceFixupResult>(WorldStreamingErrors::EntityFixupStale);
        const auto exact = FindExactPending(pending_, request);
        if (exact.HasError())
            return Result<WorldEntityReferenceFixupResult>::Failure(exact.ErrorValue());
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(exact.Value()));
        return MakeResult(request, WorldEntityReferenceFixupDisposition::Resolved, WorldEntityReferenceFixupMutation::Activated,
                          target.runtime);
    }

    namespace {
        [[nodiscard]] Result<WorldEntityReferenceFixupResult> Drain(std::vector<WorldEntityReferenceFixupRequest> &pending,
                                                                    const StreamingRuntimeOwnerToken &owner,
                                                                    const StreamingRuntimeOwnerToken &expected,
                                                                    const WorldEntityReferenceFixupRequest &request,
                                                                    const WorldEntityReferenceFixupDisposition disposition,
                                                                    const WorldEntityReferenceFixupMutation mutation) {
            if (const auto valid = ValidateOwner(owner, expected); valid.HasError())
                return Result<WorldEntityReferenceFixupResult>::Failure(valid.ErrorValue());
            if (const auto valid = ValidateSoftRequest(request); valid.HasError())
                return Result<WorldEntityReferenceFixupResult>::Failure(valid.ErrorValue());
            const auto exact = FindExactPending(pending, request);
            if (exact.HasError())
                return Result<WorldEntityReferenceFixupResult>::Failure(exact.ErrorValue());
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(exact.Value()));
            return MakeResult(request, disposition, mutation);
        }
    }  // namespace

    /** @copydoc WorldEntityReferenceFixupLedger::Cancel */
    Result<WorldEntityReferenceFixupResult> WorldEntityReferenceFixupLedger::Cancel(const StreamingRuntimeOwnerToken &owner,
                                                                                    const WorldEntityReferenceFixupRequest &request) {
        if (Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Closed)
            return Failure<WorldEntityReferenceFixupResult>(WorldStreamingErrors::EntityFixupLifecycleUnavailable);
        return Drain(pending_, owner, owner_, request, WorldEntityReferenceFixupDisposition::Cancelled,
                     WorldEntityReferenceFixupMutation::Cancelled);
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Fail */
    Result<WorldEntityReferenceFixupResult> WorldEntityReferenceFixupLedger::Fail(const StreamingRuntimeOwnerToken &owner,
                                                                                  const WorldEntityReferenceFixupRequest &request) {
        if (Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Closed)
            return Failure<WorldEntityReferenceFixupResult>(WorldStreamingErrors::EntityFixupLifecycleUnavailable);
        return Drain(pending_, owner, owner_, request, WorldEntityReferenceFixupDisposition::Failed,
                     WorldEntityReferenceFixupMutation::Failed);
    }

    /** @copydoc WorldEntityReferenceFixupLedger::RequestCancellation */
    Result<void> WorldEntityReferenceFixupLedger::RequestCancellation(const StreamingRuntimeOwnerToken &owner) noexcept {
        if (const auto valid = ValidateOwner(owner, owner_); valid.HasError())
            return valid;
        if (Lifecycle() != WorldEntityReferenceFixupLedgerLifecycle::Closed)
            lifecycle_ = WorldEntityReferenceFixupLedgerLifecycle::Cancelling;
        return Result<void>::Success();
    }

    /** @copydoc WorldEntityReferenceFixupLedger::BeginShutdown */
    Result<void> WorldEntityReferenceFixupLedger::BeginShutdown(const StreamingRuntimeOwnerToken &owner) noexcept {
        if (!owner.IsValid() || !owner_.IsValid())
            return Failure<void>(WorldStreamingErrors::EntityFixupInvalid);
        if (owner != owner_)
            return Failure<void>(WorldStreamingErrors::EntityFixupStale);
        lifecycle_ = WorldEntityReferenceFixupLedgerLifecycle::Cancelling;
        return Result<void>::Success();
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Owner */
    const StreamingRuntimeOwnerToken &WorldEntityReferenceFixupLedger::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Lifecycle */
    WorldEntityReferenceFixupLedgerLifecycle WorldEntityReferenceFixupLedger::Lifecycle() const noexcept {
        if (lifecycle_ == WorldEntityReferenceFixupLedgerLifecycle::Cancelling && pending_.empty())
            return WorldEntityReferenceFixupLedgerLifecycle::Closed;
        return lifecycle_;
    }

    /** @copydoc WorldEntityReferenceFixupLedger::Pending */
    std::span<const WorldEntityReferenceFixupRequest> WorldEntityReferenceFixupLedger::Pending() const noexcept {
        return pending_;
    }

    /** @copydoc NextWorldEntityReferenceFixupRevision */
    Result<WorldEntityReferenceFixupRevision> NextWorldEntityReferenceFixupRevision(const WorldEntityReferenceFixupRevision current) {
        if (!current.IsValid())
            return Failure<WorldEntityReferenceFixupRevision>(WorldStreamingErrors::IdentityInvalid);
        if (current.Value() == std::numeric_limits<std::uint64_t>::max())
            return Failure<WorldEntityReferenceFixupRevision>(WorldStreamingErrors::GenerationExhausted);
        return WorldEntityReferenceFixupRevision::Create(current.Value() + 1);
    }
}  // namespace Horo::WorldStreaming
