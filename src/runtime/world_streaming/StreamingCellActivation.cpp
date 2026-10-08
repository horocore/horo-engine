#include "Horo/WorldStreaming/StreamingCellActivation.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        [[nodiscard]] bool IsKnown(const StreamingCellActivationLifecycle value) noexcept {
            return value < StreamingCellActivationLifecycle::Count;
        }

        [[nodiscard]] bool IsKnown(const StreamingCellActivationCommitPoint value) noexcept {
            return value < StreamingCellActivationCommitPoint::Count;
        }

        [[nodiscard]] bool IsValid(const StreamingCellActivationRequirement &value) noexcept {
            return value.participant.IsValid() && value.revision.IsValid();
        }

        void RollbackReceipts(std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> &receipts) noexcept {
            for (auto iterator = receipts.rbegin(); iterator != receipts.rend(); ++iterator) {
                if (*iterator)
                    (*iterator)->RollbackPrepared();
            }
        }

        [[nodiscard]] bool RequirementLess(const StreamingCellActivationRequirement &left,
                                           const StreamingCellActivationRequirement &right) noexcept {
            return left.participant < right.participant;
        }

        [[nodiscard]] Result<void> ValidateContext(const StreamingCellActivationContext &context) {
            if (!context.activation.IsValid() || !context.operation.Handle().IsValid() || context.maximumReceipts == 0 ||
                !IsKnown(context.lifecycle) || !context.scheduler.IsValid())
                return Internal::Failure<void>(WorldStreamingErrors::CellActivationInvalid);
            if (context.operation.Kind() != StreamingCellOperationKind::Activate ||
                context.operation.State() != StreamingCellOperationState::Activating)
                return Internal::Failure<void>(WorldStreamingErrors::CellActivationInvalid);
            if (context.lifecycle != StreamingCellActivationLifecycle::Active)
                return Internal::Failure<void>(WorldStreamingErrors::CellActivationLifecycleUnavailable);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<StreamingCellActivationRequirement>> CanonicalizeRequirements(
            const std::span<const StreamingCellActivationRequirement> required, const std::size_t receiptCount,
            const std::size_t maximumReceipts) {
            if (required.empty())
                return Internal::Failure<std::vector<StreamingCellActivationRequirement>>(WorldStreamingErrors::CellActivationIncomplete);
            if (required.size() > maximumReceipts || receiptCount > maximumReceipts)
                return Internal::Failure<std::vector<StreamingCellActivationRequirement>>(
                    WorldStreamingErrors::CellActivationCapacityExceeded);
            if (required.size() != receiptCount)
                return Internal::Failure<std::vector<StreamingCellActivationRequirement>>(WorldStreamingErrors::CellActivationIncomplete);

            std::vector<StreamingCellActivationRequirement> canonical{required.begin(), required.end()};
            std::ranges::sort(canonical, RequirementLess);
            if (std::ranges::any_of(canonical, [](const auto &requirement) {
                return !IsValid(requirement);
            }))
                return Internal::Failure<std::vector<StreamingCellActivationRequirement>>(WorldStreamingErrors::CellActivationInvalid);
            if (std::ranges::adjacent_find(canonical, {}, &StreamingCellActivationRequirement::participant) != canonical.end())
                return Internal::Failure<std::vector<StreamingCellActivationRequirement>>(WorldStreamingErrors::CellActivationInvalid);
            return Result<std::vector<StreamingCellActivationRequirement>>::Success(std::move(canonical));
        }

        [[nodiscard]] Result<void> ValidateReceipts(const StreamingCellOperationHandle &operation,
                                                    const std::span<const StreamingCellActivationRequirement> required,
                                                    std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> &receipts) {
            std::ranges::sort(receipts, [](const auto &left, const auto &right) {
                if (!left)
                    return static_cast<bool>(right);
                if (!right)
                    return false;
                return left->Requirement().participant < right->Requirement().participant;
            });
            for (std::size_t index{}; index < receipts.size(); ++index) {
                if (!receipts[index] || !IsValid(receipts[index]->Requirement()))
                    return Internal::Failure<void>(WorldStreamingErrors::CellActivationInvalid);
                if (receipts[index]->Operation() != operation)
                    return Internal::Failure<void>(WorldStreamingErrors::CellActivationStale);
                if (receipts[index]->Requirement().participant != required[index].participant)
                    return Internal::Failure<void>(WorldStreamingErrors::CellActivationIncomplete);
                if (receipts[index]->Requirement().revision != required[index].revision)
                    return Internal::Failure<void>(WorldStreamingErrors::CellActivationStale);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc StreamingCellActivationTransaction::~StreamingCellActivationTransaction */
    StreamingCellActivationTransaction::~StreamingCellActivationTransaction() {
        Rollback();
    }

    /** @copydoc StreamingCellActivationTransaction::StreamingCellActivationTransaction */
    StreamingCellActivationTransaction::StreamingCellActivationTransaction(StreamingCellActivationTransaction &&other) noexcept
        : context_(other.context_), requirements_(std::move(other.requirements_)), receipts_(std::move(other.receipts_)),
          publicationNanoseconds_(other.publicationNanoseconds_), state_(other.state_) {
        other.state_ = StreamingCellActivationState::RolledBack;
    }

    /** @copydoc StreamingCellActivationTransaction::operator= */
    StreamingCellActivationTransaction &StreamingCellActivationTransaction::operator=(StreamingCellActivationTransaction &&other) noexcept {
        if (this == &other)
            return *this;
        Rollback();
        context_ = other.context_;
        requirements_ = std::move(other.requirements_);
        receipts_ = std::move(other.receipts_);
        publicationNanoseconds_ = other.publicationNanoseconds_;
        state_ = other.state_;
        other.state_ = StreamingCellActivationState::RolledBack;
        return *this;
    }

    /** @copydoc StreamingCellActivationTransaction::Prepare */
    Result<StreamingCellActivationTransaction> StreamingCellActivationTransaction::Prepare(
        const StreamingCellActivationContext &context, const std::span<const StreamingCellActivationRequirement> required,
        std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts) {
        if (const auto valid = ValidateContext(context); valid.HasError()) {
            RollbackReceipts(receipts);
            return Result<StreamingCellActivationTransaction>::Failure(valid.ErrorValue());
        }
        auto canonical = CanonicalizeRequirements(required, receipts.size(), context.maximumReceipts);
        if (canonical.HasError()) {
            RollbackReceipts(receipts);
            return Result<StreamingCellActivationTransaction>::Failure(canonical.ErrorValue());
        }
        if (const auto valid = ValidateReceipts(context.operation.Handle(), canonical.Value(), receipts); valid.HasError()) {
            RollbackReceipts(receipts);
            return Result<StreamingCellActivationTransaction>::Failure(valid.ErrorValue());
        }

        std::uint64_t publicationNanoseconds{};
        for (const auto &receipt : receipts) {
            const auto cost = receipt->MaximumPublicationNanoseconds();
            if (cost == 0 || cost > std::numeric_limits<std::uint64_t>::max() - publicationNanoseconds) {
                RollbackReceipts(receipts);
                return Internal::Failure<StreamingCellActivationTransaction>(WorldStreamingErrors::OwnerFrameInvalid);
            }
            publicationNanoseconds += cost;
        }
        return Result<StreamingCellActivationTransaction>::Success(
            StreamingCellActivationTransaction{context, std::move(canonical).Value(), std::move(receipts), publicationNanoseconds});
    }

    /** @copydoc StreamingCellActivationTransaction::Commit */
    Result<void> StreamingCellActivationTransaction::Commit(const StreamingCellOperation &expected,
                                                            const StreamingCellActivationCommitPoint commitPoint,
                                                            const StreamingCellActivationLifecycle lifecycle,
                                                            StreamingOwnerFrameBudget &budget, const std::uint64_t elapsedNanoseconds) {
        if (state_ != StreamingCellActivationState::Prepared)
            return Internal::Failure<void>(WorldStreamingErrors::CellActivationLifecycleUnavailable);
        if (!IsKnown(commitPoint))
            return Internal::Failure<void>(WorldStreamingErrors::CellActivationInvalid);
        if (commitPoint != StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges)
            return Internal::Failure<void>(WorldStreamingErrors::CellActivationSafePointUnavailable);
        if (!IsKnown(lifecycle) || lifecycle != StreamingCellActivationLifecycle::Active) {
            Rollback();
            return Internal::Failure<void>(WorldStreamingErrors::CellActivationLifecycleUnavailable);
        }
        if (!expected.Handle().IsValid() || expected.Handle() != context_.operation.Handle() ||
            expected.Kind() != context_.operation.Kind() || expected.State() != context_.operation.State() ||
            expected.Outcome() != context_.operation.Outcome()) {
            Rollback();
            return Internal::Failure<void>(WorldStreamingErrors::CellActivationStale);
        }

        const auto admitted = budget.TryConsume(context_.scheduler, publicationNanoseconds_, elapsedNanoseconds);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        if (!admitted.Value())
            return Internal::Failure<void>(WorldStreamingErrors::OwnerFrameDeferred);
        for (const auto &receipt : receipts_)
            receipt->PublishPrepared();
        state_ = StreamingCellActivationState::Published;
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellActivationTransaction::Rollback */
    void StreamingCellActivationTransaction::Rollback() noexcept {
        if (state_ != StreamingCellActivationState::Prepared)
            return;
        RollbackReceipts(receipts_);
        state_ = StreamingCellActivationState::RolledBack;
    }

    /** @copydoc StreamingCellActivationTransaction::Id */
    StreamingCellActivationId StreamingCellActivationTransaction::Id() const noexcept {
        return context_.activation;
    }

    /** @copydoc StreamingCellActivationTransaction::Operation */
    const StreamingCellOperationHandle &StreamingCellActivationTransaction::Operation() const noexcept {
        return context_.operation.Handle();
    }

    /** @copydoc StreamingCellActivationTransaction::State */
    StreamingCellActivationState StreamingCellActivationTransaction::State() const noexcept {
        return state_;
    }

    /** @copydoc StreamingCellActivationTransaction::Requirements */
    std::span<const StreamingCellActivationRequirement> StreamingCellActivationTransaction::Requirements() const noexcept {
        return requirements_;
    }

    StreamingCellActivationTransaction::StreamingCellActivationTransaction(
        const StreamingCellActivationContext &context, std::vector<StreamingCellActivationRequirement> requirements,
        std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts, const std::uint64_t publicationNanoseconds) noexcept
        : context_(context), requirements_(std::move(requirements)), receipts_(std::move(receipts)),
          publicationNanoseconds_(publicationNanoseconds), state_(StreamingCellActivationState::Prepared) {}
}  // namespace Horo::WorldStreaming
