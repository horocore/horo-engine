#include "EditorProjectSceneHostOwner.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Scene/CookedSceneDefinition.h"

namespace Horo::Application::Internal::EditorProjectSceneDetail {
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
        return RegisterProjectSceneNavigation(*scenes_, *navigation_, captured.Value());
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
