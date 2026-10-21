#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Scene/SceneSource.h"
#include "editor/document/SceneDocument.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Returns the declared document-boundary failure without changing owner state. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Finds one loaded page by stable source identity. */
        auto FindLoaded(auto &pages, const Assets::AssetId page) {
            return std::ranges::find(pages, page, [](const auto &record) {
                return record.head.sourceAsset;
            });
        }

        /** @brief Validates exact metadata/content identity membership before admitting a document. */
        bool MatchesPage(const PartitionedWorldPage &page, const SceneSource::SceneSourceDocument &source) {
            if (page.objects.size() != source.objects.size())
                return false;
            return std::ranges::all_of(source.objects, [&](const auto &object) {
                return std::ranges::any_of(page.objects, [&](const auto &record) {
                    return record.descriptor.address.object == object.id.value;
                });
            });
        }

    }  // namespace

    /** @copydoc WorldPageContentLease::Objects */
    std::span<const SceneSource::SceneObjectSnapshot> WorldPageContentLease::Objects() const noexcept {
        return document_ ? document_->Objects() : std::span<const SceneSource::SceneObjectSnapshot>{};
    }

    /** @copydoc WorldPageContentLease::PrefabInstances */
    std::span<const SceneSource::ScenePrefabInstance> WorldPageContentLease::PrefabInstances() const noexcept {
        return document_ ? document_->PrefabInstances() : std::span<const SceneSource::ScenePrefabInstance>{};
    }

    /** @copydoc WorldPageContentLease::IsDirty */
    bool WorldPageContentLease::IsDirty() const noexcept {
        return document_ && document_->IsDirty();
    }

    /** @copydoc PartitionedWorldWorkspace::CheckPageCapacity */
    Result<void> PartitionedWorldWorkspace::CheckPageCapacity(const Assets::AssetId page, const std::size_t objects,
                                                              const std::size_t bytes) const {
        if (bytes > limits_.maximumPageSourceBytes)
            return Failure<void>(WorldDocumentErrors::Capacity);
        std::size_t residentBytes{};
        std::size_t residentObjects{};
        for (const auto &loaded : snapshot_->loadedPages) {
            if (loaded.head.sourceAsset != page) {
                residentBytes += loaded.sourceBytes;
                residentObjects += loaded.content.Objects().size();
            }
        }
        if (bytes > limits_.maximumResidentSourceBytes - residentBytes || objects > limits_.maximumObjects - residentObjects)
            return Failure<void>(WorldDocumentErrors::Capacity);
        return Result<void>::Success();
    }

    /** @copydoc PartitionedWorldWorkspace::PreparePage */
    Result<WorldLoadedPage> PartitionedWorldWorkspace::PreparePage(const PartitionedWorldPage &page, const std::string_view source,
                                                                   const bool dirty, const std::stop_token cancellation) const {
        if (cancellation.stop_requested())
            return Failure<WorldLoadedPage>(WorldDocumentErrors::Cancelled);
        if (const auto valid = CheckPageCapacity(page.head.sourceAsset, page.objects.size(), source.size()); valid.HasError())
            return Result<WorldLoadedPage>::Failure(valid.ErrorValue());
        auto decoded = SceneSource::DecodeSceneSource(source);
        if (decoded.HasError())
            return Result<WorldLoadedPage>::Failure(decoded.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<WorldLoadedPage>(WorldDocumentErrors::Cancelled);
        if (!decoded.Value().unsupportedComponentOwners.empty())
            return Failure<WorldLoadedPage>(WorldDocumentErrors::Unsupported);
        if (!MatchesPage(page, decoded.Value()))
            return Failure<WorldLoadedPage>(WorldDocumentErrors::Conflict);
        auto document = std::make_shared<SceneDocument>();
        auto admitted = dirty ? document->LoadRecovered(std::move(decoded.Value().objects), std::move(decoded.Value().prefabInstances))
                              : document->LoadSaved(std::move(decoded.Value().objects), std::move(decoded.Value().prefabInstances));
        if (admitted.HasError())
            return Result<WorldLoadedPage>::Failure(admitted.ErrorValue());
        WorldLoadedPage prepared{.head = page.head, .sourceBytes = source.size()};
        prepared.content.document_ = std::move(document);
        return Result<WorldLoadedPage>::Success(std::move(prepared));
    }

    /** @copydoc PartitionedWorldWorkspace::LoadPage */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::LoadPage(const WorldStreaming::WorldAuthoringPageDescriptor page,
                                                                              const std::string_view source,
                                                                              const WorldDocumentFence expected,
                                                                              const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        const auto metadata = std::ranges::find(snapshot_->document.pages, page.sourceAsset, [](const auto &record) {
            return record.head.sourceAsset;
        });
        if (metadata == snapshot_->document.pages.end())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        if (metadata->head != page)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Stale);
        if (FindLoaded(snapshot_->loadedPages, page.sourceAsset) != snapshot_->loadedPages.end())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        const bool alreadyOpen = std::ranges::find(snapshot_->openPages, page.sourceAsset) != snapshot_->openPages.end();
        if (!alreadyOpen && snapshot_->openPages.size() >= limits_.maximumOpenPages)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Capacity);
        auto prepared = PreparePage(*metadata, source, false, cancellation);
        if (prepared.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(prepared.ErrorValue());
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        candidate->loadedPages.push_back(std::move(prepared).Value());
        if (!alreadyOpen)
            candidate->openPages.push_back(page.sourceAsset);
        return Publish(std::move(candidate), cancellation);
    }

    /** @copydoc PartitionedWorldWorkspace::UnloadPage */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::UnloadPage(const Assets::AssetId page,
                                                                                const WorldDocumentFence expected,
                                                                                const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        const auto loaded = FindLoaded(snapshot_->loadedPages, page);
        if (loaded == snapshot_->loadedPages.end())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        if (loaded->content.IsDirty())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Dirty);
        if (loaded->pinned || std::ranges::any_of(snapshot_->selectedObjects, [&](const auto &object) {
            return object.page == page;
        }))
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        candidate->loadedPages.erase(FindLoaded(candidate->loadedPages, page));
        std::erase(candidate->openPages, page);
        return Publish(std::move(candidate), cancellation);
    }

    /** @copydoc PartitionedWorldWorkspace::SetPagePinned */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::SetPagePinned(const Assets::AssetId page, const bool pinned,
                                                                                   const WorldDocumentFence expected,
                                                                                   const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        const auto loaded = FindLoaded(snapshot_->loadedPages, page);
        if (loaded == snapshot_->loadedPages.end())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        if (loaded->pinned == pinned)
            return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        FindLoaded(candidate->loadedPages, page)->pinned = pinned;
        return Publish(std::move(candidate), cancellation);
    }

}  // namespace Horo::Editor
