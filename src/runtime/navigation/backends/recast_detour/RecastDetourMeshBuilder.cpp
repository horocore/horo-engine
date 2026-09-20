#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "RecastDetourMeshBuilderInternal.h"

#include <memory>
#include <new>
#include <utility>

namespace Horo::Navigation {
    namespace {
        class RecastDetourNavigationMeshBuilder final : public INavigationMeshBuilder {
        public:
            [[nodiscard]] Result<NavigationTileBuildResult> BuildTile(const NavigationTileBuildRequest &request,
                                                                      const CancellationToken &cancellation) const override {
                try {
                    using namespace RecastDetourMeshBuilderInternal;
                    if (const auto valid = ValidateRequest(request); valid.HasError())
                        return Result<NavigationTileBuildResult>::Failure(valid.ErrorValue());
                    if (cancellation.IsCancellationRequested())
                        return Failure<NavigationTileBuildResult>(NavigationErrors::BakeInputCancelled);

                    const auto config = MakeConfig(request);
                    if (config.HasError())
                        return Result<NavigationTileBuildResult>::Failure(config.ErrorValue());
                    const auto prepared = PrepareTriangles(request);
                    if (prepared.HasError())
                        return Result<NavigationTileBuildResult>::Failure(prepared.ErrorValue());
                    auto pipeline = RunRecastPipeline(request, prepared.Value(), config.Value(), cancellation);
                    if (pipeline.HasError())
                        return Result<NavigationTileBuildResult>::Failure(pipeline.ErrorValue());
                    return TranslateResult(request, prepared.Value(), std::move(pipeline).Value());
                } catch (const std::bad_alloc &) {
                    return RecastDetourMeshBuilderInternal::Failure<NavigationTileBuildResult>(NavigationErrors::CapacityExceeded);
                }
            }
        };
    }  // namespace

    /** @copydoc CreateRecastDetourNavigationMeshBuilder */
    Result<std::unique_ptr<INavigationMeshBuilder>> CreateRecastDetourNavigationMeshBuilder() {
        try {
            return Result<std::unique_ptr<INavigationMeshBuilder>>::Success(std::make_unique<RecastDetourNavigationMeshBuilder>());
        } catch (const std::bad_alloc &) {
            return RecastDetourMeshBuilderInternal::Failure<std::unique_ptr<INavigationMeshBuilder>>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
