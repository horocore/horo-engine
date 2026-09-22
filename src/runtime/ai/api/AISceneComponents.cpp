#include "Horo/AI/AISceneComponents.h"

#include "Horo/AI/AIErrors.h"

#include <string>
#include <unordered_set>
#include <utility>

namespace Horo::AI {
    namespace {
        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            return Result<void>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] bool IsValid(const AiStartupPolicy policy) noexcept {
            return policy < AiStartupPolicy::Count;
        }

        [[nodiscard]] bool IsValid(const DecisionPlanKind kind) noexcept {
            return kind < DecisionPlanKind::Count;
        }
    }  // namespace

    /** @copydoc ValidateAiAgentComponent */
    Result<void> ValidateAiAgentComponent(const AiAgentComponent &component) {
        if (!component.agent.IsValid() || component.schemaVersion != CurrentAiSceneComponentSchemaVersion ||
            !IsValid(component.startupPolicy))
            return Failure(AIErrors::SceneComponentInvalid,
                           "AI agent components require a valid identity, schema version, and startup policy.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateAiControllerComponent */
    Result<void> ValidateAiControllerComponent(const AiControllerComponent &component) {
        if (!component.controller.IsValid() || !component.decisionAsset.IsValid() || !component.blackboardSchema.IsValid() ||
            !IsValid(component.decisionKind) || component.schemaVersion != CurrentAiSceneComponentSchemaVersion ||
            !IsValid(component.startupPolicy) || !component.requiredCapabilities.IsValid())
            return Failure(AIErrors::SceneComponentInvalid,
                           "AI controller components require valid controller, decision, schema, capability, and policy values.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateAiControllerDescriptor */
    Result<void> ValidateAiControllerDescriptor(const AiControllerDescriptor &descriptor) {
        if (!descriptor.controller.IsValid() || !descriptor.decisionAsset.IsValid() || !descriptor.blackboardSchema ||
            !descriptor.decisionPlan || !IsValid(descriptor.decisionKind))
            return Failure(AIErrors::ControllerDescriptorIncompatible,
                           "AI controller descriptors require valid identities and admitted immutable dependencies.");
        const std::shared_ptr<const BlackboardSchema> planSchema = descriptor.decisionPlan->BlackboardSchema();
        if (!planSchema || descriptor.decisionPlan->Asset() != descriptor.decisionAsset ||
            descriptor.decisionPlan->Kind() != descriptor.decisionKind || planSchema != descriptor.blackboardSchema ||
            descriptor.blackboardSchema->Identity() != planSchema->Identity() ||
            descriptor.blackboardSchema->Version() != planSchema->Version())
            return Failure(AIErrors::ControllerDescriptorIncompatible,
                           "AI controller descriptor dependencies do not agree on decision asset, kind, or blackboard schema.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateAiSceneComponents */
    Result<void> ValidateAiSceneComponents(const std::span<const AiSceneComponentView> components) {
        std::unordered_set<std::uint64_t> agents;
        agents.reserve(components.size());
        for (const AiSceneComponentView component : components) {
            if (component.agent != nullptr) {
                if (const Result<void> valid = ValidateAiAgentComponent(*component.agent); valid.HasError())
                    return valid;
                if (!agents.insert(component.agent->agent.Value()).second)
                    return Failure(AIErrors::DescriptorConflict, "AI agent identities must be unique within one committed Scene snapshot.");
            }
            if (component.controller != nullptr) {
                if (const Result<void> valid = ValidateAiControllerComponent(*component.controller); valid.HasError())
                    return valid;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateAiControllerBinding */
    Result<void> ValidateAiControllerBinding(const AiControllerComponent &component, const AiControllerDescriptor &descriptor) {
        if (const Result<void> valid = ValidateAiControllerComponent(component); valid.HasError())
            return valid;
        if (const Result<void> valid = ValidateAiControllerDescriptor(descriptor); valid.HasError())
            return valid;
        if (component.controller != descriptor.controller || component.decisionAsset != descriptor.decisionAsset ||
            component.decisionKind != descriptor.decisionKind || component.blackboardSchema != descriptor.blackboardSchema->Identity())
            return Failure(AIErrors::ControllerDescriptorIncompatible,
                           "The authored AI controller does not match its admitted descriptor.");
        return Result<void>::Success();
    }
}  // namespace Horo::AI
