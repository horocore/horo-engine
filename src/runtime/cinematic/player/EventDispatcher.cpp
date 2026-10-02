#include "Horo/Cinematic/EventDispatcher.h"

#include "Horo/Cinematic/EventTrackErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        [[nodiscard]] bool ValidContext(const EventRuntimeContext context) noexcept {
            using enum EventRuntimeContext;
            return context == Runtime || context == Pie || context == Preview || context == Headless;
        }

        /** @brief Validates the complete inert handler contract without inspecting dispatcher lifecycle state. */
        [[nodiscard]] bool ValidHandler(const EventHandlerRegistration &registration, const EventRuntimeContext context) noexcept {
            return registration.binding.IsValid() && registration.schema.IsValid() && registration.context == context &&
                   registration.generation != 0 && registration.callbackContext.IsValid() && registration.callback != nullptr &&
                   registration.lease != nullptr;
        }

        /** @brief Requires exact cooked/evaluation timing and reverse eligibility, not merely matching key identities. */
        [[nodiscard]] bool PlanMatches(const CookedEventPlan &plan, const SequenceFrameEvaluationPlan &evaluation) noexcept {
            return evaluation.EventCount() == plan.Keys().size() &&
                   std::ranges::all_of(evaluation.EventKeys(), [&plan](const SequenceFrameEventKey &event) {
                const auto *key = plan.Find(event.track, event.key);
                return key != nullptr && key->time == event.time && key->fireInReverse == event.fireInReverse;
            });
        }

        [[nodiscard]] bool SameOccurrence(const SequenceFrameEventOccurrence &left, const SequenceFrameEventOccurrence &right) noexcept {
            return left.player == right.player && left.track == right.track && left.key == right.key && left.traversal == right.traversal &&
                   left.direction == right.direction;
        }
    }  // namespace

    CinematicEventDispatcher::CinematicEventDispatcher(const CinematicRuntimeSessionId session, const EventRuntimeContext context,
                                                       const std::size_t maximumPending, const std::size_t maximumResults)
        : session_(session), context_(context), maximumPending_(maximumPending), maximumResults_(maximumResults) {
        pending_.reserve(maximumPending);
        results_.reserve(maximumResults);
    }

    /** @copydoc CinematicEventDispatcher::Create */
    Result<CinematicEventDispatcher> CinematicEventDispatcher::Create(const CinematicRuntimeSessionId session,
                                                                      const EventRuntimeContext context, const std::size_t maximumPending,
                                                                      const std::size_t maximumResults) {
        if (!session.IsValid() || !ValidContext(context) || maximumPending == 0 || maximumResults == 0 ||
            maximumPending > MaximumFrameOccurrences || maximumResults > MaximumFrameOccurrences)
            return Failed<CinematicEventDispatcher>(EventTrackErrors::DispatchStateInvalid);
        return Result<CinematicEventDispatcher>::Success(CinematicEventDispatcher(session, context, maximumPending, maximumResults));
    }

    const CinematicEventDispatcher::Registration *CinematicEventDispatcher::FindRegistration(const EventBindingId binding) const noexcept {
        const auto found = std::ranges::find_if(registrations_, [&](const Registration &entry) {
            return entry.handler.binding == binding;
        });
        return found == registrations_.end() ? nullptr : std::to_address(found);
    }

    CinematicEventDispatcher::Registration *CinematicEventDispatcher::FindRegistration(const EventBindingId binding) noexcept {
        const auto found = std::ranges::find(registrations_, binding, [](const Registration &entry) {
            return entry.handler.binding;
        });
        return found == registrations_.end() ? nullptr : std::to_address(found);
    }

    const CinematicEventDispatcher::Player *CinematicEventDispatcher::FindPlayer(const SequencePlayerHandle &handle) const noexcept {
        const auto found = std::ranges::find_if(players_, [&](const Player &entry) {
            return entry.handle == handle;
        });
        return found == players_.end() ? nullptr : std::to_address(found);
    }

    CinematicEventDispatcher::Player *CinematicEventDispatcher::FindPlayer(const SequencePlayerHandle &handle) noexcept {
        const auto found = std::ranges::find(players_, handle, &Player::handle);
        return found == players_.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc CinematicEventDispatcher::Delivered */
    bool CinematicEventDispatcher::Delivered(const SequenceFrameEventOccurrence &occurrence) const noexcept {
        const Player *player = FindPlayer(occurrence.player);
        if (player == nullptr)
            return false;
        const auto found = std::ranges::find_if(player->delivered, [&](const Player::DeliveryWatermark &entry) {
            return entry.track == occurrence.track && entry.key == occurrence.key;
        });
        if (found == player->delivered.end())
            return false;
        const auto watermark = occurrence.direction == SequenceTraversalDirection::Reverse ? found->reverse : found->forward;
        return occurrence.traversal <= watermark;
    }

    /** @copydoc CinematicEventDispatcher::Register */
    Result<void> CinematicEventDispatcher::Register(EventHandlerRegistration registration) {
        if (closed_ || draining_ || openTick_ != 0 || !ValidHandler(registration, context_))
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        if (Registration *old = FindRegistration(registration.binding)) {
            if (old->active || registration.generation <= old->handler.generation)
                return Failed<void>(EventTrackErrors::CookInvalid);
            old->handler = std::move(registration);
            old->active = true;
            return Result<void>::Success();
        }
        if (registrations_.size() >= MaximumFrameOccurrences)
            return Failed<void>(EventTrackErrors::DispatchCapacityExceeded);
        registrations_.emplace_back(std::move(registration), true);
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::Revoke */
    Result<void> CinematicEventDispatcher::Revoke(const EventBindingId binding, const std::uint64_t generation) {
        if (closed_ || draining_ || openTick_ != 0)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        Registration *registration = FindRegistration(binding);
        if (registration == nullptr || !registration->active || registration->handler.generation != generation)
            return Failed<void>(EventTrackErrors::StaleBinding);
        registration->active = false;
        registration->handler.lease.reset();
        registration->handler.callback = nullptr;
        registration->handler.callbackContext = {};
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::Activate */
    Result<void> CinematicEventDispatcher::Activate(const SequencePlayerHandle &player, std::shared_ptr<const CookedEventPlan> plan,
                                                    const SequenceFrameEvaluationPlan &evaluation, const std::int32_t priority) {
        if (closed_ || draining_ || openTick_ != 0 || !player.IsValid() || player.session != session_ || !plan ||
            FindPlayer(player) != nullptr || players_.size() >= MaximumFramePlayers)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        if (!PlanMatches(*plan, evaluation))
            return Failed<void>(EventTrackErrors::CookInvalid);
        auto candidate = BindPlayer(player, std::move(plan), priority);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        players_.push_back(std::move(candidate).Value());
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::BindPlayer */
    Result<CinematicEventDispatcher::Player> CinematicEventDispatcher::BindPlayer(const SequencePlayerHandle &player,
                                                                                  std::shared_ptr<const CookedEventPlan> plan,
                                                                                  const std::int32_t priority) const {
        Player candidate{player, std::move(plan), {}, priority, true};
        candidate.bindings.reserve(candidate.plan->Keys().size());
        candidate.failedTracks.reserve(candidate.plan->Keys().size());
        candidate.delivered.reserve(candidate.plan->Keys().size());
        for (const CookedEventKey &key : candidate.plan->Keys()) {
            candidate.delivered.emplace_back(key.track, key.key, 0, 0);
            if ((key.allowedContexts & static_cast<std::byte>(context_)) == std::byte{})
                return Failed<Player>(EventTrackErrors::BindingUnavailable);
            const Registration *registration = FindRegistration(key.binding);
            const bool available = registration != nullptr && registration->active && registration->handler.schema == key.schema;
            if (!available && key.required)
                return Failed<Player>(EventTrackErrors::BindingUnavailable);
            if (std::ranges::any_of(candidate.bindings, [&](const BindingFence &fence) {
                return fence.binding == key.binding;
            }))
                continue;
            candidate.bindings.emplace_back(key.binding, available ? registration->handler.generation : 0);
        }
        return Result<Player>::Success(std::move(candidate));
    }

    /** @copydoc CinematicEventDispatcher::StopPlayer */
    Result<void> CinematicEventDispatcher::StopPlayer(const SequencePlayerHandle &player) {
        if (closed_ || draining_ || openTick_ != 0)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        Player *entry = FindPlayer(player);
        if (entry == nullptr)
            return Failed<void>(EventTrackErrors::StaleBinding);
        entry->accepting = false;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::CancelPlayer */
    Result<void> CinematicEventDispatcher::CancelPlayer(const SequencePlayerHandle &player) {
        if (closed_ || draining_ || openTick_ != 0)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        const auto found = std::ranges::find_if(players_, [&](const Player &entry) {
            return entry.handle == player;
        });
        if (found == players_.end())
            return Failed<void>(EventTrackErrors::StaleBinding);
        std::erase_if(pending_, [&player](const Pending &entry) {
            return entry.occurrence.player == player;
        });
        players_.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::BeginTick */
    Result<void> CinematicEventDispatcher::BeginTick(const std::uint64_t tick) {
        if (closed_ || draining_ || openTick_ != 0 || tick == 0 || tick <= lastCommittedTick_)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        openTick_ = tick;
        tickStart_ = pending_.size();
        tickFailed_ = false;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::ValidateOccurrence */
    Result<void> CinematicEventDispatcher::ValidateOccurrence(const SequenceFrameEventOccurrence &occurrence) const {
        const Player *player = FindPlayer(occurrence.player);
        if (player == nullptr || !player->accepting)
            return Failed<void>(EventTrackErrors::StaleBinding);
        const CookedEventKey *key = player->plan->Find(occurrence.track, occurrence.key);
        if (key == nullptr)
            return Failed<void>(EventTrackErrors::UnknownName);
        if (occurrence.traversal == 0 ||
            (occurrence.direction != SequenceTraversalDirection::Forward && occurrence.direction != SequenceTraversalDirection::Reverse))
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        if (occurrence.time != key->time || (occurrence.direction == SequenceTraversalDirection::Reverse && !key->fireInReverse))
            return Failed<void>(EventTrackErrors::CookInvalid);
        if (!key->required)
            return Result<void>::Success();
        const auto fence = std::ranges::find(player->bindings, key->binding, &BindingFence::binding);
        if (const Registration *registration = FindRegistration(key->binding);
            fence == player->bindings.end() || registration == nullptr || !registration->active ||
            registration->handler.generation != fence->generation || registration->handler.schema != key->schema)
            return Failed<void>(EventTrackErrors::StaleBinding);
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::PreviouslySeen */
    bool CinematicEventDispatcher::PreviouslySeen(const SequenceFrameEventOccurrence &occurrence) const noexcept {
        return Delivered(occurrence) || std::ranges::any_of(pending_, [&occurrence](const Pending &entry) {
            return SameOccurrence(entry.occurrence, occurrence);
        });
    }

    /** @copydoc CinematicEventDispatcher::Stage */
    Result<void> CinematicEventDispatcher::Stage(const std::span<const SequenceFrameEventOccurrence> occurrences) {
        if (closed_ || draining_ || openTick_ == 0)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        if (tickFailed_)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        const auto failTick = [this](const ErrorCodeDescriptor &code) {
            tickFailed_ = true;
            return Failed<void>(code);
        };
        if (occurrences.size() > MaximumFrameOccurrences)
            return failTick(EventTrackErrors::DispatchCapacityExceeded);
        std::size_t additional{};
        for (std::size_t index = 0; index < occurrences.size(); ++index) {
            const SequenceFrameEventOccurrence &occurrence = occurrences[index];
            if (auto validated = ValidateOccurrence(occurrence); validated.HasError()) {
                tickFailed_ = true;
                return validated;
            }
            if (std::ranges::any_of(occurrences.first(index), [&](const auto &previous) {
                return SameOccurrence(previous, occurrence);
            }))
                return failTick(EventTrackErrors::DispatchStateInvalid);
            additional += PreviouslySeen(occurrence) ? 0U : 1U;
        }
        if (additional > maximumPending_ - pending_.size())
            return failTick(EventTrackErrors::DispatchCapacityExceeded);
        for (const SequenceFrameEventOccurrence &occurrence : occurrences) {
            if (PreviouslySeen(occurrence))
                continue;
            const Player &player = *FindPlayer(occurrence.player);
            const CookedEventKey &key = *player.plan->Find(occurrence.track, occurrence.key);
            const auto fence = std::ranges::find(player.bindings, key.binding, &BindingFence::binding);
            pending_.emplace_back(occurrence, player.plan, fence->generation, openTick_, player.priority, false);
        }
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::CommitTick */
    Result<void> CinematicEventDispatcher::CommitTick() {
        if (closed_ || draining_ || openTick_ == 0)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        if (tickFailed_)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        for (std::size_t index = tickStart_; index < pending_.size(); ++index)
            pending_[index].committed = true;
        lastCommittedTick_ = openTick_;
        openTick_ = 0;
        tickFailed_ = false;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::AbortTick */
    Result<void> CinematicEventDispatcher::AbortTick() {
        if (closed_ || draining_ || openTick_ == 0)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        pending_.resize(tickStart_);
        openTick_ = 0;
        tickFailed_ = false;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::PendingLess */
    bool CinematicEventDispatcher::PendingLess(const Pending &left, const Pending &right) noexcept {
        if (left.tick != right.tick)
            return left.tick < right.tick;
        if (left.occurrence.traversal != right.occurrence.traversal)
            return left.occurrence.traversal < right.occurrence.traversal;
        if (left.occurrence.direction != right.occurrence.direction)
            return left.occurrence.direction < right.occurrence.direction;
        if (left.occurrence.time != right.occurrence.time)
            return left.occurrence.direction == SequenceTraversalDirection::Reverse ? left.occurrence.time > right.occurrence.time
                                                                                    : left.occurrence.time < right.occurrence.time;
        if (left.priority != right.priority)
            return left.priority > right.priority;
        if (left.occurrence.player != right.occurrence.player)
            return left.occurrence.player < right.occurrence.player;
        if (left.occurrence.track != right.occurrence.track)
            return left.occurrence.track < right.occurrence.track;
        return left.occurrence.key < right.occurrence.key;
    }

    /** @copydoc CinematicEventDispatcher::InvokePending */
    EventDispatchOutcome CinematicEventDispatcher::InvokePending(const Pending &entry, const CookedEventKey &key) const noexcept {
        using enum EventDispatchOutcome;
        const Registration *handler = FindRegistration(key.binding);
        EventDispatchOutcome outcome = BindingUnavailable;
        const Player *player = FindPlayer(entry.occurrence.player);
        const bool failedTrack =
            player != nullptr && std::ranges::find(player->failedTracks, entry.occurrence.track) != player->failedTracks.end();
        if (handler != nullptr && (!handler->active || handler->handler.generation != entry.handlerGeneration))
            outcome = StaleBinding;
        else if (!failedTrack && handler != nullptr && handler->active && handler->handler.schema == key.schema &&
                 handler->handler.callback != nullptr) {
            try {
                outcome = handler->handler.callback(handler->handler.callbackContext,
                                                    {entry.occurrence, key.binding, key.schema, key.payload, entry.tick});
                if (outcome >= Count)
                    outcome = HandlerFailed;
            } catch (...) {
                outcome = HandlerFailed;
            }
        }

        return outcome;
    }

    /** @copydoc CinematicEventDispatcher::RecordTerminal */
    void CinematicEventDispatcher::RecordTerminal(const Pending &entry, const CookedEventKey &key, const EventDispatchOutcome outcome) {
        Player *player = FindPlayer(entry.occurrence.player);
        if (const bool failedTrack =
                player != nullptr && std::ranges::find(player->failedTracks, entry.occurrence.track) != player->failedTracks.end();
            outcome != EventDispatchOutcome::Accepted && outcome != EventDispatchOutcome::OperationStarted && player != nullptr &&
            !failedTrack)
            player->failedTracks.push_back(entry.occurrence.track);
        results_.emplace_back(entry.occurrence, key.binding, outcome, entry.tick);
        if (player != nullptr) {
            auto watermark = std::ranges::find_if(player->delivered, [&](const Player::DeliveryWatermark &delivered) {
                return delivered.track == entry.occurrence.track && delivered.key == entry.occurrence.key;
            });
            auto &traversal = entry.occurrence.direction == SequenceTraversalDirection::Reverse ? watermark->reverse : watermark->forward;
            traversal = std::max(traversal, entry.occurrence.traversal);
        }
    }

    /** @copydoc CinematicEventDispatcher::Drain */
    Result<std::size_t> CinematicEventDispatcher::Drain(const std::size_t maximumToDrain) {
        if (closed_ || draining_ || openTick_ != 0)
            return Failed<std::size_t>(EventTrackErrors::DispatchStateInvalid);
        if (maximumToDrain == 0)
            return Result<std::size_t>::Success(0);
        if (!pending_.empty() && results_.size() == maximumResults_)
            return Failed<std::size_t>(EventTrackErrors::DispatchCapacityExceeded);
        std::ranges::sort(pending_, PendingLess);
        draining_ = true;
        const std::size_t count = std::min({maximumToDrain, pending_.size(), maximumResults_ - results_.size()});
        for (std::size_t index = 0; index < count; ++index) {
            const Pending &entry = pending_[index];
            const CookedEventKey &key = *entry.plan->Find(entry.occurrence.track, entry.occurrence.key);
            RecordTerminal(entry, key, InvokePending(entry, key));
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
        draining_ = false;
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc CinematicEventDispatcher::Results */
    std::span<const EventDispatchRecord> CinematicEventDispatcher::Results() const noexcept {
        return results_;
    }

    /** @copydoc CinematicEventDispatcher::ClearResults */
    void CinematicEventDispatcher::ClearResults() noexcept {
        if (!draining_)
            results_.clear();
    }

    /** @copydoc CinematicEventDispatcher::Close */
    Result<void> CinematicEventDispatcher::Close() {
        if (draining_)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        pending_.clear();
        players_.clear();
        registrations_.clear();
        openTick_ = 0;
        closed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventDispatcher::StageHook */
    Result<void> CinematicEventDispatcher::StageHook(const BorrowedCallbackContext &context,
                                                     const std::span<const SequenceFrameEventOccurrence> occurrences) {
        auto *dispatcher = context.Get<CinematicEventDispatcher>();
        if (dispatcher == nullptr)
            return Failed<void>(EventTrackErrors::DispatchStateInvalid);
        return dispatcher->Stage(occurrences);
    }
}  // namespace Horo::Cinematic
