#pragma once

#include "NativeCacheFiles.h"

/** @file NavigationProjectBakeNativeFixture.h
 * @brief Explicit native service composition shared by project-input and final-adoption regressions.
 */
#include "navigation/NavigationBakeServiceFixture.h"
#include "navigation/NavigationProjectBakeInputFixture.h"

namespace Horo::Application::NavigationInputNativeTestSupport {
    namespace Nav = Navigation;
    namespace Inputs = NavigationInputTestSupport;
    using NavigationSourceTestSupport::Asset;
    using enum Nav::NavigationBakeExecutionMode;

    /** @brief Builds one complete owned request while retaining its actual file-backed source evidence. */
    inline NavigationBakeRequest Request(const NavigationProjectBakeInput &input, Nav::NavigationBakeExecutionMode mode = BuildAndPublish,
                                         std::optional<std::vector<Nav::NavigationBakeSurfaceKey>> selection = {}) {
        std::vector<Nav::NavigationSourceObservation> observations;
        observations.reserve(input.contributions.size());
        for (const auto &source : input.contributions)
            observations.push_back(source.observation);
        return {.input = input.input,
                .compatibility = Nav::TestSupport::IncrementalBakeFixture{}.compatibility,
                .tiles = input.tiles,
                .sources = std::move(observations),
                .projectProfile = input.source->Settings().RuntimeProfile(),
                .rebuildSurfaces = std::move(selection),
                .execution = mode,
                .authoredSurfaces = input.surfaces,
                .sourceValidation = input.source};
    }

    /** @brief Seeds current-source authority and submits the same complete file-backed request in every mode. */
    inline Result<OperationId> Submit(const NavigationProjectBakeInput &input, NavigationBakeService &service,
                                      const NavigationBakeServiceConfig &config, Nav::NavigationBakeExecutionMode mode = BuildAndPublish,
                                      std::optional<std::vector<Nav::NavigationBakeSurfaceKey>> selection = {}) {
        auto request = Request(input, mode, std::move(selection));
        if (auto updated = config.sourceAuthority->UpdateCurrent(input.input->Revisions(), request.sources); updated.HasError())
            return Result<OperationId>::Failure(updated.ErrorValue());
        return service.Submit(std::move(request));
    }

    /** @brief Runs a fresh capture to terminal success using the same real service and current-source authority. */
    inline std::shared_ptr<const NavigationBakePublication> Execute(
        const NavigationProjectBakeInput &input, NavigationBakeService &service, const NavigationBakeServiceConfig &config,
        const OperationStore &operations, Nav::NavigationBakeExecutionMode mode = BuildAndPublish,
        std::optional<std::vector<Nav::NavigationBakeSurfaceKey>> selection = {}) {
        REQUIRE(input.source->VerifyCurrent().HasValue());
        const auto operation = Submit(input, service, config, mode, std::move(selection));
        REQUIRE(operation.HasValue());
        REQUIRE(BakeTestSupport::Terminal(service, operations, operation.Value()).state == OperationState::Succeeded);
        return service.Published();
    }

    /** @brief Explicit real native host composition under the owned project's canonical output paths. */
    inline NavigationBakeServiceConfig Config(const Inputs::Project &project, std::shared_ptr<BakeTestSupport::ControlledBuilder> builder) {
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        return {.definition = Asset(1),
                .artifactType = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName).Value(),
                .target = AssetCookTargetId::Parse("headless-null").Value(),
                .cacheFiles = Horo::TestSupport::NativeCacheFiles(root / ".horo/cache/navigation"),
                .targetRoot = root / ".horo/cooked/navigation",
                .builder = std::move(builder),
                .files = std::make_shared<NativeDurableFileSystem>(),
                .budget = {1, 1024ULL * 1024ULL * 1024ULL, 128U * 1024U * 1024U, 8, 1024ULL * 1024ULL * 1024ULL,
                           Duration::FromMilliseconds(2000)},
                .sourceAuthority = std::make_shared<NavigationBakeSourceAuthority>(),
                .newOperationId = Horo::TestSupport::NewPublicationOperationId};
    }

}  // namespace Horo::Application::NavigationInputNativeTestSupport
