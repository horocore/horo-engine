#include "WorldBakeInternal.h"

#include <algorithm>
#include <set>

namespace Horo::Application {
    namespace W = WorldStreaming;
    namespace S = Runtime;

    /** @brief Rechecks exact captured file revisions after all cooking, without trusting authoring metadata as source bytes. */
    Result<void> VerifySources(const WorldBakeRequest &request, const WorldBakeOutput &output, const CancellationToken &cancellation) {
        for (const auto &[page, revision] : output.sourceRevisions) {
            const auto source = std::ranges::find(request.sources, page, [](const auto &value) {
                return value.page.sourceAsset;
            });
            if (!request.files || source == request.sources.end())
                return Result<void>::Failure(MakeError(WorldBakeErrors::Stale));
            const Platform::ProjectReadContext context{.projectIdentity = request.project.fence.project.ToString(),
                                                       .projectGeneration = request.project.fence.revision,
                                                       .cancellation = cancellation,
                                                       .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30)};
            auto current = request.files->SourceText(source->relativePath, context, {});
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!current.Value().text)
                return Result<void>::Failure(MakeError(WorldBakeErrors::Stale));
            const auto &text = *current.Value().text;
            if (FormatSha256(ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}))) != revision)
                return Result<void>::Failure(MakeError(WorldBakeErrors::Stale));
        }
        return Result<void>::Success();
    }

    /** @copydoc CaptureWorldBakeDependencies */
    Result<std::vector<WorldBakeDependency>> CaptureWorldBakeDependencies(const DurableFileSystem &files,
                                                                          const Assets::AssetCookGeneration &generation,
                                                                          const std::uint64_t revision, const std::size_t maximumBytes) {
        if (!revision || !maximumBytes)
            return Result<std::vector<WorldBakeDependency>>::Failure(MakeError(WorldBakeErrors::Invalid));
        auto contents = Assets::ReadCookGenerationContents(files, generation, maximumBytes);
        if (contents.HasError())
            return Result<std::vector<WorldBakeDependency>>::Failure(contents.ErrorValue());
        std::vector<WorldBakeDependency> result;
        for (std::size_t index = 0; index < contents.Value().artifacts.size(); ++index) {
            auto decoded = Assets::DecodeCookedArtifact(contents.Value().artifacts[index]);
            if (decoded.HasError())
                return Result<std::vector<WorldBakeDependency>>::Failure(decoded.ErrorValue());
            result.push_back({{decoded.Value().id, decoded.Value().type}, contents.Value().entries[index].artifactHash, revision, {}});
        }
        return Result<std::vector<WorldBakeDependency>>::Success(std::move(result));
    }

    namespace {
        /** @brief Orchestrates semantic cell cooking and complete build qualification before any encoded result can be adopted. */
        Result<void> CookPrepared(const WorldBakeRequest &request, WorldBakeDetail::PreparedWorld &prepared,
                                  const CancellationToken &cancellation, const std::function<void(float)> &progress) {
            const auto revision = request.document->fence.revision.Value();
            const auto &document = request.document->document;
            auto descriptor =
                W::WorldPartitionDescriptor::Create({}, document.settings.partition, document.settings.bounds, document.settings.grid,
                                                    document.layers, prepared.topology, {256, request.limits.maximumCells, 65536});
            if (descriptor.HasError())
                return Result<void>::Failure(descriptor.ErrorValue());
            auto partition = std::move(descriptor).Value();
            auto cooked = WorldBakeDetail::CookCells(request, partition, prepared, cancellation);
            if (cooked.HasError())
                return Result<void>::Failure(cooked.ErrorValue());
            auto cells = std::move(cooked).Value();
            prepared.output.cells = std::move(cells.payloads);
            const auto &expected = cells.expected;
            if (progress)
                progress(0.65F);
            auto references = WorldBakeDetail::CaptureReferences(expected, prepared.output.cells, request.limits.validation);
            if (references.HasError())
                return Result<void>::Failure(references.ErrorValue());
            auto validated = S::ValidateWorldBuild(partition, revision, expected, prepared.output.cells, references.Value(),
                                                   request.limits.validation, cancellation);
            if (validated.HasError())
                return Result<void>::Failure(validated.ErrorValue());
            prepared.output.validation = std::move(validated).Value();
            if (progress)
                progress(0.75F);
            if (!prepared.output.validation.CanPublish())
                return Result<void>::Success();
            if (auto encoded = WorldBakeDetail::Encode(request, prepared.output, cancellation, progress); encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CookWorldPages */
    Result<WorldBakeOutput> CookWorldPages(const WorldBakeRequest &request, const CancellationToken &cancellation,
                                           const std::function<void(float)> &progress) {
        if (progress)
            progress(0.0F);
        if (auto valid = WorldBakeDetail::Admit(request); valid.HasError())
            return Result<WorldBakeOutput>::Failure(valid.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<WorldBakeOutput>::Failure(MakeError(WorldBakeErrors::Cancelled));
        auto converted = WorldBakeDetail::Convert(request, cancellation);
        if (converted.HasError())
            return Result<WorldBakeOutput>::Failure(converted.ErrorValue());
        auto prepared = std::move(converted).Value();
        if (progress)
            progress(0.25F);
        const auto revision = request.document->fence.revision.Value();
        prepared.output.validation.revision = revision;
        if (!prepared.topology.empty())
            if (auto cooked = CookPrepared(request, prepared, cancellation, progress); cooked.HasError())
                return Result<WorldBakeOutput>::Failure(cooked.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<WorldBakeOutput>::Failure(MakeError(WorldBakeErrors::Cancelled));
        if (auto verified = VerifySources(request, prepared.output, cancellation); verified.HasError())
            return Result<WorldBakeOutput>::Failure(verified.ErrorValue());
        return Result<WorldBakeOutput>::Success(std::move(prepared.output));
    }
}  // namespace Horo::Application
