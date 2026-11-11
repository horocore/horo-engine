#include "CinematicVfxEventAdapterState.h"
#include "Horo/Cinematic/EventTrackErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Cinematic {
    namespace {
        /** @brief Admits only finite runtime registration metadata; Preview has no committed VFX source route. */
        bool ValidBinding(const VfxEventBinding &binding) noexcept {
            using enum Horo::Cinematic::EventRuntimeContext;
            const bool context = binding.context == Runtime || binding.context == Pie || binding.context == Headless;
            return binding.binding.IsValid() && binding.schema.IsValid() && binding.generation != 0 && context &&
                   !binding.semanticEvent.empty() && binding.semanticEvent.size() <= Gameplay::GameplayVfxBindingLimits::TextBytes &&
                   binding.semanticSchemaVersion != 0;
        }

        /** @brief Checks independently finite key and retained-result envelopes before allocation. */
        bool ValidBudget(const VfxEventBinding &binding) noexcept {
            return binding.maximumKeys != 0 && binding.maximumKeys <= VfxEventDetail::MaximumKeys && binding.maximumResults != 0 &&
                   binding.maximumResults <= MaximumFrameOccurrences;
        }

        /** @brief Checks the cold capability and metadata together without dereferencing an absent producer. */
        bool Available(const VfxEventBinding &binding, const std::shared_ptr<const CookedEventPlan> &plan,
                       const std::shared_ptr<const Gameplay::GameplayVfxCommittedProducer> &producer) noexcept {
            return ValidBinding(binding) && plan && producer && producer->IsUsable();
        }

        /** @brief Identifies semantic target/schema rejections without flattening the retained source receipt status. */
        bool InvalidTarget(const Vfx::VfxSpawnStatus status) noexcept {
            using enum Vfx::VfxSpawnStatus;
            switch (status) {
                case UnknownEvent:
                case UnknownTag:
                case InvalidRequest:
                case InvalidTransform:
                case InvalidImpactNormal:
                case InvalidParameters:
                case ParameterSchemaMismatch:
                    return true;
                default:
                    return false;
            }
        }

        /** @brief Separates adapter/source protocol failures from unavailable presentation capabilities. */
        bool HandlerFailure(const Vfx::VfxSpawnStatus status) noexcept {
            using enum Vfx::VfxSpawnStatus;
            switch (status) {
                case OutOfOrder:
                case ThreadViolation:
                case Reentrant:
                case SimulationFailed:
                    return true;
                default:
                    return false;
            }
        }

        /** @brief Converts source acceptance into the existing dispatcher outcome vocabulary without claiming an effect completed. */
        EventDispatchOutcome SourceOutcome(const Vfx::VfxSpawnStatus status) noexcept {
            if (InvalidTarget(status))
                return EventDispatchOutcome::InvalidTarget;
            if (HandlerFailure(status))
                return EventDispatchOutcome::HandlerFailed;
            using enum Vfx::VfxSpawnStatus;
            switch (status) {
                case Accepted:
                    return EventDispatchOutcome::Accepted;
                case PermissionDenied:
                    return EventDispatchOutcome::SuppressedByAuthority;
                case StaleBinding:
                    return EventDispatchOutcome::StaleBinding;
                case EventQueueFull:
                case ResultQueueFull:
                case ResourceBudgetExceeded:
                    return EventDispatchOutcome::Backpressured;
                default:
                    return EventDispatchOutcome::CapabilityUnavailable;
            }
        }

        /** @brief Checks representation only; dispatcher remains the sole session/traversal admission and dedup authority. */
        bool ValidOccurrence(const EventDispatchRequest &request) noexcept {
            const auto &occurrence = request.occurrence;
            return occurrence.player.IsValid() && occurrence.track.IsValid() && occurrence.key.IsValid() &&
                   occurrence.direction < SequenceTraversalDirection::Count && occurrence.traversal != 0 && request.committedTick != 0;
        }

        /** @brief Revalidates exact authored time, reverse policy and borrowed canonical bytes without decoding. */
        bool MatchesSource(const CookedEventKey &key, const EventDispatchRequest &request) noexcept {
            const bool direction = request.occurrence.direction == SequenceTraversalDirection::Forward || key.fireInReverse;
            return key.time == request.occurrence.time && direction && request.payload.size() == key.payload.size() &&
                   std::equal(request.payload.begin(), request.payload.end(), key.payload.begin());
        }
    }  // namespace

    /** @copydoc CinematicVfxEventAdapter::State::PrepareKeys */
    Result<void> CinematicVfxEventAdapter::State::PrepareKeys(const VfxEventPayloadProjection &projection) {
        if (plan->Keys().size() > binding.maximumKeys)
            return Result<void>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
        keys.reserve(plan->Keys().size());
        std::size_t bytes{};
        for (const auto &key : plan->Keys()) {
            if (key.payload.size() > VfxEventDetail::MaximumPayloadBytes || key.payload.size() > VfxEventDetail::MaximumPlanBytes - bytes)
                return Result<void>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
            bytes += key.payload.size();
            if (key.binding != binding.binding)
                continue;
            if (key.schema != binding.schema || (key.allowedContexts & static_cast<std::byte>(binding.context)) == std::byte{})
                return Result<void>::Failure(MakeError(EventTrackErrors::SchemaMismatch));
            auto payload = VfxEventDetail::PreparePayload(key.payload, projection);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            keys.push_back({&key, std::move(payload).Value()});
        }
        return keys.empty() ? Result<void>::Failure(MakeError(EventTrackErrors::BindingUnavailable)) : Result<void>::Success();
    }

    /** @copydoc CinematicVfxEventAdapter::Create */
    Result<std::shared_ptr<CinematicVfxEventAdapter>> CinematicVfxEventAdapter::Create(
        VfxEventBinding binding, std::shared_ptr<const CookedEventPlan> plan,
        std::shared_ptr<const Gameplay::GameplayVfxCommittedProducer> producer, VfxEventPayloadProjection projection) {
        using Output = Result<std::shared_ptr<CinematicVfxEventAdapter>>;
        if (!Available(binding, plan, producer))
            return Output::Failure(MakeError(EventTrackErrors::BindingUnavailable));
        if (!ValidBudget(binding))
            return Output::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
        if (!VfxEventDetail::ValidProjection(projection))
            return Output::Failure(MakeError(EventTrackErrors::SchemaMismatch));
        try {
            auto state = std::make_shared<State>();
            state->semantic = producer->ResolveEvent(binding.semanticEvent, binding.semanticSchemaVersion);
            if (state->semantic.status != Vfx::VfxSpawnStatus::Accepted)
                return Output::Failure(MakeError(EventTrackErrors::BindingUnavailable));
            if (state->semantic.payloadSchema != projection.parameterSchema)
                return Output::Failure(MakeError(EventTrackErrors::SchemaMismatch));
            state->binding = std::move(binding);
            state->plan = std::move(plan);
            state->producer = std::move(producer);
            if (auto prepared = state->PrepareKeys(projection); prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            state->results.resize(state->binding.maximumResults);
            return Output::Success(std::shared_ptr<CinematicVfxEventAdapter>{new CinematicVfxEventAdapter(std::move(state))});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Gameplay::GameplayVfxErrors::AllocationFailed));
        }
    }

    /** @copydoc CinematicVfxEventAdapter::State::FindRequest */
    const CinematicVfxEventAdapter::State::PreparedKey *CinematicVfxEventAdapter::State::FindRequest(
        const EventDispatchRequest &request) const noexcept {
        if (request.binding != binding.binding || request.schema != binding.schema || !ValidOccurrence(request))
            return nullptr;
        const auto key = std::ranges::find_if(keys, [&](const PreparedKey &candidate) {
            return candidate.source->track == request.occurrence.track && candidate.source->key == request.occurrence.key;
        });
        if (key == keys.end() || !MatchesSource(*key->source, request))
            return nullptr;
        return &*key;
    }

    /** @copydoc CinematicVfxEventAdapter::State::Dispatch */
    EventDispatchOutcome CinematicVfxEventAdapter::State::Dispatch(const EventDispatchRequest &request) noexcept {
        using enum Horo::Cinematic::EventDispatchOutcome;
        if (std::this_thread::get_id() != lane || busy)
            return HandlerFailed;
        if (revoked)
            return BindingUnavailable;
        const auto *key = FindRequest(request);
        if (!key)
            return InvalidTarget;
        if (resultCount == results.size())
            return Backpressured;
        busy = true;
        auto &record = results[resultCount++];
        record = {request.occurrence, request.committedTick, {}};
        record.submission = producer->Emit(semantic.key, key->payload, request.committedTick);
        busy = false;
        return SourceOutcome(record.submission.status);
    }

    /** @copydoc CinematicVfxEventAdapter::State::Invoke */
    EventDispatchOutcome CinematicVfxEventAdapter::State::Invoke(const BorrowedCallbackContext &context,
                                                                 const EventDispatchRequest &request) noexcept {
        auto *state = context.Get<State>();
        return state ? state->Dispatch(request) : EventDispatchOutcome::HandlerFailed;
    }

    CinematicVfxEventAdapter::CinematicVfxEventAdapter(std::shared_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc CinematicVfxEventAdapter::~CinematicVfxEventAdapter */
    CinematicVfxEventAdapter::~CinematicVfxEventAdapter() = default;

    /** @copydoc CinematicVfxEventAdapter::Registration */
    EventHandlerRegistration CinematicVfxEventAdapter::Registration() const {
        if (std::this_thread::get_id() != state_->lane || state_->busy || state_->revoked)
            return {};
        const auto &binding = state_->binding;
        return {binding.binding, binding.schema, binding.context, binding.generation, BorrowedCallbackContext{state_.get()},
                State::Invoke,   state_};
    }

    /** @copydoc CinematicVfxEventAdapter::Results */
    std::span<const VfxEventDispatchRecord> CinematicVfxEventAdapter::Results() const noexcept {
        if (std::this_thread::get_id() != state_->lane || state_->busy)
            return {};
        return std::span{state_->results}.first(state_->resultCount);
    }

    /** @copydoc CinematicVfxEventAdapter::ClearResults */
    void CinematicVfxEventAdapter::ClearResults() noexcept {
        if (std::this_thread::get_id() == state_->lane && !state_->busy)
            state_->resultCount = 0;
    }

    /** @copydoc CinematicVfxEventAdapter::Revoke */
    void CinematicVfxEventAdapter::Revoke() noexcept {
        if (std::this_thread::get_id() == state_->lane)
            state_->revoked = true;
    }
}  // namespace Horo::Cinematic
