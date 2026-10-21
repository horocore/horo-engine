#include "Horo/Runtime/Ui/UiLayoutInspector.h"

#include "Horo/Runtime/Ui/UiHotReload.h"

#include <algorithm>
#include <new>
#include <thread>
#include <utility>

namespace Horo::Runtime::Ui {
    struct UiLayoutInspectionSnapshot::Storage final {
        UiLayoutInspectionDescriptor descriptor;
        std::vector<UiLayoutInspectionRecord> records;
        std::vector<UiLayoutClipRecord> clipping;
        std::vector<UiLayoutClipNode> clips;
        std::vector<UiLayoutScrollRecord> scrolls;

        explicit Storage(const UiLayoutInspectorLimits &limits) {
            records.reserve(limits.elements);
            clipping.reserve(limits.elements);
            clips.reserve(limits.clips);
            scrolls.reserve(limits.scrolls);
        }
    };

    struct UiLayoutInspector::Storage final {
        const std::thread::id thread{std::this_thread::get_id()};
        UiLayoutInspectorLimits limits;
        UiCanvasId canvas;
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId runtimeCanvas;
        UiElementHandle root;
        UiDocumentId document;
        UiDocumentRevision revision;
        std::shared_ptr<const void> issuer;
        std::vector<std::shared_ptr<UiLayoutInspectionSnapshot::Storage>> slots;
        std::vector<UiElementHandle> traversal;
        std::vector<UiElementRecord> hierarchy;
        std::optional<std::size_t> latest;
        bool enabled{};
        bool stopped{};

        Storage(const UiElementTree &tree, const UiCanvasId canvas, const UiLayoutInspectorLimits limits)
            : limits(limits), canvas(canvas), instance(tree.Instance()), runtimeCanvas(tree.Canvas()), root(tree.Root().Value().handle),
              document(tree.SourceDocument()), revision(tree.SourceDocumentRevision()), issuer(tree.IssuerPin()) {
            traversal.reserve(limits.elements);
            hierarchy.reserve(limits.elements);
            slots.reserve(limits.snapshots);
            for (std::uint32_t index = 0; index < limits.snapshots; ++index)
                slots.push_back(std::make_shared<UiLayoutInspectionSnapshot::Storage>(limits));
        }
    };

    /** @copydoc UiLayoutInspectorLimits::IsValid */
    bool UiLayoutInspectorLimits::IsValid() const noexcept {
        return elements > 0 && elements <= MaximumUiTreeElements && clips > 0 && clips <= MaximumUiLayoutClipNodes && scrolls > 0 &&
               scrolls <= MaximumUiLayoutScrollContainers && snapshots >= 2 && snapshots <= MaximumUiLayoutSnapshotsInFlight;
    }

    /** @copydoc UiLayoutInspectionSnapshot::UiLayoutInspectionSnapshot */
    UiLayoutInspectionSnapshot::UiLayoutInspectionSnapshot(std::shared_ptr<const Storage> storage) noexcept
        : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutInspectionSnapshot::Descriptor */
    const UiLayoutInspectionDescriptor &UiLayoutInspectionSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiLayoutInspectionSnapshot::Records */
    std::span<const UiLayoutInspectionRecord> UiLayoutInspectionSnapshot::Records() const noexcept {
        return storage_->records;
    }

    /** @copydoc UiLayoutInspectionSnapshot::Clipping */
    std::span<const UiLayoutClipRecord> UiLayoutInspectionSnapshot::Clipping() const noexcept {
        return storage_->clipping;
    }

    /** @copydoc UiLayoutInspectionSnapshot::Clips */
    std::span<const UiLayoutClipNode> UiLayoutInspectionSnapshot::Clips() const noexcept {
        return storage_->clips;
    }

    /** @copydoc UiLayoutInspectionSnapshot::Scrolls */
    std::span<const UiLayoutScrollRecord> UiLayoutInspectionSnapshot::Scrolls() const noexcept {
        return storage_->scrolls;
    }

