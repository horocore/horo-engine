#include "EditorProjectSceneHostOwner.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Scene/CookedSceneDefinition.h"

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @brief Reads the configured Navigation registry for only the exact selected entity generation. */
    std::optional<Editor::EditorPlayAgentNavigation> ProjectPlaySceneHost::ReadAgentNavigation(const Runtime::EntityRef owner) const {
        if (!navigation_)
            return std::nullopt;
        const auto binding = navigation_->agents.ActiveBinding();
        if (binding.HasError() || binding.Value().scene.Value() != owner.runtime.value)
            return std::nullopt;
        const auto handle = navigation_->agents.FindOwner({binding.Value().scene, owner.entity.index, owner.entity.generation});
        if (!handle)
            return std::nullopt;
        const auto record = navigation_->agents.Find(*handle);
        if (record.HasError())
            return std::nullopt;
        return Editor::EditorPlayAgentNavigation{record.Value().profile.Value(), record.Value().filter.Value(), record.Value().enabled};
    }

    void ProjectPlaySceneHost::CommitQueryNavigation(const std::uint64_t tick) {
        queryNavigationPublication_.reset();
        if (!navigation_ || !navigationSelection_ || !navigation_->assets)
            return;
        const auto world = navigation_->assets->ActiveWorld();
        if (!world || world->State() != Navigation::NavigationWorldLifecycleState::Active)
            return;
        const auto active = world->ActiveDescriptor();
        if (active.HasError())
            return;
        const auto &identity = active.Value();
        const auto epoch = identity.topology.Value();
        // The cooked static provider seals obstacle, filter and coordinate revisions with its topology.
        const Navigation::NavigationOutcomeProvenance root{.snapshot = Navigation::NavigationSnapshotToken::Create(epoch).Value(),
                                                           .world = identity.world,
                                                           .topology = identity.topology,
                                                           .obstacleRevision = epoch,
                                                           .filterRevision = epoch,
                                                           .profileRevision = navigationSelection_->policy.Revision().Value(),
                                                           .originRevision = epoch};
        queryNavigationCallers_.front() = {Navigation::NavigationDynamicOwnerId::Create(identity.world.Value()).Value(),
                                           Navigation::NavigationDynamicOwnerGeneration::Create(identity.sceneGeneration.Value()).Value()};
        queryNavigationPublication_.emplace(identity, root, queryNavigationCallers_, tick);
        static_cast<void>(navigation_->coordinator.Commit(*queryNavigationPublication_));
    }

    std::optional<AI::QueryNavigationAccess> ProjectPlaySceneHost::ReadQueryNavigation(const Runtime::RuntimeSceneView &scene,
                                                                                       Runtime::EntityRef querier,
                                                                                       const std::uint64_t tick) const noexcept {
        if (!queryNavigationPublication_ || queryNavigationPublication_->tick != tick ||
            queryNavigationPublication_->activation.scene.Value() != scene.RuntimeId().value)
            return std::nullopt;
        try {
            const auto agent = ReadAgentNavigation(querier);
            if (!agent || !agent->enabled)
                return std::nullopt;
            const auto filter = Navigation::NavigationFilterId::Create(agent->filter);
            const auto world = navigation_->assets->ActiveWorld();
            if (filter.HasError() || !world || world->State() != Navigation::NavigationWorldLifecycleState::Active)
                return std::nullopt;
            const auto current = world->ActiveDescriptor();
            if (current.HasError() || current.Value() != queryNavigationPublication_->activation)
                return std::nullopt;
            return AI::QueryNavigationAccess{*world,
                                             navigation_->coordinator.ObservePathRequests(),
                                             *queryNavigationPublication_,
                                             queryNavigationCallers_.front(),
                                             filter.Value(),
                                             navigationSelection_->policy.MaximumQuery(),
                                             std::numeric_limits<std::uint64_t>::max()};
        } catch (const std::bad_alloc &) {
            return std::nullopt;
        }
    }

    namespace {
        /** @brief Re-encodes the active projection with the committed identity so unrelated live revisions cannot admit its topology. */
        [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePlayProjection(const Runtime::RuntimeSceneDefinition &live,
                                                                             const Runtime::RuntimeSceneDefinition &committed) {
            using Bytes = Result<std::vector<std::uint8_t>>;
            Runtime::SceneDefinitionBuilder builder{committed.Id(), committed.Revision()};
            for (const auto &entity : live.Entities())
                builder.Add(entity);
            for (const auto &dependency : live.AssetDependencies())
                if (const auto required = builder.RequireAsset(dependency); required.HasError())
                    return Bytes::Failure(required.ErrorValue());
            for (const auto &trigger : live.TriggerVolumeEvidence())
                if (const auto required = builder.RequireTriggerVolume(trigger); required.HasError())
                    return Bytes::Failure(required.ErrorValue());
            const auto normalized = std::move(builder).Build();
            if (normalized.HasError())
                return Bytes::Failure(normalized.ErrorValue());
            return SceneCook::EncodeCookedSceneDefinition(normalized.Value());
        }

    }  // namespace

    /** @copydoc MatchCommittedPlayProjection */
    Result<void> MatchCommittedPlayProjection(const Runtime::RuntimeSceneDefinition &live, const Runtime::RuntimeSceneDefinition &committed,
                                              const ErrorCodeDescriptor &stale) {
        const auto encoded = EncodePlayProjection(live, committed);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        const auto expected = SceneCook::EncodeCookedSceneDefinition(committed);
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        if (encoded.Value() != expected.Value())
            return Result<void>::Failure(MakeError(stale, "The active Play Scene differs from the selected committed Scene."));
        return Result<void>::Success();
    }

    /** @copydoc ProjectPlaySceneHost::PrepareNavigation */
    Result<void> ProjectPlaySceneHost::PrepareNavigation(const std::filesystem::path &root, JobSystem &jobs,
                                                         const EditorProjectScenePolicy &policy,
                                                         const Runtime::RuntimeSceneDefinition *definition) {
        if (!policy.playNavigationScene)
            return Result<void>::Success();
        if (!definition || !policy.cooked || !resources_.assets_ || identity_.value < Editor::EditorFirstPlayScene.value)
            return Result<void>::Failure(
                MakeError(Navigation::NavigationErrors::NoNavigationData, "Editor Navigation requires a committed Play Scene selection."));
        const auto selected = resources_.assets_->DecodeScene(*policy.playNavigationScene);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        if (const auto matched = MatchCommittedPlayProjection(*definition, selected.Value(), Navigation::NavigationErrors::StaleSnapshot);
            matched.HasError())
            return matched;

        const auto metadata = LoadProjectMetadata(root);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        const auto files = Platform::CreateNativeProjectReadFiles(root, metadata.Value().projectId, 1);
        if (files.HasError())
            return Result<void>::Failure(files.ErrorValue());
        const Platform::ProjectReadContext context{.projectIdentity = metadata.Value().projectId,
                                                   .projectGeneration = 1,
                                                   .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30}};
        const auto captured = CaptureProjectSceneNavigation({*files.Value(), context, resources_.registry_.Snapshot(), selected.Value(),
                                                             *policy.playNavigationScene, policy.cooked->target, jobs});
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        auto navigation = CreateProjectSceneNavigationWorld(*captured.Value());
        if (navigation.HasError())
            return Result<void>::Failure(navigation.ErrorValue());
        navigation_ = std::move(navigation).Value();
        navigationSelection_ = captured.Value();
        return RegisterProjectSceneNavigation(*scenes_, *navigation_, navigationSelection_);
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
