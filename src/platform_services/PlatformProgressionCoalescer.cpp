#include "Horo/PlatformServices/PlatformProgressionCoalescer.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::PlatformServices {
    namespace {
        /** @brief Compares only matching numeric alternatives; no signed/unsigned conversion occurs. */
        bool Stronger(const PlatformProgressionMutationEnvelope &candidate, const PlatformProgressionMutationEnvelope &current,
                      const LeaderboardOrdering ordering) {
            const bool minimum =
                candidate.kind == PlatformProgressionMutationKind::SetStatMinimum ||
                (candidate.kind == PlatformProgressionMutationKind::SubmitBestScore && ordering == LeaderboardOrdering::LowestFirst);
            if (minimum)
                return candidate.value < current.value;
            return candidate.value > current.value;
        }

        /** @brief Conditional and additive operations cannot share remote dispatch. */
        bool CanCombine(const PlatformProgressionMutationEnvelope &left, const PlatformProgressionMutationEnvelope &right) {
            using enum PlatformProgressionMutationKind;
            return left.kind != SetStatSnapshot && left.kind != AddStatOnce && left.kind != ReplaceScoreAtRevision &&
                   left.scope == right.scope && left.definitionKind == right.definitionKind && left.definition == right.definition &&
                   left.kind == right.kind && left.policy == right.policy && left.authority == right.authority &&
                   left.value.index() == right.value.index();
        }

        /** @brief Serializes one semantic target even across policy revisions. */
        bool SameTarget(const PlatformProgressionMutationEnvelope &left, const PlatformProgressionMutationEnvelope &right) {
            return left.scope == right.scope && left.definitionKind == right.definitionKind && left.definition == right.definition;
        }
    }  // namespace

    struct PlatformProgressionCoalescer::Impl final {
        struct Receipt final {
            PlatformProgressionMutationEnvelope envelope;
            LeaderboardOrdering ordering;
            std::uint64_t admittedTick{};
            std::uint64_t batch{};
            PlatformProgressionOutcome outcome{PlatformProgressionOutcome::Pending};
            std::optional<PlatformProviderFailureCategory> failure;
        };

        struct Batch final {
            PlatformProgressionDispatch dispatch;
        };

        explicit Impl(PlatformProgressionCoalescerConfig value) : config(value) {
            receipts.reserve(config.maximumReceipts);
            batches.reserve(config.maximumReceipts);
        }

        /** @brief Unknown remote effects and active attempts fence the exact target until conclusive completion. */
        [[nodiscard]] bool IsBlocked(const Receipt &first) const {
            return std::ranges::any_of(receipts, [&first](const auto &other) {
                return (other.outcome == PlatformProgressionOutcome::InFlight ||
                        other.outcome == PlatformProgressionOutcome::RemoteOutcomeUnknown) &&
                       SameTarget(first.envelope, other.envelope);
            });
        }

        /** @brief Only pending compatible members of the original fixed window can enter a dispatch. */
        [[nodiscard]] bool CanJoin(const Receipt &first, const Receipt &member) const {
            return member.outcome == PlatformProgressionOutcome::Pending && member.admittedTick >= first.admittedTick &&
                   member.admittedTick - first.admittedTick < config.windowTicks && member.ordering == first.ordering &&
                   (&member == &first || CanCombine(first.envelope, member.envelope));
        }

        /** @brief Freezes one strongest original envelope; preallocated storage makes the state change allocation-free. */
        [[nodiscard]] PlatformProgressionDispatch Dispatch(Receipt &first) {
            auto strongest = first.envelope;
            const auto next = ++sequence;
            for (auto &member : receipts) {
                if (!CanJoin(first, member))
                    continue;
                member.batch = next;
                member.outcome = PlatformProgressionOutcome::InFlight;
                if (Stronger(member.envelope, strongest, first.ordering))
                    strongest = member.envelope;
            }
            PlatformProgressionDispatch dispatch{next, strongest};
            batches.push_back({dispatch});
            return dispatch;
        }

        PlatformProgressionCoalescerConfig config;
        mutable std::mutex mutex; /**< Protects all receipts, dispatch identity, monotonic clock and closed state. */
        std::vector<Receipt> receipts;
        std::vector<Batch> batches;
        std::uint64_t tick{};
        std::uint64_t sequence{};
        bool closed{};
    };

    PlatformProgressionCoalescer::PlatformProgressionCoalescer(const PlatformProgressionCoalescerConfig config)
        : impl_(std::make_unique<Impl>(config)) {}

    PlatformProgressionCoalescer::~PlatformProgressionCoalescer() = default;

    /** @copydoc PlatformProgressionCoalescer::Create */
    Result<std::unique_ptr<PlatformProgressionCoalescer>> PlatformProgressionCoalescer::Create(
        const PlatformProgressionCoalescerConfig config) {
        if (config.maximumReceipts == 0 || config.maximumReceipts > PlatformProgressionMaximumInFlightMutations || config.windowTicks == 0)
            return Result<std::unique_ptr<PlatformProgressionCoalescer>>::Failure(
                MakeError(PlatformProgressionErrors::InvalidConfiguration));
        try {
            return Result<std::unique_ptr<PlatformProgressionCoalescer>>::Success(
                std::unique_ptr<PlatformProgressionCoalescer>(new PlatformProgressionCoalescer(config)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<PlatformProgressionCoalescer>>::Failure(MakeError(PlatformProgressionErrors::CapacityExceeded));
        }
    }

    /** @copydoc PlatformProgressionCoalescer::Admit */
    Result<PlatformProgressionAdmission> PlatformProgressionCoalescer::Admit(const PlatformProgressionMutationEnvelope &envelope,
                                                                             const LeaderboardOrdering ordering, const std::uint64_t tick) {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closed)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::Closed));
        if (!envelope.IsValid() || tick < impl_->tick ||
            (ordering != LeaderboardOrdering::HighestFirst && ordering != LeaderboardOrdering::LowestFirst))
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        for (const auto &receipt : impl_->receipts) {
            if (receipt.envelope.mutation != envelope.mutation)
                continue;
            if (receipt.envelope != envelope || receipt.ordering != ordering)
                return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::IdempotencyConflict));
            impl_->tick = tick;
            return Result<PlatformProgressionAdmission>::Success(
                {envelope.mutation, PlatformProgressionAdmissionDisposition::JoinedExisting});
        }
        if (impl_->receipts.size() == impl_->config.maximumReceipts)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::CapacityExceeded));
        impl_->tick = tick;
        impl_->receipts.push_back({envelope, ordering, tick});
        return Result<PlatformProgressionAdmission>::Success({envelope.mutation, PlatformProgressionAdmissionDisposition::Started});
    }

    /** @copydoc PlatformProgressionCoalescer::TakeReady */
    Result<std::optional<PlatformProgressionDispatch>> PlatformProgressionCoalescer::TakeReady(const std::uint64_t tick) {
        using Return = Result<std::optional<PlatformProgressionDispatch>>;
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closed)
            return Return::Failure(MakeError(PlatformProgressionErrors::Closed));
        if (tick < impl_->tick || impl_->sequence == std::numeric_limits<std::uint64_t>::max())
            return Return::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        impl_->tick = tick;
        for (auto &first : impl_->receipts) {
            if (first.outcome != PlatformProgressionOutcome::Pending)
                continue;
            if (impl_->IsBlocked(first) || tick - first.admittedTick < impl_->config.windowTicks)
                continue;
            return Return::Success(impl_->Dispatch(first));
        }
        return Return::Success(std::nullopt);
    }

    /** @copydoc PlatformProgressionCoalescer::Complete */
    Result<void> PlatformProgressionCoalescer::Complete(const PlatformProgressionDispatch &dispatch,
                                                        const PlatformProgressionOutcome outcome,
                                                        const std::optional<PlatformProviderFailureCategory> failure) {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closed)
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::Closed));
        if (outcome != PlatformProgressionOutcome::Succeeded && outcome != PlatformProgressionOutcome::Failed &&
            outcome != PlatformProgressionOutcome::RemoteOutcomeUnknown)
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        if ((failure && outcome != PlatformProgressionOutcome::Failed) ||
            (failure && static_cast<unsigned>(*failure) > static_cast<unsigned>(PlatformProviderFailureCategory::Unknown)))
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        const auto batch = std::ranges::find_if(impl_->batches, [&dispatch](const auto &value) {
            return value.dispatch == dispatch;
        });
        if (batch == impl_->batches.end())
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        for (auto &receipt : impl_->receipts) {
            if (receipt.batch == dispatch.sequence) {
                receipt.outcome = outcome;
                receipt.failure = outcome == PlatformProgressionOutcome::Failed
                                      ? std::optional{failure.value_or(PlatformProviderFailureCategory::Unknown)}
                                      : std::nullopt;
            }
        }
        if (outcome != PlatformProgressionOutcome::RemoteOutcomeUnknown)
            impl_->batches.erase(batch);
        return Result<void>::Success();
    }

    /** @copydoc PlatformProgressionCoalescer::Cancel */
    Result<void> PlatformProgressionCoalescer::Cancel(const PlatformProgressionMutationId mutation) {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closed)
            return Result<void>::Failure(MakeError(PlatformProgressionErrors::Closed));
        for (auto &receipt : impl_->receipts) {
            if (receipt.envelope.mutation == mutation && receipt.outcome == PlatformProgressionOutcome::Pending) {
                receipt.outcome = PlatformProgressionOutcome::Cancelled;
                return Result<void>::Success();
            }
        }
        return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
    }

    /** @copydoc PlatformProgressionCoalescer::Observe */
    std::optional<PlatformProgressionOutcome> PlatformProgressionCoalescer::Observe(const PlatformProgressionMutationId mutation) const {
        const std::lock_guard lock(impl_->mutex);
        for (const auto &receipt : impl_->receipts) {
            if (receipt.envelope.mutation == mutation)
                return receipt.outcome;
        }
        return std::nullopt;
    }

    /** @copydoc PlatformProgressionCoalescer::FailureCategory */
    std::optional<PlatformProviderFailureCategory> PlatformProgressionCoalescer::FailureCategory(
        const PlatformProgressionMutationId mutation) const {
        const std::lock_guard lock(impl_->mutex);
        for (const auto &receipt : impl_->receipts) {
            if (receipt.envelope.mutation == mutation)
                return receipt.failure;
        }
        return std::nullopt;
    }

    /** @copydoc PlatformProgressionCoalescer::Query */
    std::optional<PlatformProgressionReceiptSnapshot> PlatformProgressionCoalescer::Query(
        const PlatformProgressionMutationId mutation, const PlatformProgressionSessionScope &scope) const {
        const std::lock_guard lock(impl_->mutex);
        for (const auto &receipt : impl_->receipts) {
            if (receipt.envelope.mutation == mutation && receipt.envelope.scope == scope)
                return PlatformProgressionReceiptSnapshot{receipt.outcome, receipt.failure};
        }
        return std::nullopt;
    }

    /** @copydoc PlatformProgressionCoalescer::Close */
    void PlatformProgressionCoalescer::Close() {
        const std::lock_guard lock(impl_->mutex);
        impl_->closed = true;
        for (auto &receipt : impl_->receipts) {
            if (receipt.outcome == PlatformProgressionOutcome::Pending)
                receipt.outcome = PlatformProgressionOutcome::Cancelled;
            else if (receipt.outcome == PlatformProgressionOutcome::InFlight)
                receipt.outcome = PlatformProgressionOutcome::RemoteOutcomeUnknown;
        }
        impl_->batches.clear();
    }
}  // namespace Horo::PlatformServices
