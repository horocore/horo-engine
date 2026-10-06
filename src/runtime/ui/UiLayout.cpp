#include "Horo/Runtime/Ui/UiLayout.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint32_t NoParent = std::numeric_limits<std::uint32_t>::max();

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsDirtyKind(const UiLayoutDirtyKind kind) noexcept {
            return kind >= UiLayoutDirtyKind::Arrange && kind <= UiLayoutDirtyKind::All;
        }

        /** @brief Fences dirty publication to the layout owner's exact retained document identity. */
        [[nodiscard]] Result<void> ValidateLayoutOwner(const UiElementTree &tree, const UiLayoutEngineDescriptor &owner) {
            if (tree.Instance() != owner.instance || tree.Canvas() != owner.canvas || tree.SourceDocument() != owner.document)
                return Failure(UiErrors::HandleOwnerMismatch);
            return Result<void>::Success();
        }

        /** @brief Rejects malformed or foreign-generation dirties before candidate queue mutation. */
        [[nodiscard]] Result<void> ValidateInvalidation(const UiElementTree &tree, const UiLayoutInvalidation &item) {
            if (!item.tree.IsValid() || !IsDirtyKind(item.kind))
                return Failure(UiErrors::LayoutInvalid);
            if (item.tree != tree.Revision())
                return Failure(UiErrors::RevisionStale);
            if (item.kind != UiLayoutDirtyKind::All) {
                if (const auto element = tree.Get(item.element); element.HasError())
                    return Result<void>::Failure(element.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Validates a bounded dirty span without touching queue state. */
        [[nodiscard]] Result<void> ValidateInvalidations(const UiElementTree &tree, const std::span<const UiLayoutInvalidation> items) {
            for (const auto &item : items)
                if (const auto valid = ValidateInvalidation(tree, item); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        /** @brief Coalesces validated work in preallocated scratch; callers publish only after complete capacity admission. */
        [[nodiscard]] Result<void> MergeInvalidations(std::vector<UiLayoutInvalidation> &candidate,
                                                      const std::span<const UiLayoutInvalidation> items, const std::size_t capacity) {
            for (const auto &item : items) {
                if (std::ranges::find(candidate, UiLayoutDirtyKind::All, &UiLayoutInvalidation::kind) != candidate.end())
                    continue;
                if (item.kind == UiLayoutDirtyKind::All) {
                    candidate.clear();
                    candidate.push_back(item);
                    continue;
                }
                if (const auto existing = std::ranges::find(candidate, item.element, &UiLayoutInvalidation::element);
                    existing != candidate.end()) {
                    existing->kind = std::max(existing->kind, item.kind);
                    continue;
                }
                if (candidate.size() == capacity)
                    return Failure(UiErrors::CapacityExceeded);
                candidate.push_back(item);
            }
            return Result<void>::Success();
        }

    }  // namespace

    struct UiLayoutSnapshot::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiLayoutSnapshotDescriptor descriptor;
        std::vector<UiLayoutRecord> records;
        std::vector<std::uint32_t> recordLookup;

        explicit Storage(const std::uint32_t capacity) {
            records.reserve(capacity);
            recordLookup.reserve(capacity);
        }
    };

    struct UiLayoutEngine::Storage final {
        struct Node final {
            UiElementHandle element;
            std::uint32_t parent{NoParent};
            std::uint32_t firstChild{};
            std::uint32_t childCount{};
            UiLayoutConstraints constraints;
            UiLayoutMeasurement measurement;
            UiLogicalRect assignedContent;
            UiLayoutArrangement arrangement;
            bool measureDirty{true};
            bool arrangeDirty{true};
        };

        struct PublishedState final {
            UiLayoutSourceRevisions sources;
            UiLayoutConstraints rootConstraints;
            UiLogicalRect rootContent;
            UiCanvasScaleFactor fontScale;
            UiInteractionRevision interaction;
        };

        UiLayoutEngineDescriptor descriptor;
        UiLayoutEngineState lifecycle{UiLayoutEngineState::Active};
        std::vector<Node> activeNodes;
        std::vector<Node> candidateNodes;
        std::vector<std::uint32_t> activeChildren;
        std::vector<std::uint32_t> candidateChildren;
        std::vector<std::uint32_t> candidateLookup;
        std::vector<UiElementHandle> traversalScratch;
        std::vector<UiElementHandle> childHandleScratch;
        std::vector<UiLayoutConstraints> constraintScratch;
        std::vector<UiLayoutChildMeasurement> measurementScratch;
        std::vector<UiLogicalRect> rectangleScratch;
        std::vector<UiLayoutChildPlacement> placementScratch;
        std::vector<UiLayoutLine> lineScratch;
        std::vector<UiLayoutInvalidation> invalidations;
        std::vector<UiLayoutInvalidation> invalidationScratch;
        std::vector<std::shared_ptr<UiLayoutSnapshot::Storage>> slots;
        std::shared_ptr<UiLayoutSnapshot::Storage> current;
        std::size_t nextSlot{};
        PublishedState published;
        bool prepared{};

        struct Reservation final {
            explicit Reservation(Storage &owner) noexcept : owner(owner) {
                owner.prepared = true;
            }

            ~Reservation() {
                if (!retained)
                    owner.prepared = false;
            }

            Reservation(const Reservation &) = delete;
            Reservation &operator=(const Reservation &) = delete;
            Storage &owner;
            bool retained{};
        };

        explicit Storage(const UiLayoutEngineDescriptor &source)
            : descriptor(source), published{.interaction = source.initialInteractionRevision} {
            activeNodes.reserve(source.elementCapacity);
            candidateNodes.reserve(source.elementCapacity);
            activeChildren.reserve(source.elementCapacity - 1);
            candidateChildren.reserve(source.elementCapacity - 1);
            candidateLookup.reserve(source.elementCapacity);
            traversalScratch.reserve(source.elementCapacity);
            childHandleScratch.reserve(source.elementCapacity);
            constraintScratch.reserve(source.elementCapacity);
            measurementScratch.reserve(source.elementCapacity);
            rectangleScratch.reserve(source.elementCapacity);
            placementScratch.reserve(source.elementCapacity);
            lineScratch.reserve(source.elementCapacity);
            invalidations.reserve(source.invalidationCapacity);
            invalidationScratch.reserve(source.invalidationCapacity);
            slots.reserve(source.concurrentSnapshots);
            for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
                slots.push_back(std::make_shared<UiLayoutSnapshot::Storage>(source.elementCapacity));
        }

        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        ~Storage() {
            ReleaseCurrent();
        }

        [[nodiscard]] std::uint32_t FindCandidate(const UiElementHandle element) const noexcept {
            const auto found = std::ranges::lower_bound(candidateLookup, element, {}, [this](const std::uint32_t index) {
                return candidateNodes[index].element;
            });
            return found != candidateLookup.end() && candidateNodes[*found].element == element ? *found : NoParent;
        }

        [[nodiscard]] Result<void> BuildTopology(const UiElementTree &tree) {
            traversalScratch.resize(descriptor.elementCapacity);
            const auto preorder = tree.Preorder(traversalScratch);
            if (preorder.HasError())
                return Result<void>::Failure(preorder.ErrorValue());
            traversalScratch.resize(preorder.Value());
            if (traversalScratch.empty() || traversalScratch.size() > descriptor.elementCapacity)
                return Failure(UiErrors::CapacityExceeded);

            candidateNodes.clear();
            candidateChildren.clear();
            candidateLookup.clear();
            candidateNodes.resize(traversalScratch.size());
            candidateLookup.resize(traversalScratch.size());
            for (std::uint32_t index = 0; index < traversalScratch.size(); ++index) {
                candidateNodes[index].element = traversalScratch[index];
                candidateLookup[index] = index;
            }
            std::ranges::sort(candidateLookup, {}, [this](const std::uint32_t index) {
                return candidateNodes[index].element;
            });

            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                const auto record = tree.Get(candidateNodes[index].element);
                if (record.HasError())
                    return Result<void>::Failure(record.ErrorValue());
                if (record.Value().parent.IsValid()) {
                    const auto parent = FindCandidate(record.Value().parent);
                    if (parent == NoParent || parent >= index)
                        return Failure(UiErrors::ElementTreeInvalid);
                    candidateNodes[index].parent = parent;
                }

                childHandleScratch.resize(descriptor.elementCapacity);
                const auto children = tree.Children(candidateNodes[index].element, childHandleScratch);
                if (children.HasError())
                    return Result<void>::Failure(children.ErrorValue());
                childHandleScratch.resize(children.Value());
                candidateNodes[index].firstChild = static_cast<std::uint32_t>(candidateChildren.size());
                candidateNodes[index].childCount = static_cast<std::uint32_t>(children.Value());
                for (const auto child : childHandleScratch) {
                    const auto childIndex = FindCandidate(child);
                    if (childIndex == NoParent)
                        return Failure(UiErrors::ElementTreeInvalid);
                    candidateChildren.push_back(childIndex);
                }
            }
            return Result<void>::Success();
        }

        void MarkAll() noexcept {
            for (auto &node : candidateNodes) {
                node.measureDirty = true;
                node.arrangeDirty = true;
            }
        }

        void MarkMeasureDirtyToRoot(std::uint32_t index) noexcept {
            while (index != NoParent) {
                candidateNodes[index].measureDirty = true;
                candidateNodes[index].arrangeDirty = true;
                index = candidateNodes[index].parent;
            }
        }

        [[nodiscard]] Result<void> ApplyInvalidations(const UiRuntimeTreeRevision treeRevision) {
            for (const auto &invalidation : invalidations) {
                if (!IsDirtyKind(invalidation.kind) || !invalidation.tree.IsValid())
                    return Failure(UiErrors::LayoutInvalid);
                if (invalidation.tree != treeRevision)
                    return Failure(UiErrors::LayoutSourceStale);
                if (invalidation.kind == UiLayoutDirtyKind::All || invalidation.kind == UiLayoutDirtyKind::Structure) {
                    MarkAll();
                    continue;
                }
                auto index = FindCandidate(invalidation.element);
                if (index == NoParent)
                    return Failure(UiErrors::LayoutSourceStale);
                candidateNodes[index].arrangeDirty = true;
                if (invalidation.kind == UiLayoutDirtyKind::Measure) {
                    while (index != NoParent) {
                        candidateNodes[index].measureDirty = true;
                        candidateNodes[index].arrangeDirty = true;
                        index = candidateNodes[index].parent;
                    }
                }
            }
            return Result<void>::Success();
        }

        std::span<const UiElementHandle> ChildHandles(const Node &node) {
            childHandleScratch.resize(node.childCount);
            for (std::uint32_t offset = 0; offset < node.childCount; ++offset)
                childHandleScratch[offset] = candidateNodes[candidateChildren[node.firstChild + offset]].element;
            return {childHandleScratch.data(), node.childCount};
        }

        std::span<const UiLayoutChildMeasurement> ChildMeasurements(const Node &node) {
            measurementScratch.resize(node.childCount);
            for (std::uint32_t offset = 0; offset < node.childCount; ++offset) {
                const auto &child = candidateNodes[candidateChildren[node.firstChild + offset]];
                measurementScratch[offset] = {child.element, child.measurement};
            }
            return {measurementScratch.data(), node.childCount};
        }

        [[nodiscard]] Result<void> ResolveConstraints(const UiLayoutUpdateRequest &request) {
            for (const Node &node : candidateNodes) {
                if (!node.measureDirty || node.childCount == 0)
                    continue;
                constraintScratch.resize(node.childCount);
                const UiLayoutChildConstraintRequest childRequest{node.element, node.constraints, ChildHandles(node)};
                if (const auto resolved = request.evaluator->ResolveChildConstraints(childRequest, constraintScratch); resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                for (std::uint32_t offset = 0; offset < node.childCount; ++offset) {
                    if (!constraintScratch[offset].IsValid())
                        return Failure(UiErrors::LayoutInvalid);
                    auto &child = candidateNodes[candidateChildren[node.firstChild + offset]];
                    if (child.constraints != constraintScratch[offset]) {
                        child.constraints = constraintScratch[offset];
                        child.measureDirty = true;
                        child.arrangeDirty = true;
                    }
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MeasureNodes(const UiLayoutUpdateRequest &request, const bool remeasure) {
            for (std::size_t position = candidateNodes.size(); position > 0; --position) {
                auto &node = candidateNodes[position - 1];
                if (!node.measureDirty)
                    continue;
                const UiLayoutMeasureRequest measureRequest{node.element, node.constraints, ChildMeasurements(node), remeasure,
                                                            request.fontScale};
                const auto measured = request.evaluator->Measure(measureRequest);
                if (measured.HasError())
                    return Result<void>::Failure(measured.ErrorValue());
                if (!measured.Value().IsValid(node.constraints))
                    return Failure(UiErrors::LayoutInvalid);
                if (node.measurement != measured.Value())
                    node.arrangeDirty = true;
                node.measurement = measured.Value();
                node.measureDirty = false;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<bool> ArrangeNodes(const UiLayoutUpdateRequest &request, const bool remeasure) {
            bool needsRemeasure = false;
            for (Node &node : candidateNodes) {
                if (!node.arrangeDirty)
                    continue;
                rectangleScratch.resize(node.childCount);
                placementScratch.resize(node.childCount);
                lineScratch.resize(node.childCount);
                const UiLayoutArrangeRequest arrangeRequest{node.element,
                                                            node.assignedContent,
                                                            node.measurement,
                                                            ChildMeasurements(node),
                                                            remeasure,
                                                            std::span{placementScratch},
                                                            std::span{lineScratch}};
                const auto arranged = request.evaluator->Arrange(arrangeRequest, rectangleScratch);
                if (arranged.HasError())
                    return Result<bool>::Failure(arranged.ErrorValue());
                if (!arranged.Value().IsValid() || !std::ranges::all_of(rectangleScratch, &UiLogicalRect::IsValid))
                    return Failure<bool>(UiErrors::LayoutInvalid);
                node.arrangement = arranged.Value();
                node.arrangeDirty = false;
                for (std::uint32_t offset = 0; offset < node.childCount; ++offset) {
                    auto &child = candidateNodes[candidateChildren[node.firstChild + offset]];
                    if (child.assignedContent == rectangleScratch[offset])
                        continue;
                    const auto previousAssignment = child.assignedContent;
                    child.assignedContent = rectangleScratch[offset];
                    child.arrangeDirty = true;
                    const bool dependent =
                        (child.measurement.dependsOnParentWidth && previousAssignment.extent.width != child.assignedContent.extent.width) ||
                        (child.measurement.dependsOnParentHeight &&
                         previousAssignment.extent.height != child.assignedContent.extent.height);
                    if (dependent) {
                        MarkMeasureDirtyToRoot(candidateChildren[node.firstChild + offset]);
                        needsRemeasure = true;
                    }
                }
            }
            return Result<bool>::Success(needsRemeasure);
        }

        [[nodiscard]] Result<bool> EvaluatePass(const UiLayoutUpdateRequest &request, const bool remeasure) {
            candidateNodes.front().constraints = request.rootConstraints;
            candidateNodes.front().assignedContent = request.rootContent;
            if (const auto resolved = ResolveConstraints(request); resolved.HasError())
                return Result<bool>::Failure(resolved.ErrorValue());
            if (const auto measured = MeasureNodes(request, remeasure); measured.HasError())
                return Result<bool>::Failure(measured.ErrorValue());
            return ArrangeNodes(request, remeasure);
        }

        [[nodiscard]] Result<void> PrepareCandidate(const UiElementTree &tree, const UiLayoutUpdateRequest &request,
                                                    const bool topologyChanged, const bool rootChanged, const bool sourcesChanged) {
            if (topologyChanged) {
                if (const auto topology = BuildTopology(tree); topology.HasError())
                    return topology;
                MarkAll();
            } else {
                candidateNodes = activeNodes;
                candidateChildren = activeChildren;
            }
            if (rootChanged || (sourcesChanged && invalidations.empty()))
                MarkAll();
            return ApplyInvalidations(request.sources.tree);
        }

        [[nodiscard]] Result<void> EvaluateCandidate(const UiLayoutUpdateRequest &request) {
            const auto firstPass = EvaluatePass(request, false);
            if (firstPass.HasError())
                return Result<void>::Failure(firstPass.ErrorValue());
            if (!firstPass.Value())
                return Result<void>::Success();
            const auto secondPass = EvaluatePass(request, true);
            if (secondPass.HasError())
                return Result<void>::Failure(secondPass.ErrorValue());
            return secondPass.Value() ? Failure(UiErrors::LayoutNonConvergent) : Result<void>::Success();
        }

        [[nodiscard]] std::shared_ptr<UiLayoutSnapshot::Storage> TryAcquire() noexcept {
            std::size_t offset = 0;
            while (offset < slots.size()) {
                const auto index = (nextSlot + offset) % slots.size();
                if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1)) {
                    ++offset;
                    continue;
                }
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
            return {};
        }

        [[nodiscard]] Result<std::shared_ptr<UiLayoutSnapshot::Storage>> BuildSnapshot(const UiLayoutUpdateRequest &request) {
            UiInteractionRevision publication = published.interaction;
            if (current) {
                const auto next = published.interaction.Next();
                if (next.HasError())
                    return Result<std::shared_ptr<UiLayoutSnapshot::Storage>>::Failure(next.ErrorValue());
                publication = next.Value();
            }

            auto slot = TryAcquire();
            if (!slot)
                return Failure<std::shared_ptr<UiLayoutSnapshot::Storage>>(UiErrors::LayoutSnapshotStorageExhausted);
            slot->descriptor = {descriptor.instance, descriptor.canvas, descriptor.document,
                                request.sources,     publication,       request.fontScale};
            slot->records.resize(candidateNodes.size());
            slot->recordLookup.resize(candidateNodes.size());
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                const auto &node = candidateNodes[index];
                slot->records[index] = {node.element, node.measurement, node.arrangement};
                slot->recordLookup[index] = index;
            }
            std::ranges::sort(slot->recordLookup, {}, [&records = slot->records](const std::uint32_t index) {
                return records[index].element;
            });

            return Result<std::shared_ptr<UiLayoutSnapshot::Storage>>::Success(std::move(slot));
        }

        void ReleaseCurrent() noexcept {
            if (current) {
                current->leases.fetch_sub(1);
                current.reset();
            }
        }
    };

    /** @copydoc UiLayoutSnapshot::UiLayoutSnapshot */
    UiLayoutSnapshot::UiLayoutSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutSnapshot::~UiLayoutSnapshot */
    UiLayoutSnapshot::~UiLayoutSnapshot() {
        Release();
    }

    /** @copydoc UiLayoutSnapshot::UiLayoutSnapshot */
    UiLayoutSnapshot::UiLayoutSnapshot(const UiLayoutSnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiLayoutSnapshot::operator= */
    UiLayoutSnapshot &UiLayoutSnapshot::operator=(const UiLayoutSnapshot &other) noexcept {
        if (this != &other) {
            Release();
            storage_ = other.storage_;
            Retain();
        }
        return *this;
    }

    /** @copydoc UiLayoutSnapshot::UiLayoutSnapshot */
    UiLayoutSnapshot::UiLayoutSnapshot(UiLayoutSnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiLayoutSnapshot::operator= */
    UiLayoutSnapshot &UiLayoutSnapshot::operator=(UiLayoutSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiLayoutSnapshot::Retain */
    void UiLayoutSnapshot::Retain() const noexcept {
        if (storage_)
            storage_->leases.fetch_add(1);
    }

    /** @copydoc UiLayoutSnapshot::Release */
    void UiLayoutSnapshot::Release() noexcept {
        if (storage_) {
            storage_->leases.fetch_sub(1);
            storage_.reset();
        }
    }

    /** @copydoc UiLayoutSnapshot::Descriptor */
    const UiLayoutSnapshotDescriptor &UiLayoutSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiLayoutSnapshot::Records */
    std::span<const UiLayoutRecord> UiLayoutSnapshot::Records() const noexcept {
        return storage_->records;
    }

    /** @copydoc UiLayoutSnapshot::Get */
    Result<UiLayoutRecord> UiLayoutSnapshot::Get(const UiElementHandle element) const {
        if (!storage_ || !element.IsValid())
            return Failure<UiLayoutRecord>(UiErrors::LayoutInvalid);
        const auto found = std::ranges::lower_bound(storage_->recordLookup, element, {}, [this](const std::uint32_t index) {
            return storage_->records[index].element;
        });
        if (found == storage_->recordLookup.end() || storage_->records[*found].element != element)
            return Failure<UiLayoutRecord>(UiErrors::HandleStale);
        return Result<UiLayoutRecord>::Success(storage_->records[*found]);
    }

    /** @copydoc UiLayoutEngine::Create */
    Result<UiLayoutEngine> UiLayoutEngine::Create(const UiLayoutEngineDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiLayoutEngine>(UiErrors::LayoutInvalid);
        try {
            return Result<UiLayoutEngine>::Success(UiLayoutEngine{std::make_shared<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiLayoutEngine>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiLayoutEngine::UiLayoutEngine */
    UiLayoutEngine::UiLayoutEngine(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutEngine::~UiLayoutEngine */
    UiLayoutEngine::~UiLayoutEngine() {
        Shutdown();
    }

    /** @copydoc UiLayoutEngine::UiLayoutEngine */
    UiLayoutEngine::UiLayoutEngine(UiLayoutEngine &&) noexcept = default;

    /** @copydoc UiLayoutEngine::operator= */
    UiLayoutEngine &UiLayoutEngine::operator=(UiLayoutEngine &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiLayoutEngine::Invalidate */
    Result<void> UiLayoutEngine::Invalidate(const UiLayoutInvalidation &invalidation) {
        if (!storage_ || storage_->lifecycle != UiLayoutEngineState::Active)
            return Failure(UiErrors::LayoutLifecycleUnavailable);
        if (storage_->prepared)
            return Failure(UiErrors::LayoutCandidateBusy);
        if (!invalidation.tree.IsValid() || !IsDirtyKind(invalidation.kind) ||
            (invalidation.kind != UiLayoutDirtyKind::All && !invalidation.element.IsValid()))
            return Failure(UiErrors::LayoutInvalid);
        if (const auto all = std::ranges::find(storage_->invalidations, UiLayoutDirtyKind::All, &UiLayoutInvalidation::kind);
            all != storage_->invalidations.end())
            return Result<void>::Success();
        if (invalidation.kind == UiLayoutDirtyKind::All) {
            storage_->invalidations.clear();
            storage_->invalidations.push_back(invalidation);
            return Result<void>::Success();
        }
        if (const auto existing = std::ranges::find_if(storage_->invalidations,
                                                       [&invalidation](const UiLayoutInvalidation &queued) {
            return queued.tree == invalidation.tree && queued.element == invalidation.element;
        });
            existing != storage_->invalidations.end()) {
            existing->kind = std::max(existing->kind, invalidation.kind);
            return Result<void>::Success();
        }
        if (storage_->invalidations.size() == storage_->descriptor.invalidationCapacity)
            return Failure(UiErrors::CapacityExceeded);
        storage_->invalidations.push_back(invalidation);
        return Result<void>::Success();
    }

    /** @copydoc UiLayoutEngine::InvalidateBatch */
    Result<void> UiLayoutEngine::InvalidateBatch(const UiElementTree &tree, const std::span<const UiLayoutInvalidation> invalidations) {
        if (!storage_ || storage_->lifecycle != UiLayoutEngineState::Active || tree.State() != UiElementTreeState::Active)
            return Failure(UiErrors::LayoutLifecycleUnavailable);
        if (storage_->prepared)
            return Failure(UiErrors::LayoutCandidateBusy);
        const auto &owner = storage_->descriptor;
        if (const auto valid = ValidateLayoutOwner(tree, owner); valid.HasError())
            return valid;
        if (invalidations.size() > MaximumUiStructuralCommands)
            return Failure(UiErrors::CapacityExceeded);
        if (const auto valid = ValidateInvalidations(tree, storage_->invalidations); valid.HasError())
            return valid;
        if (const auto valid = ValidateInvalidations(tree, invalidations); valid.HasError())
            return valid;

        auto &candidate = storage_->invalidationScratch;
        candidate = storage_->invalidations;
        if (const auto merged = MergeInvalidations(candidate, invalidations, owner.invalidationCapacity); merged.HasError())
            return merged;
        storage_->invalidations.swap(candidate);
        return Result<void>::Success();
    }

    /** @copydoc UiLayoutEngine::Prepare */
    Result<UiLayoutEngine::PreparedUpdate> UiLayoutEngine::Prepare(const UiElementTree &tree, const UiLayoutUpdateRequest &request) {
        const auto owner = storage_;
        if (!owner || owner->lifecycle != UiLayoutEngineState::Active)
            return Failure<PreparedUpdate>(UiErrors::LayoutLifecycleUnavailable);
        if (owner->prepared)
            return Failure<PreparedUpdate>(UiErrors::LayoutCandidateBusy);
        Storage::Reservation reservation{*owner};
        if (!request.sources.IsValid() || !request.rootConstraints.IsValid() || !request.rootContent.IsValid() ||
            !request.fontScale.IsValid() || request.evaluator == nullptr)
            return Failure<PreparedUpdate>(UiErrors::LayoutInvalid);
        if (tree.State() != UiElementTreeState::Active || tree.Instance() != owner->descriptor.instance ||
            tree.Canvas() != owner->descriptor.canvas || tree.SourceDocument() != owner->descriptor.document ||
            tree.SourceDocumentRevision() != request.sources.document || tree.Revision() != request.sources.tree ||
            tree.Size() > owner->descriptor.elementCapacity)
            return Failure<PreparedUpdate>(UiErrors::LayoutSourceStale);

        const auto treeIssuer = tree.IssuerPin();
        const auto admittedRoot = tree.Root();
        if (admittedRoot.HasError())
            return Failure<PreparedUpdate>(UiErrors::LayoutSourceStale);
        const bool topologyChanged = owner->activeNodes.empty() || owner->published.sources.tree != request.sources.tree;
        const bool rootChanged =
            owner->current && (owner->published.rootConstraints != request.rootConstraints ||
                               owner->published.rootContent != request.rootContent || owner->published.fontScale != request.fontScale);
        const bool sourcesChanged = !owner->current || owner->published.sources != request.sources ||
                                    owner->published.rootConstraints != request.rootConstraints ||
                                    owner->published.rootContent != request.rootContent || owner->published.fontScale != request.fontScale;
        if (!sourcesChanged && owner->invalidations.empty()) {
            owner->current->leases.fetch_add(1);
            reservation.retained = true;
            return Result<PreparedUpdate>::Success(PreparedUpdate{owner, UiLayoutSnapshot{owner->current}, false, tree, request});
        }

        if (const auto prepared = owner->PrepareCandidate(tree, request, topologyChanged, rootChanged, sourcesChanged); prepared.HasError())
            return Result<PreparedUpdate>::Failure(prepared.ErrorValue());
        if (const auto evaluated = owner->EvaluateCandidate(request); evaluated.HasError())
            return Result<PreparedUpdate>::Failure(evaluated.ErrorValue());
        if (owner->lifecycle != UiLayoutEngineState::Active)
            return Failure<PreparedUpdate>(UiErrors::LayoutLifecycleUnavailable);
        const auto evaluatedRoot = tree.Root();
        if (evaluatedRoot.HasError() || evaluatedRoot.Value().handle != admittedRoot.Value().handle || tree.IssuerPin() != treeIssuer ||
            tree.State() != UiElementTreeState::Active || tree.Revision() != request.sources.tree ||
            tree.SourceDocumentRevision() != request.sources.document)
            return Failure<PreparedUpdate>(UiErrors::LayoutSourceStale);
        auto published = owner->BuildSnapshot(request);
        if (published.HasError())
            return Result<PreparedUpdate>::Failure(published.ErrorValue());
        reservation.retained = true;
        return Result<PreparedUpdate>::Success(PreparedUpdate{owner, UiLayoutSnapshot{std::move(published).Value()}, true, tree, request});
    }

    /** @copydoc UiLayoutEngine::PreparedUpdate::PreparedUpdate */
    UiLayoutEngine::PreparedUpdate::PreparedUpdate(std::shared_ptr<Storage> owner, UiLayoutSnapshot snapshot, const bool changes,
                                                   const UiElementTree &tree, const UiLayoutUpdateRequest &request) noexcept
        : owner_(std::move(owner)), snapshot_(std::move(snapshot)), treeIssuer_(tree.IssuerPin()),
          root_(snapshot_->Records().front().element), rootConstraints_(request.rootConstraints), rootContent_(request.rootContent),
          fontScale_(request.fontScale), changes_(changes) {}

    /** @copydoc UiLayoutEngine::PreparedUpdate::~PreparedUpdate */
    UiLayoutEngine::PreparedUpdate::~PreparedUpdate() {
        Abandon();
    }

    /** @copydoc UiLayoutEngine::PreparedUpdate::PreparedUpdate */
    UiLayoutEngine::PreparedUpdate::PreparedUpdate(PreparedUpdate &&other) noexcept
        : owner_(std::move(other.owner_)), snapshot_(std::move(other.snapshot_)), treeIssuer_(std::move(other.treeIssuer_)),
          root_(other.root_), rootConstraints_(other.rootConstraints_), rootContent_(other.rootContent_), fontScale_(other.fontScale_),
          changes_(other.changes_) {}

    /** @copydoc UiLayoutEngine::PreparedUpdate::operator= */
    UiLayoutEngine::PreparedUpdate &UiLayoutEngine::PreparedUpdate::operator=(PreparedUpdate &&other) noexcept {
        if (this != &other) {
            Abandon();
            owner_ = std::move(other.owner_);
            snapshot_ = std::move(other.snapshot_);
            treeIssuer_ = std::move(other.treeIssuer_);
            root_ = other.root_;
            rootConstraints_ = other.rootConstraints_;
            rootContent_ = other.rootContent_;
            fontScale_ = other.fontScale_;
            changes_ = other.changes_;
        }
        return *this;
    }

    /** @copydoc UiLayoutEngine::PreparedUpdate::Abandon */
    void UiLayoutEngine::PreparedUpdate::Abandon() noexcept {
        if (owner_)
            owner_->prepared = false;
        snapshot_.reset();
        treeIssuer_.reset();
        owner_.reset();
    }

    /** @copydoc UiLayoutEngine::PreparedUpdate::Candidate */
    const UiLayoutSnapshot &UiLayoutEngine::PreparedUpdate::Candidate() const noexcept {
        return *snapshot_;
    }

    /** @copydoc UiLayoutEngine::PreparedUpdate::CanPublish */
    Result<void> UiLayoutEngine::PreparedUpdate::CanPublish(const UiElementTree &tree) const {
        if (!owner_ || !snapshot_ || !owner_->prepared || owner_->lifecycle != UiLayoutEngineState::Active)
            return Failure(UiErrors::LayoutLifecycleUnavailable);
        const auto &candidate = snapshot_->Descriptor();
        const auto root = tree.Root();
        if (root.HasError() || root.Value().handle != root_ || tree.IssuerPin() != treeIssuer_ ||
            tree.State() != UiElementTreeState::Active || tree.Instance() != candidate.instance || tree.Canvas() != candidate.canvas ||
            tree.SourceDocument() != candidate.document || tree.SourceDocumentRevision() != candidate.sources.document ||
            tree.Revision() != candidate.sources.tree)
            return Failure(UiErrors::LayoutSourceStale);
        return Result<void>::Success();
    }

    /** @copydoc UiLayoutEngine::PublishValidated */
    UiLayoutSnapshot UiLayoutEngine::PublishValidated(PreparedUpdate &&candidate) noexcept {
        if (candidate.changes_) {
            storage_->ReleaseCurrent();
            storage_->current = std::const_pointer_cast<UiLayoutSnapshot::Storage>(candidate.snapshot_->storage_);
            storage_->current->leases.fetch_add(1);
            storage_->activeNodes.swap(storage_->candidateNodes);
            storage_->activeChildren.swap(storage_->candidateChildren);
            const auto &descriptor = candidate.snapshot_->Descriptor();
            storage_->published = {descriptor.sources, candidate.rootConstraints_, candidate.rootContent_, candidate.fontScale_,
                                   descriptor.interaction};
            storage_->invalidations.clear();
        }
        auto published = std::move(*candidate.snapshot_);
        candidate.Abandon();
        return published;
    }

    /** @copydoc UiLayoutEngine::Commit */
    Result<UiLayoutSnapshot> UiLayoutEngine::Commit(PreparedUpdate &&candidate, const UiElementTree &tree) {
        if (candidate.owner_ != storage_)
            return Failure<UiLayoutSnapshot>(UiErrors::LayoutSourceStale);
        if (const auto admitted = candidate.CanPublish(tree); admitted.HasError())
            return Result<UiLayoutSnapshot>::Failure(admitted.ErrorValue());
        return Result<UiLayoutSnapshot>::Success(PublishValidated(std::move(candidate)));
    }

    /** @copydoc UiLayoutEngine::Update */
    Result<UiLayoutSnapshot> UiLayoutEngine::Update(const UiElementTree &tree, const UiLayoutUpdateRequest &request) {
        auto candidate = Prepare(tree, request);
        if (candidate.HasError())
            return Result<UiLayoutSnapshot>::Failure(candidate.ErrorValue());
        return Commit(std::move(candidate).Value(), tree);
    }

    /** @copydoc UiLayoutEngine::BeginRetirement */
    Result<void> UiLayoutEngine::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiLayoutEngineState::Active)
            return Failure(UiErrors::LayoutLifecycleUnavailable);
        storage_->lifecycle = UiLayoutEngineState::Retiring;
        storage_->invalidations.clear();
        return Result<void>::Success();
    }

    /** @copydoc UiLayoutEngine::Shutdown */
    void UiLayoutEngine::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiLayoutEngineState::Stopped)
            return;
        storage_->lifecycle = UiLayoutEngineState::Stopped;
        storage_->invalidations.clear();
        // An evaluator may reenter Shutdown. Its bounded scratch references stay alive until preparation unwinds.
        if (!storage_->prepared) {
            storage_->activeNodes.clear();
            storage_->candidateNodes.clear();
            storage_->activeChildren.clear();
            storage_->candidateChildren.clear();
        }
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiLayoutEngine::State */
    UiLayoutEngineState UiLayoutEngine::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiLayoutEngineState::Stopped;
    }

    /** @copydoc UiLayoutEngine::PublishedInteraction */
    UiInteractionRevision UiLayoutEngine::PublishedInteraction() const noexcept {
        return storage_ ? storage_->published.interaction : UiInteractionRevision{};
    }

    /** @copydoc UiLayoutEngine::IsDrained */
    bool UiLayoutEngine::IsDrained() const noexcept {
        if (!storage_)
            return true;
        return std::ranges::all_of(storage_->slots, [&storage = *storage_](const auto &slot) {
            const auto leases = slot->leases.load();
            return leases == 0 || (slot == storage.current && leases == 1);
        });
    }
}  // namespace Horo::Runtime::Ui
