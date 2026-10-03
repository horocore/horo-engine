#include "Horo/AI/PerceptionEventRouting.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        /** @brief Keeps event kinds tied to their registered built-in descriptor identities. */
        struct EventProjection final {
            PerceptionListenerTypeId listener;

            /** @brief Projects committed damage facts without exposing combat state. */
            [[nodiscard]] PerceptionObservation operator()(const DamagePerceptionEvent &event) const {
                return {.key = {listener, SenseTypeIds::Damage, StimulusTypeIds::Damage, event.instigator}, .position = event.hitLocation};
            }

            /** @brief Projects committed contact facts without exposing physics state. */
            [[nodiscard]] PerceptionObservation operator()(const ProximityPerceptionEvent &event) const {
                return {.key = {listener, SenseTypeIds::Touch, StimulusTypeIds::Contact, event.other}, .position = event.contactPoint};
            }

            /** @brief Retains only weak message provenance and last-known facts. */
            [[nodiscard]] PerceptionObservation operator()(const TeamMessagePerceptionEvent &event) const {
                return {.key = {listener, SenseTypeIds::Team, StimulusTypeIds::Team, event.subject},
                        .position = event.lastKnownPosition,
                        .provenance = event.sender};
            }
        };

        /** @brief Checks an admitted listener's exact sense and stimulus pair. */
        [[nodiscard]] bool Accepts(const PerceptionDescriptorRegistry &registry, const PerceptionMemoryKey &key) {
            const auto listeners = registry.Listeners();
            const auto listener = std::ranges::find_if(listeners, [&key](const PerceptionListenerResolution &candidate) {
                return candidate.descriptor.identity == key.listener;
            });
            if (listener == listeners.end() || listener->availability != PerceptionListenerAvailability::Available ||
                listener->descriptor.sense != key.sense)
                return false;
            const auto accepted = std::span{listener->descriptor.stimuli}.first(listener->descriptor.stimulusCount);
            return std::ranges::any_of(accepted, [&key](const PerceptionStimulusRequirement &requirement) {
                return requirement.identity == key.stimulus;
            });
        }
    }  // namespace

    /** @copydoc RouteGameplayPerceptionEvent */
    Result<void> RouteGameplayPerceptionEvent(const PerceptionEventDelivery &delivery, const PerceptionDescriptorRegistry &registry,
                                              const PerceptionEventAdmission &admission, AIPerceptionMemory &memory) {
        if (admission.delivery_ != &delivery || admission.memory_ != &memory || delivery.recipient != memory.Agent())
            return Result<void>::Failure(MakeError(AIErrors::PerceptionEventInvalid));

        const auto observation = std::visit(EventProjection{delivery.listener}, delivery.event);
        if (!Accepts(registry, observation.key))
            return Result<void>::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
        return memory.ObserveAdmitted(observation, delivery.simulationTick);
    }
}  // namespace Horo::AI
