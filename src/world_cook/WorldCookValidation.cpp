#include "WorldCookInternal.h"

#include <algorithm>

namespace Horo::WorldCook {
    namespace {
        using namespace WorldStreaming;
        using Payloads = std::vector<std::shared_ptr<const Runtime::RuntimeSceneCellPayload>>;

        /** @brief Preserves a native typed failure beside its presentation-only BuildOutput projection. */
        Result<ValidationReport> FindingReport(const SourceCapture &source, const CancellationToken &cancellation, Error error,
                                               std::string code) {
            if (cancellation.IsCancellationRequested())
                return Result<ValidationReport>::Failure(MakeError(Errors::Cancelled));
            ValidationReport report{source.sourceRevision, {}};
            report.findings.push_back({{.severity = DiagnosticSeverity::Error,
                                        .result = BuildOutputResult::Failed,
                                        .stage = "world.artifact.validation",
                                        .code = DiagnosticCode{std::move(code)},
                                        .toolCode = error.code.Value(),
                                        .message = error.message},
                                       std::move(error)});
            return Result<ValidationReport>::Success(std::move(report));
        }

        /** @brief Verifies actual semantic bytes against the captured source without trusting wire schema approvals. */
        Result<void> CheckCapturedCore(const SourceCapture &source, const Runtime::SceneCellCookInput &captured,
                                       const std::span<const std::byte> bytes, const PublicationLimits &limits,
                                       const CancellationToken &cancellation) {
            using namespace Runtime;
            auto expected =
                CookRuntimeSceneCellPayload(source.partition, captured.source, captured.expected, limits.scene.semantic, cancellation);
            if (expected.HasError())
                return Result<void>::Failure(expected.ErrorValue());
            auto canonical = EncodeSceneCellPayload(expected.Value(), limits.scene, cancellation);
            if (canonical.HasError())
                return Result<void>::Failure(canonical.ErrorValue());
            if (!std::ranges::equal(canonical.Value(), bytes))
                return Result<void>::Failure(MakeError(Errors::Stale));
            return Result<void>::Success();
        }

        /** @brief Decodes actual CoreEcs bytes and compares them to the independently cooked immutable source. */
        Result<std::shared_ptr<const Runtime::RuntimeSceneCellPayload>> DecodeCell(const SourceCapture &source,
                                                                                   const CellArtifactView &artifact,
                                                                                   const CookedWorldCellManifestEntry &metadata,
                                                                                   const PublicationLimits &limits,
                                                                                   const CancellationToken &cancellation) {
            using namespace Runtime;
            const auto captured = std::ranges::find_if(source.cells, [&](const auto &cell) {
                return cell.expected.cell == metadata.cell;
            });
            if (captured == source.cells.end())
                return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Failure(MakeError(Errors::Invalid));
            auto inspected = InspectStreamingCellArtifact(artifact.bytes, metadata.artifactHash,
                                                          {limits.cell.maximumPayloads, limits.cell.maximumArtifactBytes,
                                                           limits.cell.maximumDecodedBytes},
                                                          cancellation);
            if (inspected.HasError())
                return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Failure(inspected.ErrorValue());
            const auto header = inspected.Value().Header();
            if (header.cell != metadata.cell || header.compressedSize != metadata.compressedSize ||
                header.uncompressedSize != metadata.uncompressedSize || header.payloadCrc32 != metadata.payloadCrc32 ||
                header.compression != StreamingCellCompression::None)
                return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Failure(MakeError(Errors::Stale));
            const auto core = std::ranges::find(header.payloads, StreamingCellProvider::CoreEcs, &StreamingCellPayloadHeader::provider);
            if (core == header.payloads.end() || header.payloads.size() != 1 ||
                core->requirement != StreamingCellPayloadRequirement::Required || core->version != SceneCellPayloadCodecVersion)
                return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Failure(MakeError(SceneCellPayloadErrors::Unsupported));
            const auto bytes =
                artifact.bytes.subspan(static_cast<std::size_t>(core->offset), static_cast<std::size_t>(core->compressedSize));
            auto decoded = DecodeSceneCellPayload(source.partition, bytes, captured->expected, captured->source.componentSchemas,
                                                  captured->source.behaviorSchemas, limits.scene, cancellation);
            if (decoded.HasError())
                return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Failure(decoded.ErrorValue());
            if (auto expected = CheckCapturedCore(source, *captured, bytes, limits, cancellation); expected.HasError())
                return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Failure(expected.ErrorValue());
            return Result<std::shared_ptr<const RuntimeSceneCellPayload>>::Success(
                std::make_shared<const RuntimeSceneCellPayload>(std::move(decoded).Value()));
        }

