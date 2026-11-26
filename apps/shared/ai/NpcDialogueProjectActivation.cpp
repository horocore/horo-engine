#include "NpcDialogueProjectActivation.h"

#include <algorithm>
#include <utility>

namespace Horo::App {
    namespace {
        /** @brief Requires explicit approval matching the actual project root, authored bytes, route and lifetime. */
        bool Matches(const std::filesystem::path &projectRoot, const Application::ProjectSourceDocument &project,
                     const AI::NpcDialogueRoute &route, std::uint64_t generation, const NpcDialogueEndpointApproval *approval) {
            if (!approval || !approval->projectRoot.is_absolute() || project.sourceFingerprint != approval->projectSource ||
                approval->endpoint.route != route || approval->endpoint.generation != generation)
                return false;
            std::error_code error;
            const bool sameRoot = std::filesystem::equivalent(projectRoot, approval->projectRoot, error);
            return !error && sameRoot;
        }
    }  // namespace

    /** @copydoc NpcDialogueProjectActivation::Create */
    Result<std::optional<NpcDialogueProjectActivation>> NpcDialogueProjectActivation::Create(
        const std::filesystem::path &projectRoot, std::uint64_t generation, const NpcDialogueEndpointApproval *approval) {
        using Activation = Result<std::optional<NpcDialogueProjectActivation>>;
        const auto project = Application::LoadProjectSourceDocument(projectRoot);
        if (project.HasError())
            return Activation::Failure(project.ErrorValue());
        const auto ai = Application::ResolveProjectAiSettings(project.Value());
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
        activation.presentation_ = policy.presentation;
        if (!Matches(projectRoot, project.Value(), policy.route, generation, approval))
            return Activation::Success(std::move(activation));
        const AI::NpcDialogueGrant approved{policy.route, generation, approval->fields};
        if (!approved.HasCanonicalFields())
            return Activation::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        auto binding = MakeNpcDialogueHttpsProvider(approval->endpoint);
        if (binding.HasError())
            return Activation::Failure(std::move(binding).ErrorValue());

        const auto common = std::ranges::set_intersection(policy.fields, approved.fields, activation.fields_.begin());
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

    /** @copydoc NpcDialogueProjectActivation::Presentation */
    const std::optional<Application::ProjectNpcDialoguePresentation> &NpcDialogueProjectActivation::Presentation() const noexcept {
        return presentation_;
    }
}  // namespace Horo::App
