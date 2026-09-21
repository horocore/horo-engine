#include "Horo/Runtime/Render/RenderQuery.h"

#include "BoundedRenderQueueCore.h"
#include "Horo/Runtime/Render/RenderQueryErrors.h"

#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error QueryError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsTerminal(const RenderTimestampQueryState state) noexcept {
            using enum RenderTimestampQueryState;
            return state == Failed || state == Cancelled || state == TimedOut;
        }

        [[nodiscard]] Error QueryAdmissionError(const detail::QueueAdmissionFailure failure) {
            using enum detail::QueueAdmissionFailure;
            switch (failure) {
                case Capacity:
                    return QueryError(RenderQueryErrors::CapacityExceeded);
                case Identity:
                    return QueryError(RenderQueryErrors::CapacityExceeded, "Timestamp-query identity space is exhausted.");
                case Allocation:
                    return QueryError(RenderQueryErrors::CapacityExceeded);
                case Length:
                    return QueryError(RenderQueryErrors::CapacityExceeded, "Timestamp-query metadata capacity cannot be represented.");
            }
            return QueryError(RenderQueryErrors::CapacityExceeded);
        }
    }  // namespace

    struct QueryRecord {
        RenderTimestampQueryId id;
        RenderTimestampQueryDescriptor descriptor;
        RenderTimestampQueryState state{RenderTimestampQueryState::Pending};
        RenderTimelinePoint completion;
        std::optional<std::chrono::nanoseconds> timestamp;
        std::optional<Error> failure;
        bool backendRetirementPending{false};
    };

    class RenderTimestampQueryQueue::Impl final {
        using enum RenderTimestampQueryState;
        using Core = detail::BoundedRenderQueueCore<QueryRecord, RenderTimestampQueryId, RenderTimestampQueryLimits>;
        using OperationAdapter = detail::QueueOperationAdapter<Core>;

    public:
        Impl(const RenderResourceOwnerId renderer, const bool supported, const RenderTimestampQueryLimits &limits)
            : core_(renderer, limits), operations_(core_, RenderQueryErrors::WrongThread, RenderQueryErrors::InvalidRequest,
                                                   "Timestamp-query identity is malformed, foreign, or no longer tracked."),
              supported_(supported) {}

        [[nodiscard]] Result<RenderTimestampQueryId> Request(const RenderTimestampQueryDescriptor &descriptor) {
            if (!core_.OnOwnerThread())
                return Result<RenderTimestampQueryId>::Failure(QueryError(RenderQueryErrors::WrongThread));
            if (!core_.Accepting())
                return Result<RenderTimestampQueryId>::Failure(QueryError(RenderQueryErrors::Stopped));
            if (!supported_)
                return Result<RenderTimestampQueryId>::Failure(QueryError(RenderQueryErrors::Unsupported));
            if (!descriptor.IsValid())
                return Result<RenderTimestampQueryId>::Failure(QueryError(RenderQueryErrors::InvalidDescriptor));
            return core_.Admit(true, [descriptor](const RenderTimestampQueryId id) {
                return QueryRecord{.id = id, .descriptor = descriptor};
            }, QueryAdmissionError);
        }

        [[nodiscard]] Result<RenderTimestampQueryState> State(const RenderTimestampQueryId query) const {
            return operations_.Apply<Result<RenderTimestampQueryState>>(query, [](const auto &record) {
                return Result<RenderTimestampQueryState>::Success(record.state);
            });
        }

        [[nodiscard]] Result<bool> IsReady(const RenderTimestampQueryId query) const {
            const auto state = State(query);
            return detail::QueueStateIsReady(state, Ready);
        }

        [[nodiscard]] Result<void> MarkSubmitted(const RenderTimestampQueryId query, const RenderTimelinePoint completion) {
            return operations_.Apply<Result<void>>(query, [completion, this](auto &record) {
                if (record.state != Pending || !completion.IsValid())
                    return InvalidTransition("Only a pending timestamp query may receive a valid completion point.");
                record.completion = completion;
                record.backendRetirementPending = true;
                record.state = Submitted;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Complete(const RenderTimestampQueryId query, const std::chrono::nanoseconds timestamp) {
            return operations_.Apply<Result<void>>(query, [this, timestamp](auto &record) {
                if (record.state == Cancelled || record.state == TimedOut) {
                    record.backendRetirementPending = false;
                    return Result<void>::Success();
                }
                if (record.state != Submitted || !record.completion.IsValid())
                    return InvalidTransition("Only submitted timestamp queries may complete.");
                if (timestamp.count() < 0)
                    return Result<void>::Failure(QueryError(RenderQueryErrors::TimestampInvalid));
                record.timestamp = timestamp;
                record.backendRetirementPending = false;
                record.state = Ready;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Fail(const RenderTimestampQueryId query, const Error &error) {
            return operations_.Apply<Result<void>>(query, [this, &error](auto &record) {
                if (record.state == Cancelled || record.state == TimedOut) {
                    record.backendRetirementPending = false;
                    return Result<void>::Success();
                }
                if (record.state != Pending && record.state != Submitted)
                    return InvalidTransition("Only pending or submitted timestamp queries may fail.");
                record.failure = error;
                record.backendRetirementPending = false;
                record.state = Failed;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Cancel(const RenderTimestampQueryId query) {
            return operations_.Apply<Result<void>>(query, [this](auto &record) {
                if (record.state == Cancelled)
                    return Result<void>::Success();
                if (record.state != Pending && record.state != Submitted)
                    return InvalidTransition("Only pending or submitted timestamp queries may be cancelled.");
                record.state = Cancelled;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Timeout(const RenderTimestampQueryId query) {
            return operations_.Apply<Result<void>>(query, [this](auto &record) {
                if (record.state == TimedOut)
                    return Result<void>::Success();
                if (record.state != Pending && record.state != Submitted)
                    return InvalidTransition("Only pending or submitted timestamp queries may time out.");
                record.state = TimedOut;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Retire(const RenderTimestampQueryId query) {
            return operations_.Apply<Result<void>>(query, [this](auto &record) {
                if (record.state != Cancelled && record.state != TimedOut)
                    return InvalidTransition("Only cancelled or timed-out timestamp queries may retire without publication.");
                record.backendRetirementPending = false;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<RenderTimestampQueryResult> Acquire(const RenderTimestampQueryId query) {
            return operations_.Apply<Result<RenderTimestampQueryResult>>(query, [this, query](auto &record) {
                if (record.state == Pending || record.state == Submitted)
                    return Result<RenderTimestampQueryResult>::Failure(QueryError(RenderQueryErrors::ResultPending));
                if (record.state == Cancelled)
                    return Result<RenderTimestampQueryResult>::Failure(QueryError(RenderQueryErrors::Cancelled));
                if (record.state == TimedOut)
                    return Result<RenderTimestampQueryResult>::Failure(QueryError(RenderQueryErrors::TimedOut));
                if (record.state == Failed)
                    return Result<RenderTimestampQueryResult>::Failure(*record.failure);
                if (!record.timestamp.has_value())
                    return Result<RenderTimestampQueryResult>::Failure(QueryError(RenderQueryErrors::TimestampInvalid));
                const RenderTimestampQueryResult result{record.id, record.completion, *record.timestamp};
                core_.Erase(query);
                return Result<RenderTimestampQueryResult>::Success(result);
            });
        }

        [[nodiscard]] Result<void> Discard(const RenderTimestampQueryId query) {
            const auto result = operations_.Apply<Result<void>>(query, [this](const auto &record) {
                if (!IsTerminal(record.state) || record.backendRetirementPending)
                    return InvalidTransition("Only fully retired terminal timestamp queries may be discarded.");
                return Result<void>::Success();
            });
            if (result.HasValue())
                core_.Erase(query);
            return result;
        }

        [[nodiscard]] RenderTimestampQuerySnapshot Snapshot() const noexcept {
            return detail::MakeQueueSnapshot<RenderTimestampQuerySnapshot>(core_, Submitted, Ready, IsTerminal, [this](auto &snapshot) {
                snapshot.supported = supported_;
            });
        }

        void StopAdmission() noexcept {
            operations_.StopAdmission();
        }

        void Shutdown() noexcept {
            operations_.Shutdown();
        }

    private:
        [[nodiscard]] Result<void> InvalidTransition(std::string message) const {
            return Result<void>::Failure(QueryError(RenderQueryErrors::InvalidTransition, std::move(message)));
        }

        Core core_;
        OperationAdapter operations_;
        bool supported_{false};
    };

    /** @copydoc RenderTimestampQueryQueue::Create */
    Result<std::unique_ptr<RenderTimestampQueryQueue>> RenderTimestampQueryQueue::Create(const RenderResourceOwnerId renderer,
                                                                                         const bool supportsTimestampQueries,
                                                                                         const RenderTimestampQueryLimits &limits) {
        if (!renderer.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<RenderTimestampQueryQueue>>::Failure(
                QueryError(RenderQueryErrors::InvalidConfiguration, "Query owner identity or finite limits are invalid."));
        try {
            return Result<std::unique_ptr<RenderTimestampQueryQueue>>::Success(
                std::unique_ptr<RenderTimestampQueryQueue>(  // NOSONAR(cpp:S5950) -- private constructor requires explicit ownership.
                    new RenderTimestampQueryQueue(std::make_unique<Impl>(renderer, supportsTimestampQueries, limits))));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<RenderTimestampQueryQueue>>::Failure(
                QueryError(RenderQueryErrors::CapacityExceeded, "Timestamp-query queue allocation failed."));
        } catch (const std::length_error &) {
            return Result<std::unique_ptr<RenderTimestampQueryQueue>>::Failure(
                QueryError(RenderQueryErrors::CapacityExceeded, "Timestamp-query metadata capacity cannot be represented."));
        }
    }

    RenderTimestampQueryQueue::RenderTimestampQueryQueue(std::unique_ptr<Impl> implementation) noexcept
        : implementation_(std::move(implementation)) {}

    RenderTimestampQueryQueue::~RenderTimestampQueryQueue() = default;

    /** @copydoc RenderTimestampQueryQueue::Request */
    Result<RenderTimestampQueryId> RenderTimestampQueryQueue::Request(const RenderTimestampQueryDescriptor &descriptor) {
        return implementation_->Request(descriptor);
    }

    /** @copydoc RenderTimestampQueryQueue::State */
    Result<RenderTimestampQueryState> RenderTimestampQueryQueue::State(const RenderTimestampQueryId query) const {
        return implementation_->State(query);
    }

    /** @copydoc RenderTimestampQueryQueue::IsReady */
    Result<bool> RenderTimestampQueryQueue::IsReady(const RenderTimestampQueryId query) const {
        return implementation_->IsReady(query);
    }

    /** @copydoc RenderTimestampQueryQueue::MarkSubmitted */
    Result<void> RenderTimestampQueryQueue::MarkSubmitted(const RenderTimestampQueryId query, const RenderTimelinePoint completion) {
        return implementation_->MarkSubmitted(query, completion);
    }

    /** @copydoc RenderTimestampQueryQueue::Complete */
    Result<void> RenderTimestampQueryQueue::Complete(const RenderTimestampQueryId query, const std::chrono::nanoseconds timestamp) {
        return implementation_->Complete(query, timestamp);
    }

    /** @copydoc RenderTimestampQueryQueue::Fail */
    Result<void> RenderTimestampQueryQueue::Fail(const RenderTimestampQueryId query, const Error &error) {
        return implementation_->Fail(query, error);
    }

    /** @copydoc RenderTimestampQueryQueue::Cancel */
    Result<void> RenderTimestampQueryQueue::Cancel(const RenderTimestampQueryId query) {
        return implementation_->Cancel(query);
    }

    /** @copydoc RenderTimestampQueryQueue::Timeout */
    Result<void> RenderTimestampQueryQueue::Timeout(const RenderTimestampQueryId query) {
        return implementation_->Timeout(query);
    }

    /** @copydoc RenderTimestampQueryQueue::Retire */
    Result<void> RenderTimestampQueryQueue::Retire(const RenderTimestampQueryId query) {
        return implementation_->Retire(query);
    }

    /** @copydoc RenderTimestampQueryQueue::Acquire */
    Result<RenderTimestampQueryResult> RenderTimestampQueryQueue::Acquire(const RenderTimestampQueryId query) {
        return implementation_->Acquire(query);
    }

    /** @copydoc RenderTimestampQueryQueue::Discard */
    Result<void> RenderTimestampQueryQueue::Discard(const RenderTimestampQueryId query) {
        return implementation_->Discard(query);
    }

    /** @copydoc RenderTimestampQueryQueue::Snapshot */
    RenderTimestampQuerySnapshot RenderTimestampQueryQueue::Snapshot() const noexcept {
        return implementation_->Snapshot();
    }

    /** @copydoc RenderTimestampQueryQueue::StopAdmission */
    void RenderTimestampQueryQueue::StopAdmission() noexcept {
        implementation_->StopAdmission();
    }

    /** @copydoc RenderTimestampQueryQueue::Shutdown */
    void RenderTimestampQueryQueue::Shutdown() noexcept {
        implementation_->Shutdown();
    }
}  // namespace Horo::Render