    /** @copydoc UiLayoutInspector::Create */
    Result<UiLayoutInspector> UiLayoutInspector::Create(const UiReloadGeneration &generation, const UiCanvasId canvas,
                                                        const UiLayoutInspectorLimits limits) {
        if (!limits.IsValid())
            return Result<UiLayoutInspector>::Failure(MakeError(UiErrors::CapacityExceeded));
        const auto canvases = generation.Canvases();
        const auto source = std::ranges::find(canvases, canvas, &UiReloadCanvas::id);
        if (source == canvases.end() || source->tree.State() != UiElementTreeState::Active)
            return Result<UiLayoutInspector>::Failure(MakeError(UiErrors::LayoutSourceStale));
        try {
            return Result<UiLayoutInspector>::Success(UiLayoutInspector{std::make_unique<Storage>(source->tree, canvas, limits)});
        } catch (const std::bad_alloc &) {
            return Result<UiLayoutInspector>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiLayoutInspector::UiLayoutInspector */
    UiLayoutInspector::UiLayoutInspector(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutInspector::~UiLayoutInspector */
    UiLayoutInspector::~UiLayoutInspector() {
        Shutdown();
    }

    /** @copydoc UiLayoutInspector::UiLayoutInspector */
    UiLayoutInspector::UiLayoutInspector(UiLayoutInspector &&) noexcept = default;

    /** @copydoc UiLayoutInspector::operator= */
    UiLayoutInspector &UiLayoutInspector::operator=(UiLayoutInspector &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiLayoutInspector::Enable */
    void UiLayoutInspector::Enable(const bool enabled) noexcept {
        if (storage_ && storage_->thread == std::this_thread::get_id() && !storage_->stopped)
            storage_->enabled = enabled;
    }

    /** @copydoc UiLayoutInspector::Rebind */
    Result<void> UiLayoutInspector::Rebind(const UiReloadGeneration &generation, const UiCanvasId canvas) {
        if (!storage_ || storage_->thread != std::this_thread::get_id() || storage_->stopped)
            return Result<void>::Failure(MakeError(UiErrors::LayoutLifecycleUnavailable));
        const auto canvases = generation.Canvases();
        const auto source = std::ranges::find(canvases, canvas, &UiReloadCanvas::id);
        if (canvas != storage_->canvas || source == canvases.end() || source->tree.State() != UiElementTreeState::Active ||
            source->tree.Instance() != storage_->instance || source->tree.SourceDocument() != storage_->document ||
            source->tree.SourceDocumentRevision() < storage_->revision)
            return Result<void>::Failure(MakeError(UiErrors::LayoutSourceStale));
        const auto root = source->tree.Root();
        if (root.HasError())
            return Result<void>::Failure(MakeError(UiErrors::LayoutSourceStale));
        storage_->runtimeCanvas = source->tree.Canvas();
        storage_->root = root.Value().handle;
        storage_->revision = source->tree.SourceDocumentRevision();
        storage_->issuer = source->tree.IssuerPin();
        return Result<void>::Success();
    }

    /** @copydoc UiLayoutInspector::Capture */
    UiLayoutInspectionResult UiLayoutInspector::Capture(const UiElementTree &tree, const UiLayoutSnapshot &layout,
                                                        const UiLayoutClipSnapshot *clipping,
                                                        const UiPresentationProfileSelection *selection) noexcept {
        if (!storage_)
            return {UiLayoutInspectionStatus::Stopped};
        auto &owner = *storage_;
        if (owner.thread != std::this_thread::get_id())
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::WrongThread};
        if (owner.stopped)
            return {UiLayoutInspectionStatus::Stopped};
        if (!owner.enabled)
            return {UiLayoutInspectionStatus::Disabled};
        if (tree.State() != UiElementTreeState::Active || tree.IssuerPin() != owner.issuer || tree.Instance() != owner.instance ||
            tree.Canvas() != owner.runtimeCanvas || tree.SourceDocument() != owner.document ||
            tree.SourceDocumentRevision() != owner.revision)
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::Tree};
        const auto root = tree.Root();
        if (root.HasError() || root.Value().handle != owner.root)
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::Tree};
        const auto &source = layout.Descriptor();
        const auto records = layout.Records();
        if (source.instance != tree.Instance() || source.canvas != tree.Canvas() || source.document != tree.SourceDocument() ||
            source.sources.document != tree.SourceDocumentRevision() || source.sources.tree != tree.Revision())
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::LayoutLineage};
        if (records.size() != tree.Size())
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::LayoutMembership};
        if (records.size() > owner.limits.elements ||
            (clipping && (clipping->Clips().size() > owner.limits.clips || clipping->Scrolls().size() > owner.limits.scrolls)))
            return {UiLayoutInspectionStatus::DroppedCapacity};
        if (clipping) {
            const auto &clipSource = clipping->Descriptor();
            if (clipSource.instance != source.instance || clipSource.canvas != source.canvas || clipSource.document != source.document ||
                clipSource.sources != source.sources || clipSource.interaction != source.interaction)
                return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::ClipLineage};
            if (clipping->Records().size() != records.size())
                return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::ClipMembership};
            for (std::size_t index = 0; index < records.size(); ++index)
                if (clipping->Records()[index].element != records[index].element)
                    return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::ClipMembership};
        }
        if (selection &&
            (selection->document != source.document || selection->documentRevision != source.sources.document ||
             selection->canvas != owner.canvas || !selection->tableRevision.IsValid() || !selection->evidence.IsValid() ||
             selection->evidence.canvas != owner.canvas || selection->evidence.instance != source.instance ||
             selection->evidence.runtimeCanvas != source.canvas || selection->evidence.tree != source.sources.tree ||
             selection->profile.has_value() != selection->priority.has_value() || (selection->profile && !selection->profile->IsValid())))
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::SelectionLineage};
        owner.traversal.resize(records.size());
        owner.hierarchy.resize(records.size());
        const auto traversed = tree.Preorder(owner.traversal);
        if (traversed.HasError() || traversed.Value() != records.size())
            return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::LayoutMembership};
        for (std::size_t index = 0; index < records.size(); ++index) {
            if (records[index].element != owner.traversal[index])
                return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::LayoutMembership};
            const auto node = tree.Get(owner.traversal[index]);
            if (node.HasError())
                return {UiLayoutInspectionStatus::RejectedSource, UiLayoutInspectionRejection::LayoutMembership};
            owner.hierarchy[index] = node.Value();
        }
        const auto available = std::ranges::find_if(owner.slots, [](const auto &slot) {
            return slot.use_count() == 1;
        });
        if (available == owner.slots.end())
            return {UiLayoutInspectionStatus::DroppedStorage};
        auto &slot = **available;
        slot.descriptor = {owner.canvas, source, clipping != nullptr, selection ? std::optional{*selection} : std::nullopt};
        slot.records.resize(records.size());
        for (std::size_t index = 0; index < records.size(); ++index)
            slot.records[index] = {owner.hierarchy[index].id, owner.hierarchy[index].parent, records[index]};
        if (clipping) {
            slot.clipping.assign(clipping->Records().begin(), clipping->Records().end());
            slot.clips.assign(clipping->Clips().begin(), clipping->Clips().end());
            slot.scrolls.assign(clipping->Scrolls().begin(), clipping->Scrolls().end());
        } else {
            slot.clipping.clear();
            slot.clips.clear();
            slot.scrolls.clear();
        }
        owner.latest = static_cast<std::size_t>(available - owner.slots.begin());
        return {UiLayoutInspectionStatus::Captured};
    }

    /** @copydoc UiLayoutInspector::Latest */
    std::optional<UiLayoutInspectionSnapshot> UiLayoutInspector::Latest() const noexcept {
        if (!storage_ || storage_->thread != std::this_thread::get_id() || storage_->stopped || !storage_->latest)
            return std::nullopt;
        return UiLayoutInspectionSnapshot{storage_->slots[*storage_->latest]};
    }

    /** @copydoc UiLayoutInspector::Shutdown */
    void UiLayoutInspector::Shutdown() noexcept {
        if (!storage_ || storage_->thread != std::this_thread::get_id())
            return;
        storage_->stopped = true;
        storage_->enabled = false;
        storage_->latest.reset();
        storage_->issuer.reset();
        storage_->slots.clear();
        storage_->traversal.clear();
        storage_->hierarchy.clear();
    }
}  // namespace Horo::Runtime::Ui
