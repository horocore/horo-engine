#include "UiStyleInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    struct UiStyleResolver::Storage final {
        struct Node final {
            UiElementHandle element;
            std::int32_t parent{-1};
            StyleInternal::WorkingStyle style;
            std::uint64_t contentHash{};
            std::uint64_t stateHash{};
            bool dirty{};
        };

        UiStyleResolverDescriptor descriptor;
        UiStyleResolverState lifecycle{UiStyleResolverState::Active};
        std::vector<Node> activeNodes;
        std::vector<Node> candidateNodes;
        std::vector<UiElementHandle> traversalScratch;
        std::vector<UiStyleInvalidation> invalidations;
        std::vector<std::shared_ptr<UiComputedStyleSnapshot::Storage>> slots;
        std::shared_ptr<UiComputedStyleSnapshot::Storage> current;
        UiStyleSourceRevisions sources;
        UiStylePublicationRevision publication;
        bool hasPublication{};
        std::size_t nextSlot{};

        explicit Storage(const UiStyleResolverDescriptor &source) : descriptor(source), publication(source.initialPublication) {
            activeNodes.reserve(source.elementCapacity);
            candidateNodes.reserve(source.elementCapacity);
            traversalScratch.resize(source.elementCapacity);
            invalidations.reserve(source.invalidationCapacity);
            slots.reserve(source.concurrentSnapshots);
            for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
                slots.push_back(std::make_shared<UiComputedStyleSnapshot::Storage>(source.elementCapacity, source.propertyCapacity));
        }

        ~Storage() {
            ReleaseCurrent();
        }

        void MarkAll() noexcept {
            for (auto &node : candidateNodes)
                node.dirty = true;
        }

        void MarkSubtree(const std::uint32_t root) noexcept {
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                auto ancestor = static_cast<std::int32_t>(index);
                while (ancestor >= 0 && static_cast<std::uint32_t>(ancestor) != root)
                    ancestor = candidateNodes[static_cast<std::uint32_t>(ancestor)].parent;
                if (ancestor >= 0)
                    candidateNodes[index].dirty = true;
            }
        }

        [[nodiscard]] std::uint32_t FindNode(const UiElementHandle element) const noexcept {
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index)
                if (candidateNodes[index].element == element)
                    return index;
            return std::numeric_limits<std::uint32_t>::max();
        }

        [[nodiscard]] Result<void> BuildTopology(const UiElementTree &tree) {
            const auto preorder = tree.Preorder(std::span<UiElementHandle>{traversalScratch.data(), tree.Size()});
            if (preorder.HasError())
                return Result<void>::Failure(preorder.ErrorValue());
            const auto count = static_cast<std::uint32_t>(preorder.Value());
            if (count == 0 || count > descriptor.elementCapacity)
                return StyleInternal::Failure(UiErrors::CapacityExceeded);
            candidateNodes.clear();
            candidateNodes.resize(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                candidateNodes[index].element = traversalScratch[index];
                const auto record = tree.Get(traversalScratch[index]);
                if (record.HasError())
                    return Result<void>::Failure(record.ErrorValue());
                if (!record.Value().parent.IsValid()) {
                    if (index != 0)
                        return StyleInternal::Failure(UiErrors::StyleSourceStale);
                } else {
                    const auto parent = std::find(traversalScratch.begin(), traversalScratch.begin() + index, record.Value().parent);
                    if (parent == traversalScratch.begin() + index)
                        return StyleInternal::Failure(UiErrors::StyleSourceStale);
                    candidateNodes[index].parent = static_cast<std::int32_t>(parent - traversalScratch.begin());
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyInvalidations(const UiRuntimeTreeRevision treeRevision) {
            for (const auto &invalidation : invalidations) {
                if (!invalidation.tree.IsValid() || invalidation.tree != treeRevision)
                    return StyleInternal::Failure(UiErrors::StyleSourceStale);
                switch (invalidation.kind) {
                    case UiStyleInvalidationKind::All:
                        MarkAll();
                        break;
                    case UiStyleInvalidationKind::Subtree: {
                        const auto index = FindNode(invalidation.element);
                        if (index == std::numeric_limits<std::uint32_t>::max())
                            return StyleInternal::Failure(UiErrors::StyleSourceStale);
                        MarkSubtree(index);
                        break;
                    }
                    case UiStyleInvalidationKind::Paint:
                    case UiStyleInvalidationKind::Measure: {
                        const auto index = FindNode(invalidation.element);
                        if (index == std::numeric_limits<std::uint32_t>::max())
                            return StyleInternal::Failure(UiErrors::StyleSourceStale);
                        candidateNodes[index].dirty = true;
                        break;
                    }
                    default:
                        return StyleInternal::Failure(UiErrors::StyleInvalid);
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> PrepareCandidate(const UiElementTree &tree, const UiStyleUpdateRequest &request) {
            const bool topologyChanged = !hasPublication || sources.tree != request.sources.tree;
            if (topologyChanged) {
                if (const auto result = BuildTopology(tree); result.HasError())
                    return result;
                MarkAll();
            } else {
                candidateNodes = activeNodes;
                for (auto &node : candidateNodes)
                    node.dirty = false;
            }
            const bool registryChanged = !hasPublication || sources.registry != request.sources.registry;
            const bool contentChanged = !hasPublication || sources.content != request.sources.content;
            const bool policyChanged = !hasPublication || sources.policy != request.sources.policy;
            if (registryChanged || contentChanged || policyChanged)
                MarkAll();
            for (std::uint32_t index = 0; index < request.elements.size(); ++index) {
                auto &node = candidateNodes[index];
                const auto &input = request.elements[index];
                const auto contentHash = StyleInternal::HashElementContent(input);
                const auto stateHash = StyleInternal::HashElementState(input.state);
                if (!topologyChanged && contentHash != activeNodes[index].contentHash)
                    MarkSubtree(index);
                else if (!topologyChanged && stateHash != activeNodes[index].stateHash)
                    node.dirty = true;
                node.contentHash = contentHash;
                node.stateHash = stateHash;
            }
            return ApplyInvalidations(request.sources.tree);
        }

        [[nodiscard]] Result<void> ResolveCandidate(const RuntimeStyleRegistry &registry, const UiStyleUpdateRequest &request) {
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                auto &node = candidateNodes[index];
                if (!node.dirty)
                    continue;
                const StyleInternal::WorkingStyle *parent =
                    node.parent < 0 ? nullptr : &candidateNodes[static_cast<std::uint32_t>(node.parent)].style;
                auto resolved = StyleInternal::ResolveElement(registry, request.elements[index], parent, descriptor.propertyCapacity);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                node.style = std::move(resolved).Value();
                node.dirty = false;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::shared_ptr<UiComputedStyleSnapshot::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1))
                    continue;
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
            return {};
        }

        void ReleaseCurrent() noexcept {
            if (current) {
                current->leases.fetch_sub(1);
                current.reset();
            }
        }

        [[nodiscard]] static std::uint32_t FindStyleRangeIndex(const UiComputedStyleSnapshot::Storage &slot,
                                                               const StyleInternal::WorkingStyle &working) noexcept {
            for (std::uint32_t index = 0; index < slot.styles.size(); ++index) {
                const auto &existing = slot.styles[index];
                if (existing.propertyCount != working.count)
                    continue;
                const auto existingBegin = slot.properties.begin() + existing.firstProperty;
                bool equal = true;
                for (std::uint32_t property = 0; property < working.count; ++property) {
                    if (existingBegin[property].property != working.properties[property].value.property ||
                        existingBegin[property].value != working.properties[property].value.value ||
                        existingBegin[property].provenance != working.properties[property].value.provenance) {
                        equal = false;
                        break;
                    }
                }
                if (equal)
                    return index;
            }
            return std::numeric_limits<std::uint32_t>::max();
        }

        [[nodiscard]] static Result<std::uint32_t> EnsureStyleRange(UiComputedStyleSnapshot::Storage &slot,
                                                                    const StyleInternal::WorkingStyle &working,
                                                                    const UiStyleResolverDescriptor &descriptor) {
            const auto existing = FindStyleRangeIndex(slot, working);
            if (existing != std::numeric_limits<std::uint32_t>::max())
                return Result<std::uint32_t>::Success(existing);
            if (slot.styles.size() >= descriptor.elementCapacity ||
                slot.properties.size() + working.count > static_cast<std::size_t>(descriptor.elementCapacity) * descriptor.propertyCapacity)
                return StyleInternal::Failure<std::uint32_t>(UiErrors::CapacityExceeded);
            const auto styleIndex = static_cast<std::uint32_t>(slot.styles.size());
            const auto styleId = UiComputedStyleId::Create(static_cast<std::uint64_t>(styleIndex + 1));
            if (styleId.HasError())
                return Result<std::uint32_t>::Failure(styleId.ErrorValue());
            const auto firstProperty = static_cast<std::uint32_t>(slot.properties.size());
            for (std::uint32_t property = 0; property < working.count; ++property)
                slot.properties.push_back(working.properties[property].value);
            slot.styles.push_back({styleId.Value(), firstProperty, working.count});
            return Result<std::uint32_t>::Success(styleIndex);
        }

        [[nodiscard]] Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>> Publish(const UiStyleUpdateRequest &request) {
            UiStylePublicationRevision nextPublication = descriptor.initialPublication;
            if (hasPublication) {
                const auto next = publication.Next();
                if (next.HasError())
                    return Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>>::Failure(next.ErrorValue());
                nextPublication = next.Value();
            }
            auto slot = TryAcquire();
            if (!slot)
                return StyleInternal::Failure<std::shared_ptr<UiComputedStyleSnapshot::Storage>>(UiErrors::StyleSnapshotStorageExhausted);
            slot->descriptor = {descriptor.instance, descriptor.canvas, descriptor.document, request.sources, nextPublication};
            slot->records.resize(candidateNodes.size());
            slot->lookup.resize(candidateNodes.size());
            slot->properties.clear();
            slot->styles.clear();
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                const auto range = EnsureStyleRange(*slot, candidateNodes[index].style, descriptor);
                if (range.HasError()) {
                    slot->leases.fetch_sub(1);
                    return Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>>::Failure(range.ErrorValue());
                }
                const auto &styleRange = slot->styles[range.Value()];
                slot->records[index] = {candidateNodes[index].element, styleRange.id, request.elements[index].state,
                                        styleRange.firstProperty, styleRange.propertyCount};
                slot->lookup[index] = index;
            }
            std::ranges::sort(slot->lookup, {}, [&records = slot->records](const std::uint32_t index) {
                return records[index].element;
            });
            ReleaseCurrent();
            current = slot;
            activeNodes.swap(candidateNodes);
            sources = request.sources;
            publication = nextPublication;
            hasPublication = true;
            invalidations.clear();
            slot->leases.fetch_add(1);
            return Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>>::Success(std::move(slot));
        }
    };

    /** @copydoc UiStyleResolver::Create */
    Result<UiStyleResolver> UiStyleResolver::Create(const UiStyleResolverDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return StyleInternal::Failure<UiStyleResolver>(UiErrors::StyleInvalid);
        try {
            return Result<UiStyleResolver>::Success(UiStyleResolver{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return StyleInternal::Failure<UiStyleResolver>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiStyleResolver::UiStyleResolver */
    UiStyleResolver::UiStyleResolver(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiStyleResolver::~UiStyleResolver */
    UiStyleResolver::~UiStyleResolver() {
        Shutdown();
    }

    /** @copydoc UiStyleResolver::UiStyleResolver */
    UiStyleResolver::UiStyleResolver(UiStyleResolver &&) noexcept = default;

    /** @copydoc UiStyleResolver::operator= */
    UiStyleResolver &UiStyleResolver::operator=(UiStyleResolver &&) noexcept = default;

    /** @copydoc UiStyleResolver::Invalidate */
    Result<void> UiStyleResolver::Invalidate(const UiStyleInvalidation &invalidation) {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        if (!invalidation.tree.IsValid())
            return StyleInternal::Failure(UiErrors::StyleInvalid);
        if (invalidation.kind != UiStyleInvalidationKind::All && !invalidation.element.IsValid())
            return StyleInternal::Failure(UiErrors::StyleInvalid);
        if (invalidation.kind == UiStyleInvalidationKind::All) {
            storage_->invalidations.clear();
            storage_->invalidations.push_back(invalidation);
            return Result<void>::Success();
        }
        const auto existing = std::find_if(storage_->invalidations.begin(), storage_->invalidations.end(),
                                           [&invalidation](const UiStyleInvalidation &queued) {
            return queued.tree == invalidation.tree && queued.element == invalidation.element;
        });
        if (existing != storage_->invalidations.end()) {
            const auto strength = [](const UiStyleInvalidationKind kind) {
                switch (kind) {
                    case UiStyleInvalidationKind::Paint:
                        return 0;
                    case UiStyleInvalidationKind::Measure:
                        return 1;
                    case UiStyleInvalidationKind::Subtree:
                        return 2;
                    case UiStyleInvalidationKind::All:
                        return 3;
                }
                return 0;
            };
            if (strength(invalidation.kind) > strength(existing->kind))
                existing->kind = invalidation.kind;
            return Result<void>::Success();
        }
        if (storage_->invalidations.size() == storage_->descriptor.invalidationCapacity)
            return StyleInternal::Failure(UiErrors::CapacityExceeded);
        storage_->invalidations.push_back(invalidation);
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::Update */
    Result<UiComputedStyleSnapshot> UiStyleResolver::Update(const UiElementTree &tree, const RuntimeStyleRegistry &registry,
                                                            const UiStyleUpdateRequest &request) {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleLifecycleUnavailable);
        if (registry.State() != RuntimeStyleRegistryState::Active)
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleLifecycleUnavailable);
        if (!request.sources.IsValid() || request.elements.empty() || request.elements.size() > storage_->descriptor.elementCapacity)
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleInvalid);
        if (request.sources.registry != registry.Generation())
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        if (!storage_->hasPublication && request.sources.registry != storage_->descriptor.initialRegistryGeneration)
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        if (tree.State() != UiElementTreeState::Active || tree.Instance() != storage_->descriptor.instance ||
            tree.Canvas() != storage_->descriptor.canvas || tree.SourceDocument() != storage_->descriptor.document ||
            tree.SourceDocumentRevision() != request.sources.document || tree.Revision() != request.sources.tree ||
            tree.Size() != request.elements.size() || tree.Size() > storage_->descriptor.elementCapacity)
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        for (std::size_t index = 0; index < request.elements.size(); ++index)
            if (const auto valid =
                    StyleInternal::ValidateElementInput(registry, request.elements[index], storage_->descriptor.propertyCapacity);
                valid.HasError())
                return Result<UiComputedStyleSnapshot>::Failure(valid.ErrorValue());

        const auto preorder = tree.Preorder(std::span<UiElementHandle>{storage_->traversalScratch.data(), tree.Size()});
        if (preorder.HasError())
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        for (std::size_t index = 0; index < request.elements.size(); ++index)
            if (request.elements[index].element != storage_->traversalScratch[index])
                return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);

        if (const auto prepared = storage_->PrepareCandidate(tree, request); prepared.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(prepared.ErrorValue());
        const bool sourcesChanged = !storage_->hasPublication || storage_->sources != request.sources;
        const bool anyDirty = std::ranges::any_of(storage_->candidateNodes, [](const Storage::Node &node) {
            return node.dirty;
        });
        if (!sourcesChanged && !anyDirty && storage_->invalidations.empty()) {
            storage_->current->leases.fetch_add(1);
            return Result<UiComputedStyleSnapshot>::Success(UiComputedStyleSnapshot{storage_->current});
        }
        if (const auto resolved = storage_->ResolveCandidate(registry, request); resolved.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(resolved.ErrorValue());
        auto published = storage_->Publish(request);
        if (published.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(published.ErrorValue());
        return Result<UiComputedStyleSnapshot>::Success(UiComputedStyleSnapshot{std::move(published).Value()});
    }

    /** @copydoc UiStyleResolver::BeginRetirement */
    Result<void> UiStyleResolver::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        storage_->lifecycle = UiStyleResolverState::Retiring;
        storage_->invalidations.clear();
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::Shutdown */
    void UiStyleResolver::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiStyleResolverState::Stopped)
            return;
        storage_->lifecycle = UiStyleResolverState::Stopped;
        storage_->invalidations.clear();
        storage_->activeNodes.clear();
        storage_->candidateNodes.clear();
        storage_->traversalScratch.clear();
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiStyleResolver::State */
    UiStyleResolverState UiStyleResolver::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiStyleResolverState::Stopped;
    }

    /** @copydoc UiStyleResolver::IsDrained */
    bool UiStyleResolver::IsDrained() const noexcept {
        if (!storage_)
            return true;
        return std::ranges::all_of(storage_->slots, [&storage = *storage_](const std::shared_ptr<UiComputedStyleSnapshot::Storage> &slot) {
            const auto leases = slot->leases.load();
            return leases == 0 || (slot == storage.current && leases == 1);
        });
    }
}  // namespace Horo::Runtime::Ui
