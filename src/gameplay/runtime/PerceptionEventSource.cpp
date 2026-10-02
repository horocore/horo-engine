#include "Horo/Gameplay/PerceptionEventSource.h"

#include "Horo/AI/AIErrors.h"

#include <cmath>

namespace Horo::Gameplay {
    namespace {
        /** @brief Validates source generations against the current scene without granting a mutation capability. */
        struct EventValidation final {
            Runtime::RuntimeSceneView scene;

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
        };

        /** @brief Private AI knowledge is admitted only in the host's current authoritative world. */
        [[nodiscard]] bool Authoritative(const Network::NetworkModeRoleView &role) noexcept {
            return role.role == Network::ReplicationExecutionRole::Standalone ||
                   role.role == Network::ReplicationExecutionRole::AuthorityServer;
        }
    }  // namespace

    PerceptionEventSource::PerceptionEventSource(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai,
                                                 const Network::NetworkModeComposition &host, const Network::NetworkModeRoleView &role,
                                                 AI::AiSceneActivationBinding binding, const AI::PerceptionDescriptorRegistry &registry,
                                                 const std::span<const CommittedPerceptionDelivery> deliveries)
        : scene_(&scene), ai_(&ai), host_(&host), role_(role), binding_(binding), registry_(&registry),
          deliveries_(deliveries.begin(), deliveries.end()) {}

    /** @copydoc PerceptionEventSource::Capture */
    Result<PerceptionEventSource> PerceptionEventSource::Capture(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai,
                                                                 const Network::NetworkModeComposition &host,
                                                                 const Network::NetworkModeWorldKind world,
                                                                 const AI::PerceptionDescriptorRegistry &registry,
                                                                 const std::span<const CommittedPerceptionDelivery> deliveries) {
        if (deliveries.size() > MaximumDeliveries)
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventInvalid));
        const auto role = host.Role(world, scene.View().RuntimeId());
        if (role.HasError() || !Authoritative(role.Value()))
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventUnauthorized));
        const auto binding = ai.ActiveBinding();
        if (binding.HasError() || binding.Value().scene != role.Value().scene)
            return Result<PerceptionEventSource>::Failure(MakeError(AI::AIErrors::PerceptionEventStale));
        return Result<PerceptionEventSource>::Success(
            PerceptionEventSource{scene, ai, host, role.Value(), binding.Value(), registry, deliveries});
    }

    /** @copydoc PerceptionEventSource::ValidateCurrent */
    Result<void> PerceptionEventSource::ValidateCurrent() const {
        if (closed_ || host_->ValidateRole(role_).HasError())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventUnauthorized));
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
        if (const auto event = std::visit(EventValidation{scene_->View()}, record.delivery.event); event.HasError())
            return event;
        if (record.filter != PerceptionFilterDecision::Accept)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionEventFiltered));
        const AI::PerceptionEventAdmission admission{record.delivery, memory};
        return AI::RouteGameplayPerceptionEvent(record.delivery, *registry_, admission, memory);
    }

    /** @copydoc PerceptionEventSource::Close */
    void PerceptionEventSource::Close() noexcept {
        closed_ = true;
    }
}  // namespace Horo::Gameplay
