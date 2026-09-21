#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        bool UnitInterval(const float value) noexcept {
            return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
        }

        bool ValidRole(const UiRenderResourceRole role) noexcept {
            return role >= UiRenderResourceRole::Image && role <= UiRenderResourceRole::Mask;
        }

        bool PresentIndex(const std::uint32_t index, const std::size_t size) noexcept {
            return index < size;
        }

        bool OptionalIndex(const std::uint32_t index, const std::size_t size) noexcept {
            return index == NoUiRenderIndex || PresentIndex(index, size);
        }

        bool ValidUvRect(const std::array<float, 4> &uv) noexcept {
            return std::ranges::all_of(uv, [](const float value) {
                       return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
                   }) &&
                   uv[0] <= uv[2] && uv[1] <= uv[3];
        }

        bool ValidPaintLimits(const UiRenderSnapshotLimits &limits) noexcept {
            return limits.commands <= MaximumUiRenderCommands && limits.textRuns <= MaximumUiRenderTextRuns &&
                   limits.glyphs <= MaximumUiRenderGlyphs && limits.resources <= MaximumUiRenderResources;
        }

        bool ValidProjectionLimits(const UiRenderSnapshotLimits &limits) noexcept {
            return limits.clips <= MaximumUiRenderClips && limits.masks <= MaximumUiRenderMasks && limits.transforms > 0 &&
                   limits.transforms <= MaximumUiRenderTransforms;
        }

        Result<void> ValidateTreeDescriptor(const UiElementTree &tree, const UiRenderSnapshotDescriptor &descriptor) {
            if (tree.State() != UiElementTreeState::Active)
                return Failure(UiErrors::ElementTreeLifecycleUnavailable);
            if (descriptor.instance != tree.Instance() || descriptor.canvas != tree.Canvas())
                return Failure(UiErrors::HandleOwnerMismatch);
            if (descriptor.document != tree.SourceDocument() || descriptor.documentRevision != tree.SourceDocumentRevision() ||
                descriptor.treeRevision != tree.Revision())
                return Failure(UiErrors::RevisionStale);
            return Result<void>::Success();
        }

        bool FitsWithin(const UiRenderSnapshotLimits &requested, const UiRenderSnapshotLimits &reserved) noexcept {
            const std::array requestedCounts{requested.commands, requested.textRuns,   requested.glyphs,   requested.clips,
                                             requested.masks,    requested.transforms, requested.resources};
            const std::array reservedCounts{reserved.commands, reserved.textRuns,   reserved.glyphs,   reserved.clips,
                                            reserved.masks,    reserved.transforms, reserved.resources};
            for (std::size_t index = 0; index < requestedCounts.size(); ++index)
                if (requestedCounts[index] > reservedCounts[index])
                    return false;
            return true;
        }

        Result<void> ValidateViewDescriptor(const UiRenderSnapshotDescriptor &descriptor, const UiRenderViewId expectedView,
                                            const UiRenderSnapshotRevision lastRevision, const UiRenderSnapshotLimits &reservedLimits) {
            if (!descriptor.interactionRevision.IsValid() || !descriptor.snapshotRevision.IsValid())
                return Failure(UiErrors::RevisionInvalid);
            if (!descriptor.view.IsValid() || descriptor.view.ownership != descriptor.instance.ownership || descriptor.view != expectedView)
                return Failure(UiErrors::HandleOwnerMismatch);
            if (lastRevision.IsValid() && descriptor.snapshotRevision.Compare(lastRevision) != UiRevisionRelation::Newer)
                return Failure(UiErrors::RevisionStale);
            return descriptor.limits.IsValid() && FitsWithin(descriptor.limits, reservedLimits) ? Result<void>::Success()
                                                                                                : Failure(UiErrors::CapacityExceeded);
        }

        Result<void> ValidateCounts(const UiRenderSnapshotLimits &limits, const UiRenderProjection &projection) {
            const std::array counts{projection.commands.size(), projection.textRuns.size(), projection.glyphs.size(),
                                    projection.clips.size(),    projection.masks.size(),    projection.transforms.size(),
                                    projection.resources.size()};
            const std::array capacities{static_cast<std::size_t>(limits.commands), static_cast<std::size_t>(limits.textRuns),
                                        static_cast<std::size_t>(limits.glyphs),   static_cast<std::size_t>(limits.clips),
                                        static_cast<std::size_t>(limits.masks),    static_cast<std::size_t>(limits.transforms),
                                        static_cast<std::size_t>(limits.resources)};
            return std::ranges::equal(counts, capacities, std::less_equal{}) ? Result<void>::Success()
                                                                             : Failure(UiErrors::CapacityExceeded);
        }

        Result<void> ValidateResources(const std::span<const UiRenderResourceReference> resources) {
            for (const auto &resource : resources)
                if (!resource.asset.IsValid() || !resource.revision.IsValid() || !ValidRole(resource.role))
                    return Failure(UiErrors::RenderResourceReferenceInvalid);
            return Result<void>::Success();
        }

        Result<void> ValidateText(const std::span<const UiTextRun> runs, const std::span<const UiPositionedGlyph> glyphs,
                                  const std::span<const UiRenderResourceReference> resources) {
            for (const auto &run : runs) {
                if (!PresentIndex(run.fontResource, resources.size()) || resources[run.fontResource].role != UiRenderResourceRole::FontFace)
                    return Failure(UiErrors::RenderResourceReferenceInvalid);
                if (run.firstGlyph > glyphs.size() || run.glyphCount > glyphs.size() - run.firstGlyph || !run.color.IsValid())
                    return Failure(UiErrors::RenderSnapshotInvalid);
            }
            for (const auto &glyph : glyphs)
                if (!glyph.extent.IsValid() || !ValidUvRect(glyph.uv))
                    return Failure(UiErrors::RenderSnapshotInvalid);
            return Result<void>::Success();
        }

        Result<void> ValidateClips(const std::span<const UiClip> clips) {
            for (std::uint32_t index = 0; index < clips.size(); ++index) {
                if (!clips[index].rect.extent.IsValid() || (clips[index].parent != NoUiRenderIndex && clips[index].parent >= index))
                    return Failure(UiErrors::RenderSnapshotInvalid);
            }
            return Result<void>::Success();
        }

        Result<void> ValidateMasks(const std::span<const UiMask> masks, const std::span<const UiLogicalTransform> transforms,
                                   const std::span<const UiRenderResourceReference> resources) {
            for (const auto &mask : masks) {
                if (!mask.rect.extent.IsValid() || !PresentIndex(mask.transform, transforms.size()))
                    return Failure(UiErrors::RenderSnapshotInvalid);
                if (!PresentIndex(mask.resource, resources.size()) || resources[mask.resource].role != UiRenderResourceRole::Mask)
                    return Failure(UiErrors::RenderResourceReferenceInvalid);
            }
            return Result<void>::Success();
        }

        Result<void> ValidatePayload(const UiSolidDraw &draw, const std::span<const UiTextRun>,
                                     const std::span<const UiRenderResourceReference>) {
            return draw.color.IsValid() ? Result<void>::Success() : Failure(UiErrors::RenderCommandInvalid);
        }

        Result<void> ValidatePayload(const UiBorderDraw &draw, const std::span<const UiTextRun>,
                                     const std::span<const UiRenderResourceReference>) {
            return draw.width >= 0 && draw.color.IsValid() ? Result<void>::Success() : Failure(UiErrors::RenderCommandInvalid);
        }

        Result<void> ValidatePayload(const UiImageDraw &draw, const std::span<const UiTextRun>,
                                     const std::span<const UiRenderResourceReference> resources) {
            if (!PresentIndex(draw.resource, resources.size()) || resources[draw.resource].role != UiRenderResourceRole::Image)
                return Failure(UiErrors::RenderResourceReferenceInvalid);
            return draw.tint.IsValid() ? Result<void>::Success() : Failure(UiErrors::RenderCommandInvalid);
        }

        Result<void> ValidatePayload(const UiSpriteDraw &draw, const std::span<const UiTextRun>,
                                     const std::span<const UiRenderResourceReference> resources) {
            if (!PresentIndex(draw.resource, resources.size()) || resources[draw.resource].role != UiRenderResourceRole::Image)
                return Failure(UiErrors::RenderResourceReferenceInvalid);
            return draw.tint.IsValid() && ValidUvRect(draw.uv) ? Result<void>::Success()
                                                               : Failure(UiErrors::RenderCommandInvalid);
        }

        Result<void> ValidatePayload(const UiTextDraw &draw, const std::span<const UiTextRun> runs,
                                     const std::span<const UiRenderResourceReference>) {
            return PresentIndex(draw.run, runs.size()) ? Result<void>::Success() : Failure(UiErrors::RenderCommandInvalid);
        }

        Result<void> ValidateCommands(const UiElementTree &tree, const std::span<const UiDrawCommand> commands,
                                      const std::span<const UiTextRun> runs, const std::span<const UiClip> clips,
                                      const std::span<const UiMask> masks, const std::span<const UiLogicalTransform> transforms,
                                      const std::span<const UiRenderResourceReference> resources) {
            for (const auto &command : commands) {
                if (tree.Get(command.element).HasError() || !command.rect.extent.IsValid() || !UnitInterval(command.opacity))
                    return Failure(UiErrors::RenderCommandInvalid);
                if (!PresentIndex(command.transform, transforms.size()) || !OptionalIndex(command.clip, clips.size()) ||
                    !OptionalIndex(command.mask, masks.size()))
                    return Failure(UiErrors::RenderCommandInvalid);
                const auto payload = std::visit([runs, resources]<typename Draw>(const Draw &draw) {
                    return ValidatePayload(draw, runs, resources);
                }, command.payload);
                if (payload.HasError())
                    return payload;
            }
            return Result<void>::Success();
        }

        Result<void> ValidateProjectionTables(const UiRenderProjection &projection) {
            if (const auto resources = ValidateResources(projection.resources); resources.HasError())
                return resources;
            if (!std::ranges::all_of(projection.transforms, &UiLogicalTransform::IsValid))
                return Failure(UiErrors::RenderSnapshotInvalid);
            return ValidateText(projection.textRuns, projection.glyphs, projection.resources);
        }

        Result<void> ValidateProjectionTopology(const UiElementTree &tree, const UiRenderProjection &projection) {
            if (const auto clips = ValidateClips(projection.clips); clips.HasError())
                return clips;
            if (const auto masks = ValidateMasks(projection.masks, projection.transforms, projection.resources); masks.HasError())
                return masks;
            return ValidateCommands(tree, projection.commands, projection.textRuns, projection.clips, projection.masks,
                                    projection.transforms, projection.resources);
        }

        template <typename Value> void CopyInto(std::vector<Value> &destination, const std::span<const Value> source) {
            destination.resize(source.size());
            std::ranges::copy(source, destination.begin());
        }
    }  // namespace

    /** @copydoc UiLinearColor::IsValid */
    bool UiLinearColor::IsValid() const noexcept {
        return UnitInterval(red) && UnitInterval(green) && UnitInterval(blue) && UnitInterval(alpha);
    }

    /** @copydoc UiRenderSnapshotLimits::IsValid */
    bool UiRenderSnapshotLimits::IsValid() const noexcept {
        return ValidPaintLimits(*this) && ValidProjectionLimits(*this);
    }

    /** @copydoc UiRenderExtractorDescriptor::IsValid */
    bool UiRenderExtractorDescriptor::IsValid() const noexcept {
        return view.IsValid() && limits.IsValid() && concurrentSnapshots > 0 && concurrentSnapshots <= MaximumUiRenderSnapshotsInFlight;
    }

    /** @brief One preallocated frame-owned slot guarded by an explicit immutable lease count. */
    struct UiRenderSnapshot::Storage final {
        /** @brief Releases an acquired slot unless publication commits it to a snapshot. */
        class PublishLease final {
        public:
            explicit PublishLease(Storage &storage) noexcept : storage_(&storage) {}

            PublishLease(const PublishLease &) = delete;
            PublishLease &operator=(const PublishLease &) = delete;

            ~PublishLease() {
                if (storage_)
                    storage_->leases.store(0);
            }

            void Commit() noexcept {
                storage_ = nullptr;
            }

        private:
            Storage *storage_;
        };

        mutable std::atomic<std::uint64_t> leases{};
        UiRenderSnapshotDescriptor descriptor;
        std::vector<UiDrawCommand> commands;
        std::vector<UiTextRun> textRuns;
        std::vector<UiPositionedGlyph> glyphs;
        std::vector<UiClip> clips;
        std::vector<UiMask> masks;
        std::vector<UiLogicalTransform> transforms;
        std::vector<UiRenderResourceReference> resources;

        explicit Storage(const UiRenderSnapshotLimits &limits) {
            commands.reserve(limits.commands);
            textRuns.reserve(limits.textRuns);
            glyphs.reserve(limits.glyphs);
            clips.reserve(limits.clips);
            masks.reserve(limits.masks);
            transforms.reserve(limits.transforms);
            resources.reserve(limits.resources);
        }

        /** @brief Copies a validated projection without exceeding the capacities reserved during extractor creation. */
        void Publish(const UiRenderSnapshotDescriptor &sourceDescriptor, const UiRenderProjection &projection) {
            descriptor = sourceDescriptor;
            CopyInto(commands, projection.commands);
            CopyInto(textRuns, projection.textRuns);
            CopyInto(glyphs, projection.glyphs);
            CopyInto(clips, projection.clips);
            CopyInto(masks, projection.masks);
            CopyInto(transforms, projection.transforms);
            CopyInto(resources, projection.resources);
        }
    };

    /** @brief Preallocated snapshot slots and owner-thread admission state for one exact view. */
    struct UiRenderExtractor::Storage final {
        UiRenderExtractorDescriptor descriptor;
        UiRenderExtractorState lifecycle{UiRenderExtractorState::Active};
        std::vector<std::shared_ptr<UiRenderSnapshot::Storage>> slots;
        std::size_t nextSlot{};
        UiRenderSnapshotRevision lastRevision;

        explicit Storage(const UiRenderExtractorDescriptor &source) : descriptor(source) {
            slots.reserve(source.concurrentSnapshots);
            for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
                slots.push_back(std::make_shared<UiRenderSnapshot::Storage>(source.limits));
        }

        /** @brief Validates one transaction against the exact tree, view, revision lineage, and reserved bounds. */
        Result<void> Validate(const UiElementTree &tree, const UiRenderSnapshotDescriptor &snapshotDescriptor,
                              const UiRenderProjection &projection) const {
            if (const auto treeEvidence = ValidateTreeDescriptor(tree, snapshotDescriptor); treeEvidence.HasError())
                return treeEvidence;
            if (const auto viewEvidence = ValidateViewDescriptor(snapshotDescriptor, descriptor.view, lastRevision, descriptor.limits);
                viewEvidence.HasError())
                return viewEvidence;
            if (const auto counts = ValidateCounts(snapshotDescriptor.limits, projection); counts.HasError())
                return counts;
            if (const auto tables = ValidateProjectionTables(projection); tables.HasError())
                return tables;
            return ValidateProjectionTopology(tree, projection);
        }

        /** @brief Acquires one free slot in deterministic round-robin order. */
        std::shared_ptr<UiRenderSnapshot::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                std::uint64_t expected{};
                if (slots[index]->leases.compare_exchange_strong(expected, 1)) {
                    nextSlot = (index + 1) % slots.size();
                    return slots[index];
                }
            }
            return {};
        }

        /** @brief Reports whether every preallocated slot is no longer leased. */
        bool IsDrained() const noexcept {
            return std::ranges::all_of(slots, [](const auto &slot) {
                return slot->leases.load() == 0;
            });
        }
    };

    /** @copydoc UiRenderSnapshot::UiRenderSnapshot(std::shared_ptr<const Storage>) */
    UiRenderSnapshot::UiRenderSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiRenderSnapshot::~UiRenderSnapshot */
    UiRenderSnapshot::~UiRenderSnapshot() {
        Release();
    }

    /** @copydoc UiRenderSnapshot::UiRenderSnapshot(const UiRenderSnapshot&) */
    UiRenderSnapshot::UiRenderSnapshot(const UiRenderSnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiRenderSnapshot::operator=(const UiRenderSnapshot&) */
    UiRenderSnapshot &UiRenderSnapshot::operator=(const UiRenderSnapshot &other) noexcept {
        if (this != &other) {
            UiRenderSnapshot replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiRenderSnapshot::UiRenderSnapshot(UiRenderSnapshot&&) */
    UiRenderSnapshot::UiRenderSnapshot(UiRenderSnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiRenderSnapshot::operator=(UiRenderSnapshot&&) */
    UiRenderSnapshot &UiRenderSnapshot::operator=(UiRenderSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiRenderSnapshot::Retain */
    void UiRenderSnapshot::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load();
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (storage_->leases.compare_exchange_weak(current, current + 1))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiRenderSnapshot::Release */
    void UiRenderSnapshot::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1);
        storage_.reset();
    }

    /** @copydoc UiRenderSnapshot::Descriptor */
    const UiRenderSnapshotDescriptor &UiRenderSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiRenderSnapshot::IsValid */
    bool UiRenderSnapshot::IsValid() const noexcept {
        return static_cast<bool>(storage_);
    }

    /** @copydoc UiRenderSnapshot::Commands */
    std::span<const UiDrawCommand> UiRenderSnapshot::Commands() const noexcept {
        return storage_->commands;
    }

    /** @copydoc UiRenderSnapshot::TextRuns */
    std::span<const UiTextRun> UiRenderSnapshot::TextRuns() const noexcept {
        return storage_->textRuns;
    }

    /** @copydoc UiRenderSnapshot::Glyphs */
    std::span<const UiPositionedGlyph> UiRenderSnapshot::Glyphs() const noexcept {
        return storage_->glyphs;
    }

    /** @copydoc UiRenderSnapshot::Clips */
    std::span<const UiClip> UiRenderSnapshot::Clips() const noexcept {
        return storage_->clips;
    }

    /** @copydoc UiRenderSnapshot::Masks */
    std::span<const UiMask> UiRenderSnapshot::Masks() const noexcept {
        return storage_->masks;
    }

    /** @copydoc UiRenderSnapshot::Transforms */
    std::span<const UiLogicalTransform> UiRenderSnapshot::Transforms() const noexcept {
        return storage_->transforms;
    }

    /** @copydoc UiRenderSnapshot::Resources */
    std::span<const UiRenderResourceReference> UiRenderSnapshot::Resources() const noexcept {
        return storage_->resources;
    }

    /** @copydoc UiRenderExtractor::Create */
    Result<UiRenderExtractor> UiRenderExtractor::Create(const UiRenderExtractorDescriptor &descriptor) {
        if (!descriptor.view.IsValid())
            return Failure<UiRenderExtractor>(UiErrors::HandleMalformed);
        if (!descriptor.IsValid())
            return Failure<UiRenderExtractor>(UiErrors::CapacityExceeded);
        try {
            return Result<UiRenderExtractor>::Success(UiRenderExtractor{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiRenderExtractor>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiRenderExtractor::UiRenderExtractor(std::unique_ptr<Storage>) */
    UiRenderExtractor::UiRenderExtractor(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiRenderExtractor::~UiRenderExtractor */
    UiRenderExtractor::~UiRenderExtractor() {
        Close();
    }

    /** @copydoc UiRenderExtractor::UiRenderExtractor(UiRenderExtractor&&) */
    UiRenderExtractor::UiRenderExtractor(UiRenderExtractor &&) noexcept = default;

    /** @copydoc UiRenderExtractor::operator=(UiRenderExtractor&&) */
    UiRenderExtractor &UiRenderExtractor::operator=(UiRenderExtractor &&) noexcept = default;

    /** @copydoc UiRenderExtractor::Extract */
    Result<UiRenderSnapshot> UiRenderExtractor::Extract(const UiElementTree &tree, const UiRenderSnapshotDescriptor &descriptor,
                                                        const UiRenderProjection &projection) {
        if (!storage_ || storage_->lifecycle != UiRenderExtractorState::Active)
            return Failure<UiRenderSnapshot>(UiErrors::RenderSnapshotLifecycleUnavailable);
        if (const auto validated = storage_->Validate(tree, descriptor, projection); validated.HasError())
            return Result<UiRenderSnapshot>::Failure(validated.ErrorValue());
        auto slot = storage_->TryAcquire();
        if (!slot)
            return Failure<UiRenderSnapshot>(UiErrors::RenderSnapshotStorageExhausted);
        UiRenderSnapshot::Storage::PublishLease publishLease{*slot};
        try {
            slot->Publish(descriptor, projection);
        } catch (const std::bad_alloc &) {
            return Failure<UiRenderSnapshot>(UiErrors::CapacityExceeded);
        }
        publishLease.Commit();
        storage_->lastRevision = descriptor.snapshotRevision;
        return Result<UiRenderSnapshot>::Success(UiRenderSnapshot{std::move(slot)});
    }

    /** @copydoc UiRenderExtractor::Close */
    void UiRenderExtractor::Close() noexcept {
        if (storage_)
            storage_->lifecycle = UiRenderExtractorState::Closed;
    }

    /** @copydoc UiRenderExtractor::IsDrained */
    bool UiRenderExtractor::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiRenderExtractor::State */
    UiRenderExtractorState UiRenderExtractor::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiRenderExtractorState::Closed;
    }
}  // namespace Horo::Runtime::Ui
