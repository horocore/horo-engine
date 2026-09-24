#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"
#include "PlatformOfflineQueueImpl.h"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
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
            using enum PlatformOfflineIntentState;
            return state == Pending || state == Dispatching || state == Reconciling || state == Suspended;
        }

        /** @brief Verifies that a typed operation target matches the identity of its lane. */
        template <typename T> [[nodiscard]] bool LaneTargets(const PlatformOfflineProgressionTarget &target, const T expected) noexcept {
            const auto *typedTarget = std::get_if<T>(&target);
            return typedTarget != nullptr && *typedTarget == expected;
        }

        /** @brief Checks a progression operation against its exact lane target. */
        template <typename T>
        [[nodiscard]] bool IsValidProgressionOperation(const PlatformOfflineProgressionLane &lane, const T &operation) noexcept {
            if constexpr (std::is_same_v<T, PlatformOfflineUnlockOnce>)
                return operation.achievement.IsValid() && LaneTargets(lane.target, operation.achievement);
            else if constexpr (std::is_same_v<T, PlatformOfflineSetProgressMaximum>)
                return operation.achievement.IsValid() && operation.total != 0 && operation.progress <= operation.total &&
                       LaneTargets(lane.target, operation.achievement);
            else if constexpr (std::is_same_v<T, PlatformOfflineSetStatMaximum> || std::is_same_v<T, PlatformOfflineSetStatMinimum>)
                return operation.stat.IsValid() && LaneTargets(lane.target, operation.stat);
            else if constexpr (std::is_same_v<T, PlatformOfflineReplaceStatAtRevision>)
                return operation.stat.IsValid() && operation.expectedRevision != 0 && LaneTargets(lane.target, operation.stat);
            else if constexpr (std::is_same_v<T, PlatformOfflineAddStatOnce>)
                return operation.stat.IsValid() && operation.delta != 0 && LaneTargets(lane.target, operation.stat);
            else if constexpr (std::is_same_v<T, PlatformOfflineSubmitBestScore>)
                return operation.leaderboard.IsValid() && LaneTargets(lane.target, operation.leaderboard) &&
                       (operation.order == PlatformOfflineScoreOrder::LowerIsBetter ||
                        operation.order == PlatformOfflineScoreOrder::HigherIsBetter);
            else if constexpr (std::is_same_v<T, PlatformOfflineReplaceScoreAtRevision>)
                return operation.leaderboard.IsValid() && operation.expectedRevision != 0 &&
                       LaneTargets(lane.target, operation.leaderboard);
            else
                return false;
        }

        /** @brief Checks the bounded desired-presence operation. */
        [[nodiscard]] bool IsValidPresenceOperation(const PlatformOfflinePresenceLane &lane,
                                                    const PlatformOfflinePresenceDesiredState &operation,
                                                    const std::size_t maximumPresenceDetailBytes) noexcept {
            if (!lane.subject.IsValid() || !lane.policy.IsValid() || !lane.accessPolicy.IsValid() ||
                lane.purpose != PlatformOfflinePresencePurpose::PresencePublish || operation.detail.size() > maximumPresenceDetailBytes)
                return false;
            if (operation.action == PlatformOfflinePresenceDesiredState::Action::Set)
                return operation.status.IsValid();
            return operation.action == PlatformOfflinePresenceDesiredState::Action::Clear && !operation.status.IsValid() &&
                   operation.detail.empty();
        }

        /** @brief Rejects malformed identities, lane/operation mismatches, and oversized presence payloads. */
        [[nodiscard]] bool IsValidOperation(const PlatformOfflineIntent &intent, const std::size_t maximumPresenceDetailBytes) noexcept {
            if (!intent.id.IsValid())
                return false;
            if (const auto *lane = std::get_if<PlatformOfflineProgressionLane>(&intent.lane)) {
                if (!lane->subject.IsValid() || !lane->policy.IsValid() || !lane->accessPolicy.IsValid())
                    return false;
                return std::visit([lane]<typename T>(const T &operation) {
                    return IsValidProgressionOperation(*lane, operation);
                }, intent.operation);
            }
            const auto *lane = std::get_if<PlatformOfflinePresenceLane>(&intent.lane);
            const auto *operation = std::get_if<PlatformOfflinePresenceDesiredState>(&intent.operation);
            return lane != nullptr && operation != nullptr && IsValidPresenceOperation(*lane, *operation, maximumPresenceDetailBytes);
        }

        /** @brief Applies only a declared semantics-preserving reduction to two adjacent operations. */
        [[nodiscard]] bool TryCoalesce(const PlatformOfflineOperation &current, const PlatformOfflineOperation &incoming,
                                       PlatformOfflineOperation &combined) {
            if (current.index() != incoming.index())
                return false;

            if (const auto *left = std::get_if<PlatformOfflineUnlockOnce>(&current)) {
                if (const auto &right = std::get<PlatformOfflineUnlockOnce>(incoming); left->achievement != right.achievement)
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

        /** @brief Rebuilds a reduced operation from the live receipts in one aggregate state. */
        template <typename StateType> void RecomputeOperation(StateType &state) {
            auto first = state.receipts.begin();
            while (first != state.receipts.end() && !IsActive(first->state))
                ++first;
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

    }  // namespace

    bool PlatformOfflineQueue::ReceiptsRemainLive(const State &state, const TimePoint now) {
        return std::ranges::all_of(state.receipts, [now](const State::Receipt &receipt) {
            return !IsActive(receipt.state) || receipt.expiresAt > now;
        });
    }

    PlatformOfflineQueue::State *PlatformOfflineQueue::FindTail(const PlatformOfflineLaneKey &lane) noexcept {
        for (auto it = operations_.rbegin(); it != operations_.rend(); ++it) {
            if (it->lane == lane && it->state != PlatformOfflineOperationState::Terminal)
                return std::to_address(it.base() - 1);
        }
        return nullptr;
    }

    std::size_t PlatformOfflineQueue::ActiveForSubject(const PlatformOfflineSubjectPartition &subject) const noexcept {
        std::size_t active{};
        for (const auto &candidate : operations_) {
            if (SubjectOf(candidate.lane) != subject)
                continue;
            active += static_cast<std::size_t>(std::ranges::count_if(candidate.receipts, [](const State::Receipt &receipt) {
                return IsActive(receipt.state);
            }));
        }
        return active;
    }

    Result<std::optional<PlatformOfflineAdmission>> PlatformOfflineQueue::TryCoalesceTail(State *const tail, PlatformOfflineIntent &intent,
                                                                                          const TimePoint now) {
        if (tail == nullptr || tail->state != PlatformOfflineOperationState::Pending || IsPresence(intent.operation) ||
            !ReceiptsRemainLive(*tail, now))
            return Result<std::optional<PlatformOfflineAdmission>>::Success(std::nullopt);

        PlatformOfflineOperation combined;
        if (!TryCoalesce(tail->operation, intent.operation, combined))
            return Result<std::optional<PlatformOfflineAdmission>>::Success(std::nullopt);
        if (const auto subject = SubjectOf(intent.lane); activeIntentCount_ >= config_.activeCapacity ||
                                                         ActiveForSubject(subject) >= config_.perSubjectActiveCapacity ||
                                                         retainedIntentCount_ >= config_.retainedCapacity)
            return Result<std::optional<PlatformOfflineAdmission>>::Failure(MakeError(OfflineQueueErrors::CapacityExceeded));

        tail->receipts.push_back(
            State::Receipt{.intent = std::move(intent), .admittedAt = now, .expiresAt = *AddAge(now, config_.progressionMaximumAge)});
        tail->operation = std::move(combined);
        ++activeIntentCount_;
        ++retainedIntentCount_;
        return Result<std::optional<PlatformOfflineAdmission>>::Success(
            PlatformOfflineAdmission{.disposition = PlatformOfflineAdmissionDisposition::Coalesced, .operation = tail->handle});
    }

    Result<PlatformOfflineAdmission> PlatformOfflineQueue::AdmitFresh(PlatformOfflineIntent intent, const TimePoint now,
                                                                      const TimePoint expiresAt, State *const tail) {
        const bool replacingPresence =
            tail != nullptr && IsPresence(intent.operation) && std::holds_alternative<PlatformOfflinePresenceLane>(intent.lane) &&
            (tail->state == PlatformOfflineOperationState::Pending || tail->state == PlatformOfflineOperationState::Suspended) &&
            ReceiptsRemainLive(*tail, now);
        State *const presenceTail = replacingPresence ? tail : nullptr;
        const auto replacedCount =
            presenceTail == nullptr
                ? std::size_t{}
                : static_cast<std::size_t>(std::ranges::count_if(presenceTail->receipts, [](const State::Receipt &receipt) {
            return IsActive(receipt.state);
        }));
        if (const auto subject = SubjectOf(intent.lane);
            retainedIntentCount_ >= config_.retainedCapacity || activeIntentCount_ + 1U - replacedCount > config_.activeCapacity ||
            ActiveForSubject(subject) + 1U - replacedCount > config_.perSubjectActiveCapacity || nextSequence_ == 0)
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::CapacityExceeded));

        std::vector<PlatformOfflineIntentId> superseded;
        if (presenceTail != nullptr) {
            superseded.reserve(replacedCount);
            for (const auto &receipt : presenceTail->receipts)
                if (IsActive(receipt.state))
                    superseded.push_back(receipt.intent.id);
        }

        const auto presenceTailIndex =
            presenceTail == nullptr ? operations_.size() : static_cast<std::size_t>(presenceTail - operations_.data());
        const auto handle = PlatformOfflineOperationHandle{.generation = config_.generation, .sequence = nextSequence_};
        State freshOperation{.handle = handle,
                             .lane = intent.lane,
                             .operation = intent.operation,
                             .state = PlatformOfflineOperationState::Pending,
                             .receipts = {State::Receipt{.intent = std::move(intent), .admittedAt = now, .expiresAt = expiresAt}}};
        PlatformOfflineAdmission admission{.disposition = replacingPresence ? PlatformOfflineAdmissionDisposition::PresenceSuperseded
                                                                            : PlatformOfflineAdmissionDisposition::Accepted,
                                           .operation = handle,
                                           .superseded = std::move(superseded)};

        operations_.reserve(operations_.size() + 1U);
        operations_.push_back(std::move(freshOperation));
        if (presenceTail != nullptr) {
            auto &replaced = operations_[presenceTailIndex];
            for (auto &receipt : replaced.receipts) {
                if (!IsActive(receipt.state))
                    continue;
                receipt.state = PlatformOfflineIntentState::Superseded;
                receipt.terminalAt = now;
                --activeIntentCount_;
            }
            replaced.state = PlatformOfflineOperationState::Terminal;
            replaced.terminalAt = now;
        }
        ++nextSequence_;
        ++activeIntentCount_;
        ++retainedIntentCount_;
        return Result<PlatformOfflineAdmission>::Success(std::move(admission));
    }

    /** @copydoc PlatformOfflineQueue::ExpireDue */
    void PlatformOfflineQueue::ExpireDue(State &state, const TimePoint now, std::vector<PlatformOfflineIntentId> *const expired) {
        using enum PlatformOfflineIntentState;
        if (state.state != PlatformOfflineOperationState::Pending && state.state != PlatformOfflineOperationState::Suspended)
            return;
        for (auto &receipt : state.receipts) {
            if ((receipt.state != Pending && receipt.state != Suspended) || receipt.expiresAt > now)
                continue;
            receipt.state = Expired;
            receipt.terminalAt = now;
            --activeIntentCount_;
            if (expired != nullptr)
                expired->push_back(receipt.intent.id);
        }
        if (!std::ranges::any_of(state.receipts, [](const State::Receipt &receipt) {
            return IsActive(receipt.state);
        })) {
            state.state = PlatformOfflineOperationState::Terminal;
            state.terminalAt = now;
            return;
        }
        RecomputeOperation(state);
    }

    /** @copydoc PlatformOfflineQueue::Admit */
    Result<PlatformOfflineAdmission> PlatformOfflineQueue::Admit(PlatformOfflineIntent intent, const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformOfflineAdmission>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        if (!IsValidOperation(intent, config_.maximumPresenceDetailBytes))
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));
        if (auto *existing = detail::FindReceipt(operations_, intent.id); existing != nullptr) {
            if (const auto receipt = std::ranges::find_if(existing->receipts,
                                                          [&intent](const State::Receipt &candidate) {
                return candidate.intent.id == intent.id;
            });
                receipt->intent != intent)
                return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::IdentityConflict));
            return Result<PlatformOfflineAdmission>::Success(
                PlatformOfflineAdmission{.disposition = PlatformOfflineAdmissionDisposition::JoinedExisting,
                                         .operation = existing->handle});
        }
        const auto lifetime = IsPresence(intent.operation) ? config_.presenceMaximumAge : config_.progressionMaximumAge;
        const auto expiresAt = AddAge(now, lifetime);
        if (!expiresAt.has_value())
            return Result<PlatformOfflineAdmission>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));
        auto *const tail = FindTail(intent.lane);
        auto coalesced = TryCoalesceTail(tail, intent, now);
        if (coalesced.HasError())
            return Result<PlatformOfflineAdmission>::Failure(coalesced.ErrorValue());
        if (auto coalescedAdmission = std::move(coalesced).Value(); coalescedAdmission.has_value())
            return Result<PlatformOfflineAdmission>::Success(std::move(*coalescedAdmission));
        return AdmitFresh(std::move(intent), now, *expiresAt, tail);
    }
}  // namespace Horo::PlatformServices
