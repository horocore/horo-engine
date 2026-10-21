#include "WorldBakeInternal.h"

#include <algorithm>

namespace Horo::Application::WorldBakeDetail {
    namespace {
        /** @brief Reads exact bounded authored bytes and preserves their independently verified source commitment. */
        Result<SceneSource::SceneSourceDocument> ReadPage(const WorldBakeRequest &request, const Editor::PartitionedWorldPage &page,
                                                          const CancellationToken &cancellation, std::size_t &sourceBytes,
                                                          WorldBakeOutput &output) {
            const auto origin = std::ranges::find(request.sources, page.head, &WorldBakePageSource::page);
            if (!request.sourceAuthority.files || origin == request.sources.end() || origin->relativePath.empty())
                return Result<SceneSource::SceneSourceDocument>::Failure(
                    MakeError(WorldBakeErrors::MissingPage, page.head.sourceAsset.ToString()));
            const Platform::ProjectReadContext context{.projectIdentity = request.sourceAuthority.owner.ToString(),
                                                       .projectGeneration = request.sourceAuthority.generation,
                                                       .cancellation = cancellation,
                                                       .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30)};
            const auto bytes = request.sourceAuthority.files->SourceText(origin->relativePath, context, {});
            if (bytes.HasError())
                return Result<SceneSource::SceneSourceDocument>::Failure(bytes.ErrorValue());
            if (!bytes.Value().text || bytes.Value().text->size() > request.limits.maximumSourceBytes - sourceBytes)
                return Result<SceneSource::SceneSourceDocument>::Failure(MakeError(WorldBakeErrors::Capacity));
            sourceBytes += bytes.Value().text->size();
            auto decoded = SceneSource::DecodeSceneSource(*bytes.Value().text);
            if (decoded.HasError())
                return Result<SceneSource::SceneSourceDocument>::Failure(decoded.ErrorValue());
            auto detached = std::move(decoded).Value();
            if (!detached.unsupportedComponentOwners.empty())
                return Result<SceneSource::SceneSourceDocument>::Failure(MakeError(WorldBakeErrors::Unsupported));
            const auto &text = *bytes.Value().text;
            output.sourceRevisions.emplace_back(page.head.sourceAsset,
                                                FormatSha256(ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}))));
            return Result<SceneSource::SceneSourceDocument>::Success(std::move(detached));
        }
    }  // namespace

    /** @brief Reads or leases actual page content without changing source residency, and invokes the canonical Scene converter. */
    Result<Runtime::RuntimeSceneDefinition> CapturePage(const WorldBakeRequest &request, const Editor::PartitionedWorldPage &page,
                                                        const CancellationToken &cancellation, std::size_t &sourceBytes,
                                                        WorldBakeOutput &output) {
        const auto loaded = std::ranges::find(request.document->loadedPages, page.head, &Editor::WorldLoadedPage::head);
        SceneSource::SceneSourceDocument detached;
        SceneSource::SceneSourceView sourceView;
        if (loaded != request.document->loadedPages.end()) {
            if (loaded->sourceBytes > request.limits.maximumSourceBytes - sourceBytes)
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(WorldBakeErrors::Capacity));
            sourceBytes += loaded->sourceBytes;
            sourceView = {loaded->content.Objects(), loaded->content.PrefabInstances()};
        } else {
            auto read = ReadPage(request, page, cancellation, sourceBytes, output);
            if (read.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(read.ErrorValue());
            detached = std::move(read).Value();
            sourceView = {detached.objects, detached.prefabInstances};
        }
        if (sourceView.objects.size() != page.objects.size())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(WorldBakeErrors::Conflict));
        auto converted = ConvertPage(sourceView, page.head, request);
        if (converted.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(converted.ErrorValue());
        auto scene = std::move(converted).Value();
        if (scene.Entities().size() != page.objects.size())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(
                MakeError(WorldBakeErrors::Conflict, "Expanded Scene membership differs from the authored placement capture."));
        return Result<Runtime::RuntimeSceneDefinition>::Success(std::move(scene));
    }
}  // namespace Horo::Application::WorldBakeDetail
