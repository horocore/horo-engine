#pragma once
#include "Horo/Application/UiStyleWorkspace.h"

namespace Horo::Application::UiStyleWorkspaceInternal {
    struct Capture final {
        std::shared_ptr<const Assets::AssetCookInputSnapshot> inputs;
        Assets::AssetRegistrySnapshot selected;
        std::vector<Runtime::Ui::UiStyleAssetDocument> documents;
        Runtime::Ui::UiCoreStyleDefaults defaults;
        std::vector<Assets::AssetId> resources;
    };

    /** @brief Capture exact sidecars and transitive declared source closure under the host read lease. */
    [[nodiscard]] Result<Capture> CaptureSources(const UiStyleWorkspaceConfig &config, const Assets::AssetRegistrySnapshot &assets,
                                                 Runtime::Ui::RuntimeStyleAssetId root, const CancellationToken &cancellation);
    /** @brief Build the single schema from actual verified first-phase resource envelopes. */
    [[nodiscard]] Result<Runtime::Ui::UiStyleSchema> Schema(const Capture &capture,
                                                            std::span<const Assets::AssetCookCandidateArtifactView> resources);
    /** @brief Load one pinned manifest through the normal provider and revalidate all domain payloads. */
    [[nodiscard]] Result<UiStyleWorkspaceCandidate> LoadCandidate(JobSystem &jobs, const UiStyleWorkspaceConfig &config,
                                                                  const Capture &capture, const Assets::AssetCookGeneration &generation,
                                                                  Assets::AssetId workspace, std::uint64_t request,
                                                                  Runtime::Ui::RuntimeStyleAssetId theme,
                                                                  Runtime::Ui::RuntimeStyleGeneration styleGeneration,
                                                                  const CancellationToken &cancellation);
}  // namespace Horo::Application::UiStyleWorkspaceInternal
