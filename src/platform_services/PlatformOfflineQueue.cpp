#include "Horo/PlatformServices/PlatformOfflineQueue.h"

#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Horo::PlatformServices {
    struct PlatformOfflineQueue::State final {
        struct Receipt final {
            PlatformOfflineIntent intent;
            TimePoint admittedAt;
            TimePoint expiresAt;
            PlatformOfflineIntentState state{PlatformOfflineIntentState::Pending};
            std::optional<TimePoint> terminalAt;
        };

        PlatformOfflineOperationHandle handle;
        PlatformOfflineLaneKey lane;
        PlatformOfflineOperation operation;
        PlatformOfflineOperationState state{PlatformOfflineOperationState::Pending};
        std::vector<Receipt> receipts;
        std::optional<TimePoint> terminalAt;
    };

    namespace {
        using OfflineClock = PlatformOfflineQueue::Clock;
        using OfflineTimePoint = PlatformOfflineQueue::TimePoint;

        /** @brief Returns the pseudonymous subject partition shared by either semantic lane type. */
        [[nodiscard]] const PlatformOfflineSubjectPartition &SubjectOf(const PlatformOfflineLaneKey &lane) noexcept {
            return std::visit([](const auto &typedLane) -> const PlatformOfflineSubjectPartition & {
                return typedLane.subject;
            }, lane);
        }

        /** @brief Identifies the bounded desired-presence operation category. */
        [[nodiscard]] bool IsPresence(const PlatformOfflineOperation &operation) noexcept {
            return std::holds_alternative<PlatformOfflinePresenceDesiredState>(operation);
        }

        /** @brief Identifies receipts that still consume active capacity or require lifecycle handling. */
        [[nodiscard]] bool IsActive(const PlatformOfflineIntentState state) noexcept {
            return state == PlatformOfflineIntentState::Pending || state == PlatformOfflineIntentState::Dispatching ||
                   state == PlatformOfflineIntentState::Reconciling || state == PlatformOfflineIntentState::Suspended;
        }

        /** @brief Verifies that a typed operation target matches the identity of its lane. */
        template <typename T> [[nodiscard]] bool LaneTargets(const PlatformOfflineProgressionTarget &target, const T expected) noexcept {
            const auto *typedTarget = std::get_if<T>(&target);
            return typedTarget != nullptr && *typedTarget == expected;
        }

        /** @brief Rejects malformed identities, lane/operation mismatches, and oversized presence payloads. */
        [[nodiscard]] bool IsValidOperation(const PlatformOfflineIntent &intent, const std::size_t maximumPresenceDetailBytes) noexcept {
            if (!intent.id.IsValid())
                return false;

            if (const auto *lane = std::get_if<PlatformOfflineProgressionLane>(&intent.lane)) {
                if (!lane->subject.IsValid() || !lane->policy.IsValid() || !lane->accessPolicy.IsValid())
                    return false;

                return std::visit([lane](const auto &operation) {
                    using T = std::decay_t<decltype(operation)>;
                    if constexpr (std::is_same_v<T, PlatformOfflineUnlockOnce>)
                        return operation.achievement.IsValid() && LaneTargets(lane->target, operation.achievement);
                    else if constexpr (std::is_same_v<T, PlatformOfflineSetProgressMaximum>)
                        return operation.achievement.IsValid() && operation.total != 0 && operation.progress <= operation.total &&
                               LaneTargets(lane->target, operation.achievement);
                    else if constexpr (std::is_same_v<T, PlatformOfflineSetStatMaximum> ||
                                       std::is_same_v<T, PlatformOfflineSetStatMinimum> ||
                                       std::is_same_v<T, PlatformOfflineReplaceStatAtRevision> ||
                                       std::is_same_v<T, PlatformOfflineAddStatOnce>) {
                        if (!operation.stat.IsValid() || !LaneTargets(lane->target, operation.stat))
                            return false;
                        if constexpr (std::is_same_v<T, PlatformOfflineReplaceStatAtRevision>)
                            return operation.expectedRevision != 0;
                        else if constexpr (std::is_same_v<T, PlatformOfflineAddStatOnce>)
                            return operation.delta != 0;
                        else
                            return true;
                    } else if constexpr (std::is_same_v<T, PlatformOfflineSubmitBestScore> ||
                                         std::is_same_v<T, PlatformOfflineReplaceScoreAtRevision>) {
                        if (!operation.leaderboard.IsValid() || !LaneTargets(lane->target, operation.leaderboard))
                            return false;
                        if constexpr (std::is_same_v<T, PlatformOfflineSubmitBestScore>)
                            return operation.order == PlatformOfflineScoreOrder::LowerIsBetter ||
                                   operation.order == PlatformOfflineScoreOrder::HigherIsBetter;
                        else
                            return operation.expectedRevision != 0;
                    } else
                        return false;
                }, intent.operation);
            }

            const auto *lane = std::get_if<PlatformOfflinePresenceLane>(&intent.lane);
            const auto *operation = std::get_if<PlatformOfflinePresenceDesiredState>(&intent.operation);
            if (lane == nullptr || operation == nullptr || !lane->subject.IsValid() || !lane->policy.IsValid() ||
                !lane->accessPolicy.IsValid() || lane->purpose != PlatformOfflinePresencePurpose::PresencePublish ||
                operation->detail.size() > maximumPresenceDetailBytes)
                return false;

            if (operation->action == PlatformOfflinePresenceDesiredState::Action::Set)
                return operation->status.IsValid();
            return operation->action == PlatformOfflinePresenceDesiredState::Action::Clear && !operation->status.IsValid() &&
                   operation->detail.empty();
        }

        /** @brief Applies only a declared semantics-preserving reduction to two adjacent operations. */
        [[nodiscard]] bool TryCoalesce(const PlatformOfflineOperation &current, const PlatformOfflineOperation &incoming,
                                       PlatformOfflineOperation &combined) {
            if (current.index() != incoming.index())
                return false;

            if (const auto *left = std::get_if<PlatformOfflineUnlockOnce>(&current)) {
                const auto &right = std::get<PlatformOfflineUnlockOnce>(incoming);
                if (left->achievement != right.achievement)
                    return false;
                combined = *left;
                return true;
            }
            if (const auto *left = std::get_if<PlatformOfflineSetProgressMaximum>(&current)) {
                const auto &right = std::get<PlatformOfflineSetProgressMaximum>(incoming);
                if (left->achievement != right.achievement || left->total != right.total)
                    return false;
                combined = PlatformOfflineSetProgressMaximum{.achievement = left->achievement,
                                                             .progress = std::max(left->progress, right.progress),
                                                             .total = left->total};
                return true;
            }
            if (const auto *left = std::get_if<PlatformOfflineSetStatMaximum>(&current)) {
                const auto &right = std::get<PlatformOfflineSetStatMaximum>(incoming);
                if (left->stat != right.stat)
                    return false;
                combined = PlatformOfflineSetStatMaximum{.stat = left->stat, .value = std::max(left->value, right.value)};
                return true;
            }
            if (const auto *left = std::get_if<PlatformOfflineSetStatMinimum>(&current)) {
                const auto &right = std::get<PlatformOfflineSetStatMinimum>(incoming);
                if (left->stat != right.stat)
                    return false;
                combined = PlatformOfflineSetStatMinimum{.stat = left->stat, .value = std::min(left->value, right.value)};
                return true;
            }
            if (const auto *left = std::get_if<PlatformOfflineSubmitBestScore>(&current)) {
                const auto &right = std::get<PlatformOfflineSubmitBestScore>(incoming);
                if (left->leaderboard != right.leaderboard || left->order != right.order)
                    return false;
                const auto best = left->order == PlatformOfflineScoreOrder::LowerIsBetter ? std::min(left->score, right.score)
                                                                                          : std::max(left->score, right.score);
                combined = PlatformOfflineSubmitBestScore{.leaderboard = left->leaderboard, .score = best, .order = left->order};
                return true;
            }
            return false;
        }

        /** @brief Adds a bounded lifetime without overflowing the monotonic clock representation. */
        [[nodiscard]] std::optional<OfflineTimePoint> AddAge(const OfflineTimePoint time, const std::chrono::seconds age) noexcept {
            const auto delta = std::chrono::duration_cast<OfflineClock::duration>(age);
            if (time.time_since_epoch() > OfflineClock::duration::max() - delta)
                return std::nullopt;
            return OfflineTimePoint{time.time_since_epoch() + delta};
        }

    }  // namespace

    /** @copydoc PlatformOfflineQueue::PlatformOfflineQueue */
    PlatformOfflineQueue::PlatformOfflineQueue(PlatformOfflineQueueConfig config) : config_(config) {}

    /** @copydoc PlatformOfflineQueue::~PlatformOfflineQueue */
    PlatformOfflineQueue::~PlatformOfflineQueue() = default;

    /** @copydoc PlatformOfflineQueue::ConfigurationIsValid */
    bool PlatformOfflineQueue::ConfigurationIsValid() const noexcept {
        return config_.activeCapacity != 0 && config_.activeCapacity <= PlatformOfflineQueueMaximumActiveIntents &&
               config_.perSubjectActiveCapacity != 0 &&
               config_.perSubjectActiveCapacity <= PlatformOfflineQueueMaximumActiveIntentsPerSubject &&
               config_.retainedCapacity >= config_.activeCapacity &&
               config_.retainedCapacity <= PlatformOfflineQueueMaximumRetainedIntents && config_.progressionMaximumAge.count() > 0 &&
               config_.progressionMaximumAge <= PlatformOfflineQueueMaximumProgressionAge && config_.presenceMaximumAge.count() > 0 &&
               config_.presenceMaximumAge < config_.progressionMaximumAge &&
               config_.presenceMaximumAge <= PlatformOfflineQueueMaximumPresenceAge && config_.terminalRetention.count() > 0 &&
               config_.terminalRetention <= PlatformOfflineQueueMaximumTerminalRetention && config_.producerRedeliveryHorizon.count() > 0 &&
               config_.producerRedeliveryHorizon <= config_.terminalRetention &&
               config_.producerRedeliveryHorizon <= PlatformOfflineQueueMaximumTerminalRetention &&
               config_.maximumPresenceDetailBytes <= PlatformOfflineQueueMaximumPresenceDetailBytes && config_.generation.IsValid();
    }

    /** @copydoc PlatformOfflineQueue::ObserveTime */
    Result<void> PlatformOfflineQueue::ObserveTime(const TimePoint now) {
        if (!ConfigurationIsValid())
            return Result<void>::Failure(MakeError(OfflineQueueErrors::InvalidConfiguration));
        if (lastObservedAt_.has_value() && now < *lastObservedAt_)
            return Result<void>::Failure(MakeError(OfflineQueueErrors::ClockMovedBackward));
        lastObservedAt_ = now;
        return Result<void>::Success();
    }

    /** @copydoc PlatformOfflineQueue::FindOperation */
    PlatformOfflineQueue::State *PlatformOfflineQueue::FindOperation(const PlatformOfflineOperationHandle operation) noexcept {
        if (!operation.IsValid() || operation.generation != config_.generation)
            return nullptr;
        const auto found = std::ranges::find_if(operations_, [operation](const State &candidate) {
            return candidate.handle.sequence == operation.sequence;
        });
        return found == operations_.end() ? nullptr : &*found;
    }

    /** @copydoc PlatformOfflineQueue::FindOperation */
    const PlatformOfflineQueue::State *PlatformOfflineQueue::FindOperation(const PlatformOfflineOperationHandle operation) const noexcept {
        if (!operation.IsValid() || operation.generation != config_.generation)
            return nullptr;
        const auto found = std::ranges::find_if(operations_, [operation](const State &candidate) {
            return candidate.handle.sequence == operation.sequence;
        });
        return found == operations_.end() ? nullptr : &*found;
    }

    /** @copydoc PlatformOfflineQueue::FindReceipt */
    PlatformOfflineQueue::State *PlatformOfflineQueue::FindReceipt(const PlatformOfflineIntentId id) noexcept {
        const auto found = std::ranges::find_if(operations_, [id](const State &candidate) {
            return std::ranges::any_of(candidate.receipts, [id](const State::Receipt &receipt) {
                return receipt.intent.id == id;
            });
        });
        return found == operations_.end() ? nullptr : &*found;
    }

    /** @copydoc PlatformOfflineQueue::FindReceipt */
    const PlatformOfflineQueue::State *PlatformOfflineQueue::FindReceipt(const PlatformOfflineIntentId id) const noexcept {
        const auto found = std::ranges::find_if(operations_, [id](const State &candidate) {
            return std::ranges::any_of(candidate.receipts, [id](const State::Receipt &receipt) {
                return receipt.intent.id == id;
            });
        });
        return found == operations_.end() ? nullptr : &*found;
    }

    /** @copydoc PlatformOfflineQueue::Snapshot */
    PlatformOfflineOperationSnapshot PlatformOfflineQueue::Snapshot(const State &state) const {
        PlatformOfflineOperationSnapshot snapshot{.handle = state.handle,
                                                  .lane = state.lane,
                                                  .operation = state.operation,
                                                  .state = state.state};
        snapshot.receipts.reserve(state.receipts.size());
        for (const auto &receipt : state.receipts)
            snapshot.receipts.push_back(PlatformOfflineReceiptSnapshot{.id = receipt.intent.id,
                                                                       .state = receipt.state,
                                                                       .admittedAt = receipt.admittedAt,
                                                                       .expiresAt = receipt.expiresAt,
                                                                       .terminalAt = receipt.terminalAt});
        return snapshot;
    }

    /** @copydoc PlatformOfflineQueue::RecomputeOperation */
    void PlatformOfflineQueue::RecomputeOperation(State &state) {
        const auto first = std::ranges::find_if(state.receipts, [](const State::Receipt &receipt) {
            return IsActive(receipt.state);
        });
        if (first == state.receipts.end()) {
            state.state = PlatformOfflineOperationState::Terminal;
            return;
        }

        auto combined = first->intent.operation;
        for (auto it = std::next(first); it != state.receipts.end(); ++it) {
            if (!IsActive(it->state))
                continue;
            PlatformOfflineOperation next;
            if (TryCoalesce(combined, it->intent.operation, next))
                combined = std::move(next);
        }
        state.operation = std::move(combined);
    }

    /** @copydoc PlatformOfflineQueue::ExpireDue */
    void PlatformOfflineQueue::ExpireDue(State &state, const TimePoint now, std::vector<PlatformOfflineIntentId> *const expired) {
        if (state.state != PlatformOfflineOperationState::Pending && state.state != PlatformOfflineOperationState::Suspended)
            return;

        for (auto &receipt : state.receipts) {
            if ((receipt.state != PlatformOfflineIntentState::Pending && receipt.state != PlatformOfflineIntentState::Suspended) ||
                receipt.expiresAt > now)
                continue;
            receipt.state = PlatformOfflineIntentState::Expired;
            receipt.terminalAt = now;
            --activeIntentCount_;
            if (expired != nullptr)
                expired->push_back(receipt.intent.id);
        }

        const auto hasActiveReceipt = std::ranges::any_of(state.receipts, [](const State::Receipt &receipt) {
            return IsActive(receipt.state);
        });
        if (!hasActiveReceipt) {
            state.state = PlatformOfflineOperationState::Terminal;
            state.terminalAt = now;
            return;
        }
        RecomputeOperation(state);
    }

    /** @copydoc PlatformOfflineQueue::Admit */
    Result<PlatformOfflineAdmission> PlatformOfflineQueue::Admit(PlatformOfflineIntent intent, const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<PlatformOfflineAdmission>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        if (!IsValidOperation(intent, config_.maximumPresenceDetailBytes))
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));

        if (auto *existing = FindReceipt(intent.id); existing != nullptr) {
            const auto receipt = std::ranges::find_if(existing->receipts, [&intent](const State::Receipt &candidate) {
                return candidate.intent.id == intent.id;
            });
            if (receipt->intent != intent)
                return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::IdentityConflict));
            return Result<PlatformOfflineAdmission>::Success(
                PlatformOfflineAdmission{.disposition = PlatformOfflineAdmissionDisposition::JoinedExisting,
                                         .operation = existing->handle});
        }

        const auto lifetime = IsPresence(intent.operation) ? config_.presenceMaximumAge : config_.progressionMaximumAge;
        const auto expiresAt = AddAge(now, lifetime);
        if (!expiresAt.has_value())
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));

        State *tail = nullptr;
        for (auto it = operations_.rbegin(); it != operations_.rend(); ++it) {
            if (it->lane == intent.lane && it->state != PlatformOfflineOperationState::Terminal) {
                tail = &*it;
                break;
            }
        }

        PlatformOfflineOperation combined;
        if (tail != nullptr && tail->state == PlatformOfflineOperationState::Pending && !IsPresence(intent.operation)) {
            const bool allReceiptsLive = std::ranges::all_of(tail->receipts, [now](const State::Receipt &receipt) {
                return !IsActive(receipt.state) || receipt.expiresAt > now;
            });
            if (allReceiptsLive && TryCoalesce(tail->operation, intent.operation, combined)) {
                const auto subject = SubjectOf(intent.lane);
                std::size_t activeForSubject{};
                for (const auto &candidate : operations_)
                    if (SubjectOf(candidate.lane) == subject)
                        activeForSubject +=
                            static_cast<std::size_t>(std::ranges::count_if(candidate.receipts, [](const State::Receipt &receipt) {
                            return IsActive(receipt.state);
                        }));
                if (activeIntentCount_ >= config_.activeCapacity || activeForSubject >= config_.perSubjectActiveCapacity ||
                    retainedIntentCount_ >= config_.retainedCapacity)
                    return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::CapacityExceeded));
                tail->receipts.push_back(State::Receipt{.intent = std::move(intent), .admittedAt = now, .expiresAt = *expiresAt});
                tail->operation = std::move(combined);
                ++activeIntentCount_;
                ++retainedIntentCount_;
                return Result<PlatformOfflineAdmission>::Success(
                    PlatformOfflineAdmission{.disposition = PlatformOfflineAdmissionDisposition::Coalesced, .operation = tail->handle});
            }
        }

        const bool replacingPresence =
            tail != nullptr && IsPresence(intent.operation) && std::holds_alternative<PlatformOfflinePresenceLane>(intent.lane) &&
            (tail->state == PlatformOfflineOperationState::Pending || tail->state == PlatformOfflineOperationState::Suspended) &&
            std::ranges::all_of(tail->receipts, [now](const State::Receipt &receipt) {
            return !IsActive(receipt.state) || receipt.expiresAt > now;
        });
        const auto replacedCount = replacingPresence ? static_cast<std::size_t>(std::ranges::count_if(tail->receipts,
                                                                                                      [](const State::Receipt &receipt) {
            return IsActive(receipt.state);
        }))
                                                     : 0U;
        std::size_t activeForSubject{};
        const auto subject = SubjectOf(intent.lane);
        for (const auto &candidate : operations_)
            if (SubjectOf(candidate.lane) == subject)
                activeForSubject += static_cast<std::size_t>(std::ranges::count_if(candidate.receipts, [](const State::Receipt &receipt) {
                    return IsActive(receipt.state);
                }));
        if (retainedIntentCount_ >= config_.retainedCapacity || activeIntentCount_ + 1U - replacedCount > config_.activeCapacity ||
            activeForSubject + 1U - replacedCount > config_.perSubjectActiveCapacity)
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::CapacityExceeded));
        if (nextSequence_ == 0)
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::CapacityExceeded));

        std::vector<PlatformOfflineIntentId> superseded;
        if (replacingPresence) {
            for (auto &receipt : tail->receipts) {
                if (!IsActive(receipt.state))
                    continue;
                receipt.state = PlatformOfflineIntentState::Superseded;
                receipt.terminalAt = now;
                --activeIntentCount_;
                superseded.push_back(receipt.intent.id);
            }
            tail->state = PlatformOfflineOperationState::Terminal;
            tail->terminalAt = now;
        }

        const auto handle = PlatformOfflineOperationHandle{.generation = config_.generation, .sequence = nextSequence_++};
        State operation{.handle = handle,
                        .lane = intent.lane,
                        .operation = intent.operation,
                        .state = PlatformOfflineOperationState::Pending,
                        .receipts = {State::Receipt{.intent = std::move(intent), .admittedAt = now, .expiresAt = *expiresAt}}};
        operations_.push_back(std::move(operation));
        ++activeIntentCount_;
        ++retainedIntentCount_;
        return Result<PlatformOfflineAdmission>::Success(
            PlatformOfflineAdmission{.disposition = replacingPresence ? PlatformOfflineAdmissionDisposition::PresenceSuperseded
                                                                      : PlatformOfflineAdmissionDisposition::Accepted,
                                     .operation = handle,
                                     .superseded = std::move(superseded)});
    }

    /** @copydoc PlatformOfflineQueue::Query */
    Result<PlatformOfflineOperationSnapshot> PlatformOfflineQueue::Query(const PlatformOfflineIntentId id) const {
        if (!id.IsValid())
            return Result<PlatformOfflineOperationSnapshot>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));
        const auto *operation = FindReceipt(id);
        if (operation == nullptr)
            return Result<PlatformOfflineOperationSnapshot>::Failure(MakeError(OfflineQueueErrors::Expired));
        return Result<PlatformOfflineOperationSnapshot>::Success(Snapshot(*operation));
    }

    /** @copydoc PlatformOfflineQueue::PendingInLane */
    std::vector<PlatformOfflineOperationSnapshot> PlatformOfflineQueue::PendingInLane(const PlatformOfflineLaneKey &lane) const {
        std::vector<PlatformOfflineOperationSnapshot> pending;
        for (const auto &operation : operations_)
            if (operation.state == PlatformOfflineOperationState::Pending && operation.lane == lane)
                pending.push_back(Snapshot(operation));
        return pending;
    }

    /** @copydoc PlatformOfflineQueue::Expire */
    Result<std::vector<PlatformOfflineIntentId>> PlatformOfflineQueue::Expire(const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<std::vector<PlatformOfflineIntentId>>::Failure(observed.ErrorValue());
        std::vector<PlatformOfflineIntentId> expired;
        for (auto &operation : operations_)
            ExpireDue(operation, now, &expired);
        return Result<std::vector<PlatformOfflineIntentId>>::Success(std::move(expired));
    }

    /** @copydoc PlatformOfflineQueue::MarkDispatching */
    Result<PlatformRequestMutation> PlatformOfflineQueue::MarkDispatching(const PlatformOfflineOperationHandle operation,
                                                                          const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Dispatching)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        for (auto &candidate : operations_)
            if (candidate.lane == record->lane && candidate.handle.sequence <= record->handle.sequence)
                ExpireDue(candidate, now, nullptr);
        if (record->state == PlatformOfflineOperationState::Terminal)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Expired));
        if (record->state != PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        const auto earlierLaneWork = std::ranges::any_of(operations_, [record](const State &candidate) {
            return candidate.lane == record->lane && candidate.handle.sequence < record->handle.sequence &&
                   candidate.state != PlatformOfflineOperationState::Terminal;
        });
        if (earlierLaneWork)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        record->state = PlatformOfflineOperationState::Dispatching;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Pending)
                receipt.state = PlatformOfflineIntentState::Dispatching;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::MarkReconciling */
    Result<PlatformRequestMutation> PlatformOfflineQueue::MarkReconciling(const PlatformOfflineOperationHandle operation) {
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Reconciling)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Dispatching)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        record->state = PlatformOfflineOperationState::Reconciling;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Dispatching)
                receipt.state = PlatformOfflineIntentState::Reconciling;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::SuspendPending */
    Result<PlatformRequestMutation> PlatformOfflineQueue::SuspendPending(const PlatformOfflineOperationHandle operation) {
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Suspended)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        record->state = PlatformOfflineOperationState::Suspended;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Pending)
                receipt.state = PlatformOfflineIntentState::Suspended;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::CancelPending */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CancelPending(const PlatformOfflineOperationHandle operation,
                                                                        const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Terminal) {
            const auto hasAbandoned = std::ranges::any_of(record->receipts, [](const State::Receipt &receipt) {
                return receipt.state == PlatformOfflineIntentState::Abandoned;
            });
            const auto onlyAbandonedOrExpired = std::ranges::all_of(record->receipts, [](const State::Receipt &receipt) {
                return receipt.state == PlatformOfflineIntentState::Abandoned || receipt.state == PlatformOfflineIntentState::Expired;
            });
            if (hasAbandoned && onlyAbandonedOrExpired)
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            const auto allExpired = std::ranges::all_of(record->receipts, [](const State::Receipt &receipt) {
                return receipt.state == PlatformOfflineIntentState::Expired;
            });
            return Result<PlatformRequestMutation>::Failure(
                MakeError(allExpired ? OfflineQueueErrors::Expired : OfflineQueueErrors::InvalidTransition));
        }
        if (record->state != PlatformOfflineOperationState::Pending && record->state != PlatformOfflineOperationState::Suspended)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        ExpireDue(*record, now, nullptr);
        if (record->state == PlatformOfflineOperationState::Terminal)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Expired));
        for (auto &receipt : record->receipts) {
            if (!IsActive(receipt.state))
                continue;
            receipt.state = PlatformOfflineIntentState::Abandoned;
            receipt.terminalAt = now;
            --activeIntentCount_;
        }
        record->state = PlatformOfflineOperationState::Terminal;
        record->terminalAt = now;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::Resume */
    Result<PlatformRequestMutation> PlatformOfflineQueue::Resume(const PlatformOfflineOperationHandle operation, const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Suspended)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        ExpireDue(*record, now, nullptr);
        if (record->state == PlatformOfflineOperationState::Terminal)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Expired));
        record->state = PlatformOfflineOperationState::Pending;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Suspended)
                receipt.state = PlatformOfflineIntentState::Pending;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::CompleteOperation */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CompleteOperation(State &record, const PlatformOfflineIntentState terminal,
                                                                            const TimePoint now) {
        if (record.state == PlatformOfflineOperationState::Terminal) {
            const auto hasOutcome = std::ranges::any_of(record.receipts, [terminal](const State::Receipt &receipt) {
                return receipt.state == terminal;
            });
            const auto sameOutcome = std::ranges::all_of(record.receipts, [terminal](const State::Receipt &receipt) {
                return receipt.state == terminal || receipt.state == PlatformOfflineIntentState::Expired;
            });
            return hasOutcome && sameOutcome ? Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged)
                                             : Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        }
        if (record.state != PlatformOfflineOperationState::Dispatching && record.state != PlatformOfflineOperationState::Reconciling)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        for (auto &receipt : record.receipts) {
            if (!IsActive(receipt.state))
                continue;
            receipt.state = terminal;
            receipt.terminalAt = now;
            --activeIntentCount_;
        }
        record.state = PlatformOfflineOperationState::Terminal;
        record.terminalAt = now;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::CompleteSuccess */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CompleteSuccess(const PlatformOfflineOperationHandle operation,
                                                                          const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        return CompleteOperation(*record, PlatformOfflineIntentState::Succeeded, now);
    }

    /** @copydoc PlatformOfflineQueue::CompletePermanentlyFailed */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CompletePermanentlyFailed(const PlatformOfflineOperationHandle operation,
                                                                                    const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        auto *record = FindOperation(operation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        return CompleteOperation(*record, PlatformOfflineIntentState::PermanentlyFailed, now);
    }

    /** @copydoc PlatformOfflineQueue::Shutdown */
    Result<std::vector<PlatformOfflineIntentId>> PlatformOfflineQueue::Shutdown(const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<std::vector<PlatformOfflineIntentId>>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<std::vector<PlatformOfflineIntentId>>::Success({});

        std::vector<PlatformOfflineIntentId> expired;
        for (auto &operation : operations_) {
            ExpireDue(operation, now, &expired);
            if (operation.state == PlatformOfflineOperationState::Pending) {
                operation.state = PlatformOfflineOperationState::Suspended;
                for (auto &receipt : operation.receipts)
                    if (receipt.state == PlatformOfflineIntentState::Pending)
                        receipt.state = PlatformOfflineIntentState::Suspended;
            } else if (operation.state == PlatformOfflineOperationState::Dispatching) {
                operation.state = PlatformOfflineOperationState::Reconciling;
                for (auto &receipt : operation.receipts)
                    if (receipt.state == PlatformOfflineIntentState::Dispatching)
                        receipt.state = PlatformOfflineIntentState::Reconciling;
            }
        }
        closed_ = true;
        return Result<std::vector<PlatformOfflineIntentId>>::Success(std::move(expired));
    }

    /** @copydoc PlatformOfflineQueue::Compact */
    Result<std::vector<PlatformOfflineIntentId>> PlatformOfflineQueue::Compact(const TimePoint now) {
        auto observed = ObserveTime(now);
        if (observed.HasError())
            return Result<std::vector<PlatformOfflineIntentId>>::Failure(observed.ErrorValue());

        std::vector<PlatformOfflineIntentId> compacted;
        auto operation = operations_.begin();
        while (operation != operations_.end()) {
            if (operation->state != PlatformOfflineOperationState::Terminal || !operation->terminalAt.has_value()) {
                ++operation;
                continue;
            }
            const auto retentionEndsAt = AddAge(*operation->terminalAt, config_.terminalRetention);
            if (!retentionEndsAt.has_value() || now < *retentionEndsAt) {
                ++operation;
                continue;
            }
            for (const auto &receipt : operation->receipts)
                compacted.push_back(receipt.intent.id);
            retainedIntentCount_ -= operation->receipts.size();
            operation = operations_.erase(operation);
        }
        return Result<std::vector<PlatformOfflineIntentId>>::Success(std::move(compacted));
    }

    /** @copydoc PlatformOfflineQueue::Generation */
    PlatformOfflineQueueGeneration PlatformOfflineQueue::Generation() const noexcept {
        return config_.generation;
    }

    /** @copydoc PlatformOfflineQueue::ActiveIntentCount */
    std::size_t PlatformOfflineQueue::ActiveIntentCount() const noexcept {
        return activeIntentCount_;
    }

    /** @copydoc PlatformOfflineQueue::RetainedIntentCount */
    std::size_t PlatformOfflineQueue::RetainedIntentCount() const noexcept {
        return retainedIntentCount_;
    }

    /** @copydoc PlatformOfflineQueue::OperationCount */
    std::size_t PlatformOfflineQueue::OperationCount() const noexcept {
        return operations_.size();
    }
}  // namespace Horo::PlatformServices
