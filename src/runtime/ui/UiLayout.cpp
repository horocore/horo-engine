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
        std::vector<UiLayoutInvalidation> invalidations;
        std::vector<std::shared_ptr<UiLayoutSnapshot::Storage>> slots;
        std::shared_ptr<UiLayoutSnapshot::Storage> current;
        std::size_t nextSlot{};
        PublishedState published;

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
            invalidations.reserve(source.invalidationCapacity);
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
                const UiLayoutArrangeRequest arrangeRequest{node.element, node.assignedContent, node.measurement, ChildMeasurements(node),
                                                            remeasure};
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

        [[nodiscard]] Result<std::shared_ptr<UiLayoutSnapshot::Storage>> PublishCandidate(const UiLayoutUpdateRequest &request) {
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

            ReleaseCurrent();
            current = slot;
            activeNodes.swap(candidateNodes);
            activeChildren.swap(candidateChildren);
            published.sources = request.sources;
            published.rootConstraints = request.rootConstraints;
            published.rootContent = request.rootContent;
            published.fontScale = request.fontScale;
            published.interaction = publication;
            invalidations.clear();
            slot->leases.fetch_add(1);
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
            return Result<UiLayoutEngine>::Success(UiLayoutEngine{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiLayoutEngine>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiLayoutEngine::UiLayoutEngine */
    UiLayoutEngine::UiLayoutEngine(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutEngine::~UiLayoutEngine */
    UiLayoutEngine::~UiLayoutEngine() {
        Shutdown();
    }

    /** @copydoc UiLayoutEngine::UiLayoutEngine */
    UiLayoutEngine::UiLayoutEngine(UiLayoutEngine &&) noexcept = default;

    /** @copydoc UiLayoutEngine::operator= */
    UiLayoutEngine &UiLayoutEngine::operator=(UiLayoutEngine &&) noexcept = default;

    /** @copydoc UiLayoutEngine::Invalidate */
    Result<void> UiLayoutEngine::Invalidate(const UiLayoutInvalidation &invalidation) {
        if (!storage_ || storage_->lifecycle != UiLayoutEngineState::Active)
            return Failure(UiErrors::LayoutLifecycleUnavailable);
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

    /** @copydoc UiLayoutEngine::Update */
    Result<UiLayoutSnapshot> UiLayoutEngine::Update(const UiElementTree &tree, const UiLayoutUpdateRequest &request) {
        if (!storage_ || storage_->lifecycle != UiLayoutEngineState::Active)
            return Failure<UiLayoutSnapshot>(UiErrors::LayoutLifecycleUnavailable);
        if (!request.sources.IsValid() || !request.rootConstraints.IsValid() || !request.rootContent.IsValid() ||
            !request.fontScale.IsValid() || request.evaluator == nullptr)
            return Failure<UiLayoutSnapshot>(UiErrors::LayoutInvalid);
        if (tree.State() != UiElementTreeState::Active || tree.Instance() != storage_->descriptor.instance ||
            tree.Canvas() != storage_->descriptor.canvas || tree.SourceDocument() != storage_->descriptor.document ||
            tree.SourceDocumentRevision() != request.sources.document || tree.Revision() != request.sources.tree ||
            tree.Size() > storage_->descriptor.elementCapacity)
            return Failure<UiLayoutSnapshot>(UiErrors::LayoutSourceStale);

        const bool topologyChanged = storage_->activeNodes.empty() || storage_->published.sources.tree != request.sources.tree;
        const bool rootChanged = storage_->current && (storage_->published.rootConstraints != request.rootConstraints ||
                                                       storage_->published.rootContent != request.rootContent ||
                                                       storage_->published.fontScale != request.fontScale);
        const bool sourcesChanged = !storage_->current || storage_->published.sources != request.sources ||
                                    storage_->published.rootConstraints != request.rootConstraints ||
                                    storage_->published.rootContent != request.rootContent ||
                                    storage_->published.fontScale != request.fontScale;
        if (!sourcesChanged && storage_->invalidations.empty()) {
            storage_->current->leases.fetch_add(1);
            return Result<UiLayoutSnapshot>::Success(UiLayoutSnapshot{storage_->current});
        }

        if (const auto prepared = storage_->PrepareCandidate(tree, request, topologyChanged, rootChanged, sourcesChanged);
            prepared.HasError())
            return Result<UiLayoutSnapshot>::Failure(prepared.ErrorValue());
        if (const auto evaluated = storage_->EvaluateCandidate(request); evaluated.HasError())
            return Result<UiLayoutSnapshot>::Failure(evaluated.ErrorValue());
        auto published = storage_->PublishCandidate(request);
        if (published.HasError())
            return Result<UiLayoutSnapshot>::Failure(published.ErrorValue());
        return Result<UiLayoutSnapshot>::Success(UiLayoutSnapshot{std::move(published).Value()});
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
        storage_->activeNodes.clear();
        storage_->candidateNodes.clear();
        storage_->activeChildren.clear();
        storage_->candidateChildren.clear();
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiLayoutEngine::State */
    UiLayoutEngineState UiLayoutEngine::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiLayoutEngineState::Stopped;
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
