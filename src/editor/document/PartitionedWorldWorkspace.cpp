#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Horo::Editor {
    namespace WorldDocumentErrors {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.editor.world_document"},
                                          .code = ErrorCode{"world_document.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "World document input is invalid."};
        const ErrorCodeDescriptor Unsupported{.domain = ErrorDomainId{"horo.editor.world_document"},
                                              .code = ErrorCode{"world_document.unsupported"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "World document operation is unsupported."};
        const ErrorCodeDescriptor Conflict{.domain = ErrorDomainId{"horo.editor.world_document"},
                                           .code = ErrorCode{"world_document.conflict"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "World document identities or memberships conflict."};
        const ErrorCodeDescriptor Capacity{.domain = ErrorDomainId{"horo.editor.world_document"},
                                           .code = ErrorCode{"world_document.capacity"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "World document metadata exceeds its bounded capacity."};
        const ErrorCodeDescriptor Lifecycle{.domain = ErrorDomainId{"horo.editor.world_document"},
                                            .code = ErrorCode{"world_document.lifecycle"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "World document owner is unavailable."};
        const ErrorCodeDescriptor Stale{.domain = ErrorDomainId{"horo.editor.world_document"},
                                        .code = ErrorCode{"world_document.stale"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "World document fence no longer names the current publication."};
        const ErrorCodeDescriptor Cancelled{.domain = ErrorDomainId{"horo.editor.world_document"},
                                            .code = ErrorCode{"world_document.cancelled"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "World document operation was cancelled before publication."};
        const ErrorCodeDescriptor WrongThread{.domain = ErrorDomainId{"horo.editor.world_document"},
                                              .code = ErrorCode{"world_document.wrong_thread"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "World document operations require their owner thread."};
        const ErrorCodeDescriptor Exhausted{.domain = ErrorDomainId{"horo.editor.world_document"},
                                            .code = ErrorCode{"world_document.exhausted"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "World document generation or revision is exhausted."};
        const ErrorCodeDescriptor Dirty{.domain = ErrorDomainId{"horo.editor.world_document"},
                                        .code = ErrorCode{"world_document.dirty"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "Dirty world metadata requires an explicit discard decision."};
    }  // namespace WorldDocumentErrors

    namespace {
        namespace WS = WorldStreaming;
        constexpr auto KnownLayerFlags = static_cast<std::uint32_t>(WS::WorldLayerFlags::Persistent | WS::WorldLayerFlags::Optional |
                                                                    WS::WorldLayerFlags::ServerOnly | WS::WorldLayerFlags::ClientOnly);
        constexpr auto ExclusiveTargetFlags = static_cast<std::uint32_t>(WS::WorldLayerFlags::ServerOnly | WS::WorldLayerFlags::ClientOnly);

        /** @brief Returns a declared typed Editor failure. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Validates one layer's authored identity, name and supported policy. */
        Result<void> ValidateLayer(const WS::WorldLayerDescriptor &layer) {
            if (!layer.id.IsValid() || layer.name.empty() || layer.name.find('\0') != std::string::npos ||
                !IsValidUtf8ScalarSequence(layer.name) || !std::isfinite(layer.priorityMultiplier) || layer.priorityMultiplier <= 0)
                return Failure<void>(WorldDocumentErrors::Invalid);
            if (layer.ownership > WS::WorldLayerOwnership::NetworkReplication ||
                (static_cast<std::uint32_t>(layer.flags) & ~KnownLayerFlags) != 0)
                return Failure<void>(WorldDocumentErrors::Unsupported);
            if ((static_cast<std::uint32_t>(layer.flags) & ExclusiveTargetFlags) == ExclusiveTargetFlags)
                return Failure<void>(WorldDocumentErrors::Invalid);
            return Result<void>::Success();
        }

        /** @brief Validates source layers without requiring cooked cells. */
        Result<void> ValidateLayers(const PartitionedWorldDocument &document, const WorldDocumentLimits &limits) {
            std::set<WS::StreamingLayerId> identities;
            std::size_t nameBytes{};
            for (const auto &layer : document.layers) {
                if (layer.name.size() > limits.maximumLayerNameBytes - nameBytes)
                    return Failure<void>(WorldDocumentErrors::Capacity);
                nameBytes += layer.name.size();
                if (const auto valid = ValidateLayer(layer); valid.HasError())
                    return valid;
                if (!identities.insert(layer.id).second)
                    return Failure<void>(WorldDocumentErrors::Conflict);
            }
            return Result<void>::Success();
        }

        /** @brief Validates object descriptors and membership within one immutable page. */
        Result<void> ValidatePageObjects(const PartitionedWorldPage &page, const PartitionedWorldDocument &document) {
            std::set<WS::WorldAuthoringObjectAddress> objects;
            for (const auto &object : page.objects) {
                if (const auto valid = WS::ValidateWorldSpatialObjectDescriptor(object.descriptor); valid.HasError())
                    return valid;
                const auto &descriptor = object.descriptor;
                if (descriptor.address.page != page.head.sourceAsset || descriptor.revision != page.head.revision ||
                    !objects.insert(descriptor.address).second || std::ranges::none_of(document.layers, [&](const auto &layer) {
                    return layer.id == object.layer;
                }))
                    return Failure<void>(WorldDocumentErrors::Conflict);
            }
            return Result<void>::Success();
        }

        /** @brief Validates immutable page revisions and their object/layer membership. */
        Result<void> ValidatePages(const PartitionedWorldDocument &document, const WorldDocumentLimits &limits) {
            std::set<Assets::AssetId> pages;
            std::size_t objectCount{};
            for (const auto &page : document.pages) {
                if (!page.head.IsValid())
                    return Failure<void>(WorldDocumentErrors::Invalid);
                if (page.head.partition != document.settings.partition || !pages.insert(page.head.sourceAsset).second)
                    return Failure<void>(WorldDocumentErrors::Conflict);
                if (page.objects.size() > limits.maximumObjects - objectCount)
                    return Failure<void>(WorldDocumentErrors::Capacity);
                objectCount += page.objects.size();
                if (const auto valid = ValidatePageObjects(page, document); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Checks a requested ceiling against its nonzero hard maximum. */
        bool ValidLimit(const std::size_t requested, const std::size_t maximum) {
            return requested != 0 && requested <= maximum;
        }

        /** @brief Validates all owner resource ceilings before document admission. */
        bool ValidLimits(const WorldDocumentLimits &limits) {
            constexpr WorldDocumentLimits hard;
            return ValidLimit(limits.maximumLayers, hard.maximumLayers) && ValidLimit(limits.maximumPages, hard.maximumPages) &&
                   ValidLimit(limits.maximumObjects, hard.maximumObjects) &&
                   ValidLimit(limits.maximumLayerNameBytes, hard.maximumLayerNameBytes) &&
                   ValidLimit(limits.maximumOpenPages, hard.maximumOpenPages);
        }

        /** @brief Requires changed existing pages to advance by exactly one revision in the same partition. */
        Result<void> ValidatePageSuccessors(const PartitionedWorldDocument &document, const PartitionedWorldDocument &previous) {
            if (document.settings.partition != previous.settings.partition)
                return Failure<void>(WorldDocumentErrors::Conflict);
            for (const auto &page : document.pages) {
                const auto old = std::ranges::find_if(previous.pages, [&](const auto &record) {
                    return record.head.sourceAsset == page.head.sourceAsset;
                });
                if (old == previous.pages.end() || *old == page)
                    continue;
                const auto next = WS::NextWorldAuthoringRevision(old->head.revision);
                if (next.HasError())
                    return Result<void>::Failure(next.ErrorValue());
                if (page.head.revision != next.Value())
                    return Failure<void>(WorldDocumentErrors::Stale);
            }
            return Result<void>::Success();
        }

        /** @brief Compares metadata exactly without inventing cooked-world identities. */
        bool SameDocument(const PartitionedWorldDocument &left, const PartitionedWorldDocument &right) {
            const auto &a = left.settings;
            const auto &b = right.settings;
            return left.version == right.version && left.layers == right.layers && left.pages == right.pages &&
                   a.partition == b.partition && a.bounds == b.bounds && a.grid.Origin() == b.grid.Origin() &&
                   a.grid.Bounds() == b.grid.Bounds() && a.grid.BaseCellSizeMillimeters() == b.grid.BaseCellSizeMillimeters() &&
                   a.grid.LodLevels() == b.grid.LodLevels();
        }

        /** @brief Admits a bounded authored aggregate before copying its storage. */
        Result<void> ValidateDocument(const PartitionedWorldDocument &document, const WorldDocumentLimits &limits) {
            if (document.version != 1)
                return Failure<void>(WorldDocumentErrors::Unsupported);
            if (!document.settings.partition.IsValid())
                return Failure<void>(WorldDocumentErrors::Invalid);
            const auto minimum = document.settings.bounds.minimum.Millimeters();
            const auto maximum = document.settings.bounds.maximum.Millimeters();
            for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
                if (minimum[axis] > maximum[axis])
                    return Failure<void>(WorldDocumentErrors::Invalid);
            }
            const auto layer = WS::StreamingLayerId::Create(0).Value();
            for (const auto &coordinate : {document.settings.bounds.minimum, document.settings.bounds.maximum}) {
                if (const auto cell = WS::QuantizeWorldToCell(coordinate, document.settings.grid, 0, layer); cell.HasError())
                    return Result<void>::Failure(cell.ErrorValue());
            }
            if (document.layers.size() > limits.maximumLayers || document.pages.size() > limits.maximumPages)
                return Failure<void>(WorldDocumentErrors::Capacity);
            if (const auto valid = ValidateLayers(document, limits); valid.HasError())
                return valid;
            return ValidatePages(document, limits);
        }
    }  // namespace

    /** @copydoc PartitionedWorldWorkspace::PartitionedWorldWorkspace */
    PartitionedWorldWorkspace::PartitionedWorldWorkspace(Assets::AssetId owner, WorldDocumentLimits limits)
        : owner_(owner), limits_(limits) {}

    /** @copydoc PartitionedWorldWorkspace::Open */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::Open(const PartitionedWorldDocument &document,
                                                                          const std::stop_token cancellation) {
        if (std::this_thread::get_id() != thread_)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::WrongThread);
        if (state_ != WS::WorldAuthoringOwnerState::Active)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Lifecycle);
        if (snapshot_)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        if (!owner_.IsValid())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Invalid);
        if (!ValidLimits(limits_))
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Invalid);
        return Install(document, cancellation);
    }

    /** @copydoc PartitionedWorldWorkspace::Install */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::Install(const PartitionedWorldDocument &document,
                                                                             const std::stop_token cancellation) {
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        if (const auto valid = ValidateDocument(document, limits_); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Exhausted);
        const auto generation = WorldDocumentGeneration::Create(generation_ + 1).Value();
        auto candidate = std::make_shared<const PartitionedWorldSnapshot>(
            PartitionedWorldSnapshot{.fence = {.owner = owner_,
                                               .generation = generation,
                                               .revision = WorldDocumentRevision::Create(1).Value()},
                                     .document = document});
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        snapshot_ = std::move(candidate);
        generation_ = generation.Value();
        ClearLayerHistory();
        return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
    }

    /** @copydoc PartitionedWorldWorkspace::Commit */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::Commit(const PartitionedWorldDocument &document,
                                                                            const WorldDocumentFence expected,
                                                                            const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        if (const auto valid = ValidateDocument(document, limits_); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (const auto valid = ValidatePageSuccessors(document, snapshot_->document); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (SameDocument(document, snapshot_->document))
            return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        candidate->document = document;
        candidate->dirty = true;
        std::erase_if(candidate->openPages, [&](const auto &page) {
            return std::ranges::none_of(document.pages, [&](const auto &record) {
                return record.head.sourceAsset == page;
            });
        });
        if (candidate->currentLayer && std::ranges::none_of(document.layers, [&](const auto &layer) {
            return layer.id == *candidate->currentLayer;
        }))
            candidate->currentLayer.reset();
        candidate->layerUndoCount = 0;
        candidate->layerRedoCount = 0;
        auto published = Publish(std::move(candidate), cancellation);
        if (published.HasValue())
            ClearLayerHistory();
        return published;
    }

    /** @copydoc PartitionedWorldWorkspace::ValidateLayerCandidate */
    Result<void> PartitionedWorldWorkspace::ValidateLayerCandidate(const PartitionedWorldDocument &document) const {
        return ValidateDocument(document, limits_);
    }

    /** @copydoc PartitionedWorldWorkspace::CheckFence */
    Result<void> PartitionedWorldWorkspace::CheckFence(const WorldDocumentFence expected) const {
        if (std::this_thread::get_id() != thread_)
            return Failure<void>(WorldDocumentErrors::WrongThread);
        if (state_ != WS::WorldAuthoringOwnerState::Active || !snapshot_)
            return Failure<void>(WorldDocumentErrors::Lifecycle);
        if (expected != snapshot_->fence)
            return Failure<void>(WorldDocumentErrors::Stale);
        return Result<void>::Success();
    }

    /** @copydoc PartitionedWorldWorkspace::Publish */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::Publish(std::shared_ptr<PartitionedWorldSnapshot> candidate,
                                                                             const std::stop_token cancellation) {
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        if (snapshot_->fence.revision.Value() == std::numeric_limits<std::uint64_t>::max())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Exhausted);
        candidate->fence.revision = WorldDocumentRevision::Create(snapshot_->fence.revision.Value() + 1).Value();
        snapshot_ = std::move(candidate);
        return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
    }

    /** @copydoc PartitionedWorldWorkspace::SetWorkingSet */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::SetWorkingSet(const std::span<const Assets::AssetId> pages,
                                                                                   const WorldDocumentFence expected,
                                                                                   const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        if (pages.size() > limits_.maximumOpenPages)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Capacity);
        std::set<Assets::AssetId> distinct;
        for (const auto &page : pages) {
            if (!page.IsValid() || !distinct.insert(page).second ||
                std::ranges::none_of(snapshot_->document.pages, [&](const auto &record) {
                return record.head.sourceAsset == page;
            }))
                return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        }
        if (std::ranges::equal(pages, snapshot_->openPages))
            return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        candidate->openPages.assign(pages.begin(), pages.end());
        return Publish(std::move(candidate), cancellation);
    }

    /** @copydoc PartitionedWorldWorkspace::Replace */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::Replace(const PartitionedWorldDocument &document,
                                                                             const WorldDocumentFence expected,
                                                                             const WorldDocumentDisposition disposition,
                                                                             const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        if (disposition != WorldDocumentDisposition::Preserve && disposition != WorldDocumentDisposition::Discard)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Unsupported);
        if (snapshot_->dirty && disposition != WorldDocumentDisposition::Discard)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Dirty);
        return Install(document, cancellation);
    }

    /** @copydoc PartitionedWorldWorkspace::Close */
    Result<void> PartitionedWorldWorkspace::Close(const WorldDocumentFence expected, const WorldDocumentDisposition disposition) {
        if (std::this_thread::get_id() != thread_)
            return Failure<void>(WorldDocumentErrors::WrongThread);
        if (!snapshot_ || state_ == WS::WorldAuthoringOwnerState::Closed)
            return Failure<void>(WorldDocumentErrors::Lifecycle);
        if (expected != snapshot_->fence)
            return Failure<void>(WorldDocumentErrors::Stale);
        if (disposition != WorldDocumentDisposition::Preserve && disposition != WorldDocumentDisposition::Discard)
            return Failure<void>(WorldDocumentErrors::Unsupported);
        if (snapshot_->dirty && disposition != WorldDocumentDisposition::Discard)
            return Failure<void>(WorldDocumentErrors::Dirty);
        snapshot_.reset();
        ClearLayerHistory();
        return Result<void>::Success();
    }

    /** @copydoc PartitionedWorldWorkspace::BeginCancellation */
    Result<void> PartitionedWorldWorkspace::BeginCancellation() {
        if (std::this_thread::get_id() != thread_)
            return Failure<void>(WorldDocumentErrors::WrongThread);
        if (state_ != WS::WorldAuthoringOwnerState::Closed)
            state_ = WS::WorldAuthoringOwnerState::Cancelling;
        return Result<void>::Success();
    }

    /** @copydoc PartitionedWorldWorkspace::Shutdown */
    Result<void> PartitionedWorldWorkspace::Shutdown(const WorldDocumentDisposition disposition) {
        if (std::this_thread::get_id() != thread_)
            return Failure<void>(WorldDocumentErrors::WrongThread);
        if (disposition != WorldDocumentDisposition::Preserve && disposition != WorldDocumentDisposition::Discard)
            return Failure<void>(WorldDocumentErrors::Unsupported);
        if (snapshot_) {
            if (const auto closed = Close(snapshot_->fence, disposition); closed.HasError())
                return closed;
        }
        state_ = WS::WorldAuthoringOwnerState::Closed;
        return Result<void>::Success();
    }

    /** @copydoc PartitionedWorldWorkspace::Snapshot */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::Snapshot() const {
        if (std::this_thread::get_id() != thread_)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::WrongThread);
        if (!snapshot_)
            return Failure<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Lifecycle);
        return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
    }
}  // namespace Horo::Editor
