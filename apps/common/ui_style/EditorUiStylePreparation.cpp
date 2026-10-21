#include "EditorUiStylePreparation.h"

#include "Horo/Application/UiStyleWorkspace.h"
#include "Horo/Platform/NativeImmutableFileStorage.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Allocate a real native attempt identity without deriving/truncating a hash. */
        Result<Assets::AssetId> OperationId() {
            std::array<std::uint8_t, 16> bytes{};
            auto random = Platform::CreateNativeSecureRandomSource();
            if (const auto filled = random->Fill(std::as_writable_bytes(std::span{bytes})); filled.HasError())
                return Result<Assets::AssetId>::Failure(filled.ErrorValue());
            const auto id = Assets::AssetId::FromBytes(bytes);
            if (!id.IsValid())
                return Result<Assets::AssetId>::Failure(MakeError(Runtime::Ui::UiErrors::StyleInvalid));
            return Result<Assets::AssetId>::Success(id);
        }

        /** @brief Translate only immutable domain values; no Editor type crosses into the producer. */
        UiInspectorStyleProducer Adapter(const std::shared_ptr<Application::UiStyleWorkspaceProducer> &owner) {
            return {.themes = [](const Assets::AssetRegistrySnapshot &assets) {
                auto available = Application::UiStyleWorkspaceThemes(assets);
                if (available.HasError())
                    return Result<std::vector<UiInspectorTheme>>::Failure(available.ErrorValue());
                std::vector<UiInspectorTheme> themes;
                for (auto &theme : std::move(available).Value())
                    themes.push_back({theme.id, std::move(theme.label)});
                return Result<std::vector<UiInspectorTheme>>::Success(std::move(themes));
            }, .submit = [owner](Assets::AssetRegistrySnapshot assets, const Runtime::Ui::RuntimeStyleAssetId theme) -> Result<void> {
                const auto submitted = owner->Submit(std::move(assets), theme);
                return submitted.HasError() ? Result<void>::Failure(submitted.ErrorValue()) : Result<void>::Success();
            }, .poll = [owner](const Assets::AssetRegistryRevision assets) -> Result<std::optional<UiInspectorStyleCandidate>> {
                using R = Result<std::optional<UiInspectorStyleCandidate>>;
                try {
                    auto result = owner->TakeCompleted(assets);
                    if (result.HasError())
                        return R::Failure(result.ErrorValue());
                    if (!result.Value())
                        return R::Success(std::nullopt);
                    auto candidate = std::make_shared<Application::UiStyleWorkspaceCandidate>(std::move(*std::move(result).Value()));
                    auto sourceLease = std::move(candidate->sourceLease);
                    return R::Success(UiInspectorStyleCandidate{candidate->registry, candidate->theme, candidate->assets, candidate,
                                                                std::move(sourceLease)});
                } catch (const std::bad_alloc &) {
                    return R::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
                }
            }, .shutdown = [owner] {
                static_cast<void>(owner->Shutdown({WaitPolicy::MainThreadPumpAllowed, Duration::FromMilliseconds(30'000)}));
            }};
        }
    }  // namespace

    /** @copydoc MakeEditorUiInspectorResourcesFactory */
    UiInspectorResourcesFactory MakeEditorUiInspectorResourcesFactory(JobSystem &jobs, Assets::AssetRegistry &assets,
                                                                      ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        return [&jobs, &assets, &mutations, &files](const std::filesystem::path &root, const Assets::AssetId workspace) {
            return CreateEditorUiInspectorResources(root, workspace, jobs, assets, mutations, files);
        };
    }

    /** @copydoc CreateEditorUiInspectorResources */
    Result<std::unique_ptr<UiInspectorResources>> CreateEditorUiInspectorResources(const std::filesystem::path &root,
                                                                                   const Assets::AssetId workspace, JobSystem &jobs,
                                                                                   Assets::AssetRegistry &assets,
                                                                                   ProjectMutationCoordinator &mutations,
                                                                                   DurableFileSystem &files) {
        using R = Result<std::unique_ptr<UiInspectorResources>>;
        try {
            auto cache = Platform::CreateNativeImmutableFileStorage(root, ".horo/ui-style-cache");
            if (cache.HasError())
                return R::Failure(cache.ErrorValue());
            auto target = AssetCookTargetId::Parse("headless-null");
            if (target.HasError())
                return R::Failure(target.ErrorValue());
            Application::UiStyleWorkspaceConfig config;
            config.cook.sourceRoot = root;
            config.cook.cacheFiles = std::move(cache).Value();
            config.cook.cookedRoot = root / ".horo" / "ui-style-validation" / workspace.ToString() / "headless-null";
            config.cook.target = std::move(target).Value();
            config.cook.limits.maximumAssets = Runtime::Ui::MaximumUiStyleTokens;
            config.cook.limits.maximumConcurrentCooks = 1;
            config.cook.limits.maximumSourceBytes = Runtime::Ui::MaximumUiStyleAssetBytes;
            config.cook.limits.maximumArtifactBytes = 2 * Runtime::Ui::MaximumUiStyleAssetBytes;
            // The native services outlive the screen/capability; its shutdown joins every user of this borrowed authority.
            config.cook.publicationFiles = std::shared_ptr<DurableFileSystem>(&files, [](DurableFileSystem *) {
            });
            config.cook.newPublicationOperationId = OperationId;
            config.currentAssets = [&assets] {
                return assets.Snapshot().Revision();
            };
            config.acquireSourceLease = [&mutations, root, workspace]() -> Result<std::shared_ptr<const void>> {
                auto acquired = mutations.TryAcquire({root, ProjectMutationOwner::Asset, "ui-style-" + workspace.ToString()});
                if (acquired.HasError())
                    return Result<std::shared_ptr<const void>>::Failure(acquired.ErrorValue());
                return Result<std::shared_ptr<const void>>::Success(std::make_shared<ProjectMutationLease>(std::move(acquired).Value()));
            };
            auto owner = std::make_shared<Application::UiStyleWorkspaceProducer>(jobs, std::move(config), workspace);
            auto resources = std::make_unique<UiInspectorResources>(assets);
            if (auto bound = resources->BindStyleProducer(Adapter(owner)); bound.HasError())
                return R::Failure(bound.ErrorValue());
            return R::Success(std::move(resources));
        } catch (const std::bad_alloc &) {
            return R::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Editor
