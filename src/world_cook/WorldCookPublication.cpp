#include "WorldCookInternal.h"

#include <algorithm>
#include <array>

namespace Horo::WorldCook {
    namespace Errors {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.worldcook"},
                                          .code = ErrorCode{"world.cook.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The complete world publication capture is invalid.",
                                          .retryable = false,
                                          .userActionable = true};
        const ErrorCodeDescriptor CapacityExceeded{.domain = ErrorDomainId{"horo.worldcook"},
                                                   .code = ErrorCode{"world.cook.capacity-exceeded"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "World publication exceeds its explicit bounded storage.",
                                                   .retryable = false,
                                                   .userActionable = true};
        const ErrorCodeDescriptor Cancelled{.domain = ErrorDomainId{"horo.worldcook"},
                                            .code = ErrorCode{"world.cook.cancelled"},
                                            .defaultSeverity = ErrorSeverity::Info,
                                            .summary = "The unpublished world cook was cancelled.",
                                            .retryable = true,
                                            .userActionable = false};
        const ErrorCodeDescriptor Stale{.domain = ErrorDomainId{"horo.worldcook"},
                                        .code = ErrorCode{"world.cook.stale"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "The world publication capture was replaced.",
                                        .retryable = true,
                                        .userActionable = true};

        /** @copydoc Descriptors */
        std::span<const ErrorCodeDescriptor> Descriptors() noexcept {
            static const std::array values{Invalid, CapacityExceeded, Cancelled, Stale};
            return values;
        }
    }  // namespace Errors

    namespace {
        /**
         * @brief Allocates the result owner before invoking Assets, then initializes its result directly from the prvalue.
         * @details C++17 guaranteed copy elision adds no aggregate-owned vector/string move after Assets commits. The
         * existing Assets implementation and its own return boundary retain their independent owner contract.
         */
        struct PublicationStorage final {
            PublicationStorage(const ReleaseCapture &release, const Assets::AssetCookGenerationContents &inventory,
                               const PublicationLimits &limits, const Assets::AssetCookPublicationPolicy &policy)
                : result(Assets::PublishCookGeneration(release.targetRoot, release.target, inventory.entries, inventory.artifacts,
                                                       limits.assets, policy)) {}

            const Result<Assets::AssetCookGeneration> result;
        };
    }  // namespace

    /** @copydoc PublishWorldCook */
    Result<PublicationReport> PublishWorldCook(Runtime::IncrementalSceneCellCook &cache, const SourceCapture &source,
                                               const ReleaseCapture &release, const PublicationLimits &limits,
                                               const Assets::AssetCookPublicationPolicy &policy, const CancellationToken &cancellation) {
        if (!policy.beforeCommit || !source.sourceRevision || !release.target.IsValid() || !release.indexArtifact.IsValid() ||
            !release.dependencyArtifact.IsValid() || !limits.maximumTotalArtifactBytes || !limits.maximumObjectBindings)
            return Result<PublicationReport>::Failure(MakeError(Errors::Invalid));
        auto prepared = cache.Prepare(source.partition, source.cells, source.dependencyArtifacts, source.settings,
                                      source.expectedCacheRevision, limits.scene.semantic, cancellation);
        if (prepared.HasError())
            return Result<PublicationReport>::Failure(prepared.ErrorValue());
        auto receipt = std::move(prepared).Value();
        auto encoded = Detail::EncodeWorld(source, receipt.Report(), limits, cancellation);
        if (encoded.HasError())
            return Result<PublicationReport>::Failure(encoded.ErrorValue());
        std::vector<CellArtifactView> views;
        views.reserve(encoded.Value().cells.size());
        for (const auto &cell : encoded.Value().cells) {
            const auto definition =
                std::ranges::find(source.partition.Cells(), cell.integrity.cell, &WorldStreaming::WorldPartitionCellDescriptor::id);
            views.push_back({definition->package.chunkAsset, cell.bytes});
        }
        auto validation = ValidateWorldCookArtifacts(source, release, {encoded.Value().index.bytes, encoded.Value().dependencies, views},
                                                     limits, cancellation);
        if (validation.HasError())
            return Result<PublicationReport>::Failure(validation.ErrorValue());
        PublicationReport report{std::make_shared<const ValidationReport>(validation.Value()), {}, {}};
        if (!report.validation->CanPublish())
            return Result<PublicationReport>::Success(std::move(report));
        report.scene = receipt.ReportLease();
        auto inventory = Detail::PrepareInventory(source, release, encoded.Value(), limits, cancellation);
        if (inventory.HasError())
            return Result<PublicationReport>::Failure(inventory.ErrorValue());
        if (auto admitted = Detail::ValidateInventory(source, release, inventory.Value(), limits); admitted.HasError())
            return Result<PublicationReport>::Failure(admitted.ErrorValue());
        auto composed = policy;
        composed.beforeCommit = [&]() -> Result<void> {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Errors::Cancelled));
            if (auto current = policy.beforeCommit(); current.HasError())
                return current;
            return receipt.Reserve(cancellation);
        };
        composed.afterCommit = [&](const Assets::AssetCookGeneration &generation) {
            // Adopt also when generation.durabilityError is set: the sole Assets pointer has committed.
            receipt.Adopt();
            if (policy.afterCommit)
                policy.afterCommit(generation);
        };
        // allocate_shared allocates its control block before constructing PublicationStorage and entering Assets.
        auto published = std::make_shared<const PublicationStorage>(release, inventory.Value(), limits, composed);
        if (published->result.HasError())
            return Result<PublicationReport>::Failure(published->result.ErrorValue());
        report.publication = {published, &published->result.Value()};
        return Result<PublicationReport>::Success(std::move(report));
    }
}  // namespace Horo::WorldCook
