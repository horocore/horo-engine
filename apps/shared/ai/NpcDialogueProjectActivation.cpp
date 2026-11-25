#include "NpcDialogueProjectActivation.h"

#include <algorithm>
#include <utility>

namespace Horo::App {
    namespace {
        /** @brief Requires explicit approval matching exact authored bytes, route and lifetime. */
        bool Matches(const Application::ProjectSourceDocument &project, const AI::NpcDialogueRoute &route, std::uint64_t generation,
                     const NpcDialogueEndpointApproval *approval) noexcept {
            return approval && project.sourceFingerprint == approval->projectSource && approval->endpoint.route == route &&
                   approval->endpoint.generation == generation;
        }

        /** @brief Rejects malformed grants before copying the intersection. */
        bool ValidFields(std::span<const AI::NpcDialogueFactId> fields) noexcept {
            return fields.size() <= AI::NpcDialogueTurn::MaximumFacts && (fields.empty() || fields.front().IsValid()) &&
                   std::ranges::is_sorted(fields) && std::ranges::adjacent_find(fields) == fields.end();
        }
    }  // namespace

    /** @copydoc NpcDialogueProjectActivation::Create */
    Result<std::optional<NpcDialogueProjectActivation>> NpcDialogueProjectActivation::Create(
        const Application::ProjectSourceDocument &project, std::uint64_t generation, const NpcDialogueEndpointApproval *approval) {
        using Activation = Result<std::optional<NpcDialogueProjectActivation>>;
        const auto ai = Application::ResolveProjectAiSettings(project);
        if (ai.HasError())
            return Activation::Failure(ai.ErrorValue());
        if (!ai.Value() || !ai.Value()->NpcDialogue())
            return Activation::Success(std::nullopt);
        if (generation == 0)
            return Activation::Failure(MakeError(AI::AIErrors::TaskContextInvalid));

        const auto &policy = *ai.Value()->NpcDialogue();
        NpcDialogueProjectActivation activation;
        activation.route_ = policy.route;
        activation.generation_ = generation;
        activation.triggers_ = policy.triggers;
        if (!Matches(project, policy.route, generation, approval))
            return Activation::Success(std::move(activation));
        if (!ValidFields(approval->fields))
            return Activation::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        auto binding = MakeNpcDialogueHttpsProvider(approval->endpoint);
        if (binding.HasError())
            return Activation::Failure(std::move(binding).ErrorValue());

        const auto common = std::ranges::set_intersection(policy.fields, approval->fields, activation.fields_.begin());
        activation.count_ = static_cast<std::size_t>(common.out - activation.fields_.begin());
        activation.binding_ = std::move(binding).Value();
        return Activation::Success(std::move(activation));
    }

    /** @copydoc NpcDialogueProjectActivation::Grant */
    AI::NpcDialogueGrant NpcDialogueProjectActivation::Grant() const noexcept {
        return {route_, generation_, std::span{fields_}.first(count_)};
    }

    /** @copydoc NpcDialogueProjectActivation::Binding */
    const std::optional<AI::NpcDialogueProviderBinding> &NpcDialogueProjectActivation::Binding() const noexcept {
        return binding_;
    }

    /** @copydoc NpcDialogueProjectActivation::Triggers */
    std::span<const Application::ProjectNpcDialogueTrigger> NpcDialogueProjectActivation::Triggers() const noexcept {
        return triggers_;
    }
}  // namespace Horo::App