        /** @brief Requires one actual entity binding for each placement and one base binding for each nonspatial object. */
        Result<void> CheckObjects(const SourceCapture &source, const DecodedWorldDependencies &dependencies, const Payloads &payloads) {
            std::size_t expectedBindings{};
            for (const auto &object : dependencies.objects) {
                expectedBindings += object.cells.empty() ? 1 : object.cells.size();
                if (object.cells.empty() && std::ranges::count_if(source.objects, [&](const auto &binding) {
                    const auto *base = std::get_if<BaseObjectBinding>(&binding);
                    return base && base->source == object.object;
                }) != 1)
                    return Result<void>::Failure(MakeError(Errors::Invalid));
                for (const auto cell : object.cells) {
                    if (std::ranges::count_if(source.objects, [&](const auto &binding) {
                        const auto *spatial = std::get_if<SpatialObjectBinding>(&binding);
                        return spatial && spatial->source == object.object && spatial->cell == cell;
                    }) != 1)
                        return Result<void>::Failure(MakeError(Errors::Invalid));
                }
            }
            if (expectedBindings != source.objects.size())
                return Result<void>::Failure(MakeError(Errors::Invalid));
            std::size_t entityCount{};
            for (const auto &payload : payloads) {
                entityCount += payload->Definition().Entities().size();
                for (const auto &entity : payload->Definition().Entities()) {
                    if (std::ranges::count_if(source.objects, [&](const auto &binding) {
                        const auto *spatial = std::get_if<SpatialObjectBinding>(&binding);
                        return spatial && spatial->cell == payload->Identity().cell && spatial->entity == entity.object;
                    }) != 1)
                        return Result<void>::Failure(MakeError(Errors::Invalid));
                }
            }
            if (entityCount != static_cast<std::size_t>(std::ranges::count_if(source.objects, [](const auto &binding) {
                return std::holds_alternative<SpatialObjectBinding>(binding);
            })))
                return Result<void>::Failure(MakeError(Errors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Reconstructs the sole manifest authority and admits the complete hard package closure. */
        Result<CookedWorldIndexManifest> AdmitManifest(const SourceCapture &source, const ReleaseCapture &release,
                                                       const ArtifactViews &artifacts, const DecodedWorldIndex &index,
                                                       const DecodedWorldDependencies &dependencies, const PublicationLimits &limits,
                                                       const CancellationToken &cancellation) {
            auto expectedDependencies = EncodeWorldDependencyArtifact(index, source.assignments, source.placements, source.dependencies,
                                                                      limits.dependencies, cancellation);
            if (expectedDependencies.HasError())
                return Result<CookedWorldIndexManifest>::Failure(expectedDependencies.ErrorValue());
            if (!std::ranges::equal(expectedDependencies.Value(), artifacts.dependencies))
                return Result<CookedWorldIndexManifest>::Failure(MakeError(Errors::Stale));
            auto manifest = Detail::MakeManifest(source.partition, index.cells, dependencies.cells, limits);
            if (manifest.HasError())
                return Result<CookedWorldIndexManifest>::Failure(manifest.ErrorValue());
            auto expectedIndex = EncodeWorldIndexArtifact(manifest.Value(), source.worldName, source.coordinates, source.volumes,
                                                          limits.index, cancellation);
            if (expectedIndex.HasError())
                return Result<CookedWorldIndexManifest>::Failure(expectedIndex.ErrorValue());
            if (!std::ranges::equal(expectedIndex.Value().bytes, artifacts.index))
                return Result<CookedWorldIndexManifest>::Failure(MakeError(Errors::Stale));
            if (auto packages = WorldPackageChunkAssignment::Create(manifest.Value(), release.packages, release.binding,
                                                                    release.baseManifest, limits.packages);
                packages.HasError())
                return Result<CookedWorldIndexManifest>::Failure(packages.ErrorValue());
            return Result<CookedWorldIndexManifest>::Success(std::move(manifest).Value());
        }

        /** @brief Decodes exactly one actual Scene baseline for every authoritative partition cell. */
        Result<Payloads> DecodeWorldCells(const SourceCapture &source, const ArtifactViews &artifacts,
                                          const CookedWorldIndexManifest &manifest, const PublicationLimits &limits,
                                          const CancellationToken &cancellation) {
            if (artifacts.cells.size() != source.partition.Cells().size())
                return Result<Payloads>::Failure(MakeError(Errors::Invalid));
            Payloads payloads;
            for (const auto &metadata : manifest.Cells()) {
                if (cancellation.IsCancellationRequested())
                    return Result<Payloads>::Failure(MakeError(Errors::Cancelled));
                const auto definition = std::ranges::find(source.partition.Cells(), metadata.cell, &WorldPartitionCellDescriptor::id);
                if (std::ranges::count(artifacts.cells, definition->package.chunkAsset, &CellArtifactView::id) != 1)
                    return Result<Payloads>::Failure(MakeError(Errors::Invalid));
                const auto artifact = std::ranges::find(artifacts.cells, definition->package.chunkAsset, &CellArtifactView::id);
                auto payload = DecodeCell(source, *artifact, metadata, limits, cancellation);
                if (payload.HasError())
                    return Result<Payloads>::Failure(payload.ErrorValue());
                payloads.push_back(std::move(payload).Value());
            }
            return Result<Payloads>::Success(std::move(payloads));
        }

    }  // namespace

    /** @copydoc ValidateWorldCookArtifacts */
    Result<ValidationReport> ValidateWorldCookArtifacts(const SourceCapture &source, const ReleaseCapture &release,
                                                        const ArtifactViews &artifacts, const PublicationLimits &limits,
                                                        const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<ValidationReport>::Failure(MakeError(Errors::Cancelled));
        if (!source.sourceRevision || !limits.maximumTotalArtifactBytes || !limits.maximumObjectBindings ||
            !limits.semantic.maximumDiagnostics || source.objects.size() > limits.maximumObjectBindings)
            return Result<ValidationReport>::Failure(MakeError(Errors::Invalid));
        if (auto inventory = Detail::ValidateReleaseCoverage(source, release, artifacts, limits, cancellation); inventory.HasError()) {
            if (cancellation.IsCancellationRequested())
                return Result<ValidationReport>::Failure(MakeError(Errors::Cancelled));
            return FindingReport(source, cancellation, inventory.ErrorValue(), "world.build.missing_package_content");
        }
        auto index = DecodeWorldIndexArtifact(artifacts.index, limits.index, cancellation);
        if (index.HasError())
            return FindingReport(source, cancellation, index.ErrorValue(), "world.build.corrupt_index");
        auto dependencies = DecodeWorldDependencyArtifact(artifacts.dependencies, index.Value(), limits.dependencies, cancellation);
        if (dependencies.HasError())
            return FindingReport(source, cancellation, dependencies.ErrorValue(), "world.build.corrupt_dependencies");
        auto manifest = AdmitManifest(source, release, artifacts, index.Value(), dependencies.Value(), limits, cancellation);
        if (manifest.HasError())
            return FindingReport(source, cancellation, manifest.ErrorValue(), "world.build.invalid_world_artifacts");
        auto decodedCells = DecodeWorldCells(source, artifacts, manifest.Value(), limits, cancellation);
        if (decodedCells.HasError())
            return FindingReport(source, cancellation, decodedCells.ErrorValue(), "world.build.corrupt_or_missing_cell");
        const auto &payloads = decodedCells.Value();
        std::vector<Runtime::WorldBuildCell> cells;
        for (const auto &payload : payloads)
            cells.emplace_back(payload->Identity(), std::nullopt);
        if (auto bindings = CheckObjects(source, dependencies.Value(), payloads); bindings.HasError())
            return FindingReport(source, cancellation, bindings.ErrorValue(), "world.build.missing_object_content");
        auto semantic = Runtime::ValidateWorldBuild(source.partition, source.sourceRevision, cells, payloads, source.requiredReferences,
                                                    limits.semantic, cancellation);
        if (semantic.HasError())
            return Result<ValidationReport>::Failure(semantic.ErrorValue());
        ValidationReport report{source.sourceRevision, {}};
        for (const auto &diagnostic : semantic.Value().diagnostics)
            report.findings.emplace_back(diagnostic, MakeError(Errors::Invalid));
        return Result<ValidationReport>::Success(std::move(report));
    }
}  // namespace Horo::WorldCook
