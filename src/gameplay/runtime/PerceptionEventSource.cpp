#include "Horo/Gameplay/PerceptionEventSource.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::Gameplay {
    namespace {
        /** @brief Checks acoustic metadata before evaluating radius or timing. */
        [[nodiscard]] bool ValidHearing(const AI::HearingPerceptionEvent &event) noexcept {
            return std::isfinite(event.intensity) && event.intensity > 0 && event.intensity <= 1 && std::isfinite(event.radiusMeters) &&
                   event.radiusMeters > 0 && event.lifetimeTicks != 0;
        }

        /** @brief Checks the exclusive upper bound without overflowing on long lifetimes. */
        [[nodiscard]] bool HearingActiveAt(const AI::HearingPerceptionEvent &event, std::uint64_t tick) noexcept {
            return tick >= event.emittedTick && tick - event.emittedTick < event.lifetimeTicks;
        }

        /** @brief Validates source generations against the current scene without granting a mutation capability. */
        struct EventValidation final {
            Runtime::RuntimeSceneView scene;
            std::uint64_t tick;

            [[nodiscard]] Result<void> Source(const AI::PerceptionSourceRef source) const {
                if (!source.IsValid() || source.sceneIncarnation != scene.RuntimeId().value)
                    return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
                if (const Runtime::EntityRef entity{scene.RuntimeId(), {source.slot, source.generation}}; scene.Get(entity).HasError())
                    return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> operator()(const AI::DamagePerceptionEvent &event) const {
                if (!std::isfinite(event.amount) || event.amount <= 0)
                    return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
                return Source(event.instigator);
            }

            [[nodiscard]] Result<void> operator()(const AI::ProximityPerceptionEvent &event) const {
                return Source(event.other);
            }

            [[nodiscard]] Result<void> operator()(const AI::TeamMessagePerceptionEvent &event) const {
                if (event.deliveryId == 0)
                    return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
                if (const auto valid = Source(event.sender); valid.HasError())
                    return valid;
                return Source(event.subject);
            }

            [[nodiscard]] Result<void> operator()(const AI::HearingPerceptionEvent &event) const {
                if (!ValidHearing(event) || tick < event.emittedTick)
                    return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
                if (!HearingActiveAt(event, tick))
                    return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
                return Source(event.source);
            }
        };

        /** @brief Computes a distance from exact global positions, preserving nearby differences at extreme coordinates. */
        [[nodiscard]] double DistanceMeters(const Math::WorldCoordinate64 &left, const Math::WorldCoordinate64 &right) {
            const auto a = left.Millimeters();
            const auto b = right.Millimeters();
            const auto separation = [](const std::int64_t x, const std::int64_t y) {
                const auto low = static_cast<std::uint64_t>(x < y ? x : y);
                const auto high = static_cast<std::uint64_t>(x < y ? y : x);
                return static_cast<double>(high - low) / 1000.0;
            };
            return std::hypot(separation(a[0], b[0]), separation(a[1], b[1]), separation(a[2], b[2]));
        }

        /** @brief Applies the captured recipient policy to each typed event's provenance. */
        struct EventFilter final {
            const AI::PerceptionSpatialSnapshot &snapshot;
            Runtime::EntityRef listener;

            [[nodiscard]] bool Allows(const AI::PerceptionSourceRef source, const AI::SenseTypeId sense) const {
                return snapshot.Allows(listener, {listener.runtime, {source.slot, source.generation}}, sense);
            }

            [[nodiscard]] bool operator()(const AI::DamagePerceptionEvent &event) const {
                return Allows(event.instigator, AI::SenseTypeIds::Damage);
            }

            [[nodiscard]] bool operator()(const AI::ProximityPerceptionEvent &event) const {
                return Allows(event.other, AI::SenseTypeIds::Touch);
            }

            [[nodiscard]] bool operator()(const AI::TeamMessagePerceptionEvent &event) const {
                return Allows(event.subject, AI::SenseTypeIds::Team) && Allows(event.sender, AI::SenseTypeIds::Team);
            }

            [[nodiscard]] bool operator()(const AI::HearingPerceptionEvent &event) const {
                const auto position = snapshot.ListenerPosition(listener);
                return Allows(event.source, AI::SenseTypeIds::Hearing) && position.HasValue() &&
                       DistanceMeters(event.location, position.Value()) <= event.radiusMeters;
            }
        };

        /** @brief Keeps coalescing inside one producer-authorized listener and simulation window. */
        [[nodiscard]] bool SharesAdmittedWindow(const AI::PerceptionEventDelivery &requested,
                                                 const CommittedPerceptionDelivery &candidate) noexcept {
            const auto &delivery = candidate.delivery;
            return candidate.filter == PerceptionFilterDecision::Accept && delivery.recipient == requested.recipient &&
                   delivery.listener == requested.listener && delivery.simulationTick == requested.simulationTick;
        }

        /** @brief Selects the newest admissible sound for the requested source, breaking ties by producer order. */
        [[nodiscard]] const CommittedPerceptionDelivery &CoalesceHearing(const std::span<const CommittedPerceptionDelivery> records,
                                                                         const std::size_t requested, const EventFilter &filter) {
            const auto &delivery = records[requested].delivery;
            const auto *sound = std::get_if<AI::HearingPerceptionEvent>(&delivery.event);
            if (!sound)
                return records[requested];
            auto selected = requested;
            auto emitted = sound->emittedTick;
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto &candidate = records[index];
                if (!SharesAdmittedWindow(delivery, candidate))
                    continue;
                const auto *next = std::get_if<AI::HearingPerceptionEvent>(&candidate.delivery.event);
                if (!next || next->source != sound->source || std::pair{next->emittedTick, index} <= std::pair{emitted, selected})
                    continue;
                if (!ValidHearing(*next) || !HearingActiveAt(*next, delivery.simulationTick) || !filter(*next))
                    continue;
                selected = index;
                emitted = next->emittedTick;
            }
            return records[selected];
        }

        /** @brief Private AI knowledge is admitted only in the host's current authoritative world. */
        [[nodiscard]] bool Authoritative(const Network::NetworkModeRoleView &role) noexcept {
            return role.role == Network::ReplicationExecutionRole::Standalone ||
                   role.role == Network::ReplicationExecutionRole::AuthorityServer;
        }

        /** @brief Individual bad input is drops-only. Invalidating the authority still fails the sensing window. */
        [[nodiscard]] bool IsRejectedDelivery(const Error &failure) noexcept {
            const auto code = failure.code.Value();
            return code == AI::AIErrors::PerceptionEventInvalid.code.Value() || code == AI::AIErrors::PerceptionEventFiltered.code.Value() ||
                   code == AI::AIErrors::PerceptionEventStale.code.Value();
        }
    }  // namespace

    PerceptionEventSource::PerceptionEventSource(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai,
                                                 const Network::NetworkModeComposition &host, const CapturedAuthority &authority,
                                                 const AI::PerceptionDescriptorRegistry &registry,
                                                 const AI::PerceptionSpatialBroadphase &filtering,
                                                 const std::span<const CommittedPerceptionDelivery> deliveries)
        : scene_(&scene), ai_(&ai), host_(&host), role_(authority.role), binding_(authority.binding), registry_(&registry),
          filtering_(&filtering), filterSnapshot_(filtering.Current()), deliveries_(deliveries.begin(), deliveries.end()) {}

    /** @copydoc PerceptionEventSource::Capture */
    Result<PerceptionEventSource> PerceptionEventSource::Capture(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai,
                                                                 const Network::NetworkModeComposition &host,
                                                                 const Network::NetworkModeWorldKind world,
                                                                 const AI::PerceptionDescriptorRegistry &registry,
                                                                 const AI::PerceptionSpatialBroadphase &filtering,
                                                                 const std::span<const CommittedPerceptionDelivery> deliveries) {
        if (deliveries.size() > MaximumDeliveries)
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
        const auto role = host.Role(world, scene.View().RuntimeId());
        if (role.HasError() || !Authoritative(role.Value()))
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventUnauthorized));
        const auto binding = ai.ActiveBinding();
        if (binding.HasError() || binding.Value().scene != role.Value().scene)
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        if (!filtering.Current() || filtering.Current()->Scene() != binding.Value().scene)
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        return Result<PerceptionEventSource>::Success(
            PerceptionEventSource{scene, ai, host, {role.Value(), binding.Value()}, registry, filtering, deliveries});
    }

    /** @copydoc PerceptionEventSource::ValidateCurrent */
    Result<void> PerceptionEventSource::ValidateCurrent() const {
        if (closed_ || host_->ValidateRole(role_).HasError())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventUnauthorized));
        if (filtering_->Current() != filterSnapshot_)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        if (const auto binding = ai_->ActiveBinding();
            binding.HasError() || binding.Value() != binding_ || scene_->View().RuntimeId() != binding_.scene)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        return Result<void>::Success();
    }

    /** @copydoc PerceptionEventSource::ValidateRecipient */
    Result<void> PerceptionEventSource::ValidateRecipient(const AI::PerceptionEventDelivery &delivery,
                                                          const AI::AIPerceptionMemory &memory) const {
        if (delivery.recipient != memory.Agent() || memory.SceneIncarnation() != binding_.scene.value)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
        const auto recipient = ai_->Find(delivery.recipient);
        if (recipient.HasError())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        if (recipient.Value().state != AI::AiAgentActivationState::Active ||
            !recipient.Value().stagedCapabilities.Contains(AI::AiCapability::Perception))
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventUnauthorized));
        if (scene_->View().Get(recipient.Value().owner).HasError())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        return Result<void>::Success();
    }

    /** @copydoc PerceptionEventSource::Deliver */
    Result<void> PerceptionEventSource::Deliver(const std::size_t index, AI::AIPerceptionMemory &memory) const {
        if (index >= deliveries_.size())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
        if (const auto current = ValidateCurrent(); current.HasError())
            return current;
        const auto &record = deliveries_[index];
        if (const auto recipient = ValidateRecipient(record.delivery, memory); recipient.HasError())
            return recipient;
        const EventValidation validate{scene_->View(), record.delivery.simulationTick};
        if (const auto event = std::visit(validate, record.delivery.event); event.HasError())
            return event;
        if (record.filter != PerceptionFilterDecision::Accept)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventFiltered));
        const auto listener = ai_->Find(record.delivery.recipient).Value().owner;
        const EventFilter filter{*filterSnapshot_, listener};
        if (!std::visit(filter, record.delivery.event))
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventFiltered));
        const auto &selected = CoalesceHearing(deliveries_, index, filter).delivery;
        const AI::PerceptionEventAdmission admission{selected, memory};
        return AI::RouteGameplayPerceptionEvent(selected, *registry_, admission, memory);
    }

    /** @copydoc PerceptionEventSource::Feed */
    AI::PerceptionEventFeed PerceptionEventSource::Feed() const & noexcept {
        return {this, *ai_, *filtering_,
                [](const void *owner) { return static_cast<const PerceptionEventSource *>(owner)->ValidateCurrent(); },
                [](const void *owner, std::uint64_t tick, const AI::AiControllerPerceptionEndpoints &endpoints,
                   AI::AIPerceptionMemory &memory) {
                    return static_cast<const PerceptionEventSource *>(owner)->DeliverSensingWindow(tick, endpoints, memory);
                }};
    }

    /** @copydoc PerceptionEventSource::DeliverSensingWindow */
    Result<void> PerceptionEventSource::DeliverSensingWindow(std::uint64_t tick, const AI::AiControllerPerceptionEndpoints &endpoints,
                                                             AI::AIPerceptionMemory &memory) const {
        if (const auto current = ValidateCurrent(); current.HasError())
            return current;
        for (std::size_t index = 0; index < deliveries_.size(); ++index) {
            const auto &delivery = deliveries_[index].delivery;
            if (delivery.simulationTick != tick || delivery.recipient != memory.Agent() ||
                !std::holds_alternative<AI::HearingPerceptionEvent>(delivery.event) ||
                !std::ranges::binary_search(endpoints.listeners, delivery.listener))
                continue;
            if (registry_ != endpoints.registry.get())
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
            if (auto delivered = Deliver(index, memory); delivered.HasError() && !IsRejectedDelivery(delivered.ErrorValue()))
                return delivered;
        }
        return Result<void>::Success();
    }

    /** @copydoc PerceptionEventSource::Close */
    void PerceptionEventSource::Close() noexcept {
        closed_ = true;
    }
}  // namespace Horo::Gameplay
