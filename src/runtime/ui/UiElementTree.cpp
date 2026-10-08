#include "Horo/Runtime/Ui/UiElementTree.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Checks exact shared owner identity without treating it as residency proof. */
        bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas) noexcept {
            return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership;
        }

        /** @brief Checks the closed safe-point vocabulary. */
        bool IsSafePoint(const UiStructuralCommitPoint point) noexcept {
            return point >= UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands &&
                   point <= UiStructuralCommitPoint::CommitDeferredLifecycleChanges;
        }
    }  // namespace

    /** @copydoc UiElementTreeLimits::IsValid */
    bool UiElementTreeLimits::IsValid() const noexcept {
        return elements > 0 && elements <= MaximumUiTreeElements && depth > 0 && depth <= MaximumUiTreeDepth && commands > 0 &&
               commands <= MaximumUiStructuralCommands;
    }

    /** @brief Immutable identity retained by all namespaces reserved by one actual owner allocator. */
    struct UiElementSlotRange::Authority final {};

    /** @copydoc UiElementSlotRange::UiElementSlotRange */
    UiElementSlotRange::UiElementSlotRange(const std::uint32_t firstSlot, const std::uint32_t slotCount,
                                           std::shared_ptr<const Authority> authority) noexcept
        : firstSlot_(firstSlot), slotCount_(slotCount), authority_(std::move(authority)) {}

    /** @copydoc UiElementSlotRange::FirstSlot */
    std::uint32_t UiElementSlotRange::FirstSlot() const noexcept {
        return firstSlot_;
    }

    /** @copydoc UiElementSlotRange::SlotCount */
    std::uint32_t UiElementSlotRange::SlotCount() const noexcept {
        return slotCount_;
    }

    /** @copydoc UiElementSlotAllocator::Create */
    Result<UiElementSlotAllocator> UiElementSlotAllocator::Create(const UiOwnershipGeneration ownership,
                                                                  const std::uint32_t previousIssuedSlot) {
        if (!ownership.IsValid())
            return Failure<UiElementSlotAllocator>(UiErrors::OwnershipGenerationInvalid);
        try {
            UiElementSlotAllocator allocator{ownership, std::make_shared<UiElementSlotRange::Authority>()};
            allocator.nextSlot_ = std::uint64_t(previousIssuedSlot) + 1;
            return Result<UiElementSlotAllocator>::Success(std::move(allocator));
        } catch (const std::bad_alloc &) {
            return Failure<UiElementSlotAllocator>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiElementSlotAllocator::UiElementSlotAllocator */
    UiElementSlotAllocator::UiElementSlotAllocator(const UiOwnershipGeneration ownership,
                                                   std::shared_ptr<const UiElementSlotRange::Authority> authority) noexcept
        : ownership_(ownership), authority_(std::move(authority)) {}

    /** @copydoc UiElementSlotAllocator::UiElementSlotAllocator */
    UiElementSlotAllocator::UiElementSlotAllocator(UiElementSlotAllocator &&other) noexcept
        : ownership_(std::exchange(other.ownership_, {})), nextSlot_(std::exchange(other.nextSlot_, 1)),
          authority_(std::move(other.authority_)) {}

    /** @copydoc UiElementSlotAllocator::Ownership */
    UiOwnershipGeneration UiElementSlotAllocator::Ownership() const noexcept {
        return ownership_;
    }

    /** @copydoc UiElementSlotAllocator::Reserve */
    Result<UiElementSlotRange> UiElementSlotAllocator::Reserve(const std::uint32_t slotCount) {
        if (!ownership_.IsValid())
            return Failure<UiElementSlotRange>(UiErrors::OwnershipGenerationInvalid);
        if (slotCount == 0)
            return Failure<UiElementSlotRange>(UiErrors::CapacityExceeded);
        if (constexpr auto MaximumSlot = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
            nextSlot_ > MaximumSlot - slotCount + 1)
            return Failure<UiElementSlotRange>(UiErrors::GenerationExhausted);
        const auto firstSlot = static_cast<std::uint32_t>(nextSlot_);
        nextSlot_ += slotCount;
        return Result<UiElementSlotRange>::Success(UiElementSlotRange{firstSlot, slotCount, authority_});
    }

    /** @copydoc UiStructuralCommandBuffer::Create */
    Result<UiStructuralCommandBuffer> UiStructuralCommandBuffer::Create(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas,
                                                                        const UiDocumentRevision documentRevision,
                                                                        const UiRuntimeTreeRevision treeRevision,
                                                                        const std::uint32_t capacity) {
        if (!SameOwner(instance, canvas))
            return Failure<UiStructuralCommandBuffer>(UiErrors::HandleMalformed);
        if (!documentRevision.IsValid() || !treeRevision.IsValid())
            return Failure<UiStructuralCommandBuffer>(UiErrors::RevisionInvalid);
        if (capacity == 0 || capacity > MaximumUiStructuralCommands)
            return Failure<UiStructuralCommandBuffer>(UiErrors::CapacityExceeded);
        try {
            return Result<UiStructuralCommandBuffer>::Success(
                UiStructuralCommandBuffer{instance, canvas, documentRevision, treeRevision, capacity});
        } catch (const std::bad_alloc &) {
            return Failure<UiStructuralCommandBuffer>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiStructuralCommandBuffer::UiStructuralCommandBuffer */
    UiStructuralCommandBuffer::UiStructuralCommandBuffer(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas,
                                                         const UiDocumentRevision documentRevision,
                                                         const UiRuntimeTreeRevision treeRevision, const std::uint32_t capacity)
        : instance_(instance), canvas_(canvas), documentRevision_(documentRevision), treeRevision_(treeRevision), capacity_(capacity) {
        commands_.reserve(capacity);
    }

    /** @copydoc UiStructuralCommandBuffer::Add */
    Result<void> UiStructuralCommandBuffer::Add(UiStructuralCommand command) {
        if (commands_.size() == capacity_)
            return Failure(UiErrors::CapacityExceeded);
        commands_.push_back(std::move(command));
        return Result<void>::Success();
    }

    /** @copydoc UiStructuralCommandBuffer::Instance */
    RuntimeUiInstanceId UiStructuralCommandBuffer::Instance() const noexcept {
        return instance_;
    }

    /** @copydoc UiStructuralCommandBuffer::Canvas */
    UiCanvasInstanceId UiStructuralCommandBuffer::Canvas() const noexcept {
        return canvas_;
    }

    /** @copydoc UiStructuralCommandBuffer::DocumentRevision */
    UiDocumentRevision UiStructuralCommandBuffer::DocumentRevision() const noexcept {
        return documentRevision_;
    }

    /** @copydoc UiStructuralCommandBuffer::TreeRevision */
    UiRuntimeTreeRevision UiStructuralCommandBuffer::TreeRevision() const noexcept {
        return treeRevision_;
    }

    /** @copydoc UiStructuralCommandBuffer::Commands */
    std::span<const UiStructuralCommand> UiStructuralCommandBuffer::Commands() const noexcept {
        return commands_;
    }

    /** @brief Private flat slot tree; children and cached preorder own deterministic stable order. */
    struct UiElementTree::Storage final {
        struct Node final {
            UiElementId id;
            std::uint32_t generation{};
            std::uint32_t parentSlot{};
            bool occupied{};
            bool retired{};
            std::vector<std::uint32_t> children;
        };

        UiElementTreeDescriptor descriptor;
        UiElementSlotRange elementSlots;
        UiElementTreeState lifecycle{UiElementTreeState::Active};
        std::vector<Node> nodes;
        std::vector<std::uint32_t> stableIndex;
        std::vector<std::uint32_t> preorder;
        std::uint32_t rootSlot{};
        std::uint32_t size{};

        Storage(const UiElementTreeDescriptor &source, const UiElementSlotRange slots)
            : descriptor(source), elementSlots(slots), nodes(source.limits.elements) {
            stableIndex.reserve(source.limits.elements);
            preorder.reserve(source.limits.elements);
        }

        /** @brief Returns the exact current handle for an occupied zero-based slot. */
        UiElementHandle Handle(const std::uint32_t slot) const noexcept {
            return {descriptor.instance.ownership, elementSlots.FirstSlot() + slot, nodes[slot].generation};
        }

        /** @brief Resolves stable identity through the sorted current index. */
        std::uint32_t FindSlot(const UiElementId id) const noexcept {
            const auto found = std::ranges::lower_bound(stableIndex, id, {}, [this](const std::uint32_t slot) {
                return nodes[slot].id;
            });
            return found != stableIndex.end() && nodes[*found].id == id ? *found : std::numeric_limits<std::uint32_t>::max();
        }

        /** @brief Validates a transient handle against this exact owner and resident slot. */
        Result<std::uint32_t> Slot(const UiElementHandle handle) const {
            if (const auto owner = ValidateUiHandleOwner(handle, descriptor.instance.ownership); owner.HasError())
                return Result<std::uint32_t>::Failure(owner.ErrorValue());
            if (handle.slot < elementSlots.FirstSlot() || handle.slot - elementSlots.FirstSlot() >= elementSlots.SlotCount())
                return Failure<std::uint32_t>(UiErrors::HandleStale);
            const auto slot = handle.slot - elementSlots.FirstSlot();
            const auto &node = nodes[slot];
            if (const auto resident = ValidateUiHandleResidency(handle, descriptor.instance.ownership, elementSlots.FirstSlot() + slot,
                                                                node.generation, node.occupied);
                resident.HasError())
                return Result<std::uint32_t>::Failure(resident.ErrorValue());
            return Result<std::uint32_t>::Success(slot);
        }

        /** @brief Rebuilds stable lookup and rejects duplicate identities. */
        Result<void> RebuildIndex() {
            stableIndex.clear();
            for (std::uint32_t slot = 0; slot < nodes.size(); ++slot)
                if (nodes[slot].occupied)
                    stableIndex.push_back(slot);
            std::ranges::sort(stableIndex, {}, [this](const std::uint32_t slot) {
                return nodes[slot].id;
            });
            if (std::ranges::adjacent_find(stableIndex, [this](const std::uint32_t left, const std::uint32_t right) {
                return nodes[left].id == nodes[right].id;
            }) != stableIndex.end())
                return Failure(UiErrors::ElementTreeIdentityConflict);
            return Result<void>::Success();
        }

        /** @brief Checks every parent chain is connected, acyclic and inside the depth bound. */
        Result<void> ValidateDepth() const {
            for (const auto slot : stableIndex) {
                auto cursor = slot;
                std::uint32_t depth{};
                while (true) {
                    if (++depth > descriptor.limits.depth)
                        return Failure(UiErrors::ElementTreeInvalid);
                    if (cursor == rootSlot)
                        break;
                    const auto parent = nodes[cursor].parentSlot;
                    if (parent == 0 || parent > nodes.size() || !nodes[parent - 1].occupied)
                        return Failure(UiErrors::ElementTreeInvalid);
                    cursor = parent - 1;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Rebuilds allocation-free traversal order after candidate mutation. */
        Result<void> RebuildPreorder() {
            preorder.clear();
            std::vector<std::uint32_t> stack;
            stack.reserve(size);
            stack.push_back(rootSlot);
            while (!stack.empty()) {
                const auto slot = stack.back();
                stack.pop_back();
                preorder.push_back(slot);
                const auto &children = nodes[slot].children;
                for (auto child = children.rbegin(); child != children.rend(); ++child)
                    stack.push_back(*child);
            }
            if (preorder.size() != size)
                return Failure(UiErrors::ElementTreeInvalid);
            return ValidateDepth();
        }

        /** @brief Rebuilds every derived candidate projection. */
        Result<void> RebuildDerived() {
            if (const auto index = RebuildIndex(); index.HasError())
                return index;
            return RebuildPreorder();
        }

        /** @brief Initializes occupied slots without publishing partially validated topology. */
        Result<void> InitializeNodes(const std::span<const UiElementDescriptor> elements) {
            for (std::uint32_t slot = 0; slot < elements.size(); ++slot) {
                if (!elements[slot].id.IsValid())
                    return Failure(UiErrors::ElementTreeInvalid);
                nodes[slot].id = elements[slot].id;
                nodes[slot].generation = 1;
                nodes[slot].occupied = true;
            }
            size = static_cast<std::uint32_t>(elements.size());
            return RebuildIndex();
        }

        /** @brief Resolves stable parent identities and preserves authored sibling order. */
        Result<void> LinkNodes(const std::span<const UiElementDescriptor> elements) {
            std::uint32_t roots{};
            for (std::uint32_t slot = 0; slot < elements.size(); ++slot) {
                if (!elements[slot].parent.IsValid()) {
                    rootSlot = slot;
                    ++roots;
                    continue;
                }
                const auto parent = FindSlot(elements[slot].parent);
                if (parent == std::numeric_limits<std::uint32_t>::max())
                    return Failure(UiErrors::ElementTreeInvalid);
                nodes[slot].parentSlot = parent + 1;
                nodes[parent].children.push_back(slot);
            }
            return roots == 1 ? Result<void>::Success() : Failure(UiErrors::ElementTreeInvalid);
        }

        /** @brief Builds and validates a complete initial retained-tree candidate. */
        Result<void> Populate(const std::span<const UiElementDescriptor> elements) {
            if (const auto initialized = InitializeNodes(elements); initialized.HasError())
                return initialized;
            if (const auto linked = LinkNodes(elements); linked.HasError())
                return linked;
            return RebuildPreorder();
        }

        /** @brief Finds the first reusable slot without ever wrapping its generation. */
        Result<std::uint32_t> AcquireSlot() {
            for (std::uint32_t slot = 0; slot < nodes.size(); ++slot) {
                auto &node = nodes[slot];
                if (!node.occupied && !node.retired) {
                    if (node.generation == 0)
                        node.generation = 1;
                    return Result<std::uint32_t>::Success(slot);
                }
            }
            return Failure<std::uint32_t>(UiErrors::CapacityExceeded);
        }

        /** @brief Applies one insertion to the detached candidate. */
        Result<std::uint32_t> Apply(const UiInsertElementCommand &command) {
            if (!command.id.IsValid() || FindSlot(command.id) != std::numeric_limits<std::uint32_t>::max())
                return Failure<std::uint32_t>(UiErrors::ElementTreeIdentityConflict);
            const auto parent = Slot(command.parent);
            if (parent.HasError())
                return Result<std::uint32_t>::Failure(parent.ErrorValue());
            auto &children = nodes[parent.Value()].children;
            if (command.childIndex > children.size())
                return Failure<std::uint32_t>(UiErrors::StructuralCommandInvalid);
            if (size == descriptor.limits.elements)
                return Failure<std::uint32_t>(UiErrors::CapacityExceeded);
            auto acquired = AcquireSlot();
            if (acquired.HasError())
                return acquired;
            const auto slot = acquired.Value();
            auto &node = nodes[slot];
            node.id = command.id;
            node.parentSlot = parent.Value() + 1;
            node.occupied = true;
            node.children.clear();
            children.insert(children.begin() + command.childIndex, slot);
            ++size;
            return Result<std::uint32_t>::Success(0);
        }

        /** @brief Releases one detached subtree and invalidates every exact old handle. */
        Result<std::uint32_t> Apply(const UiRemoveElementCommand &command) {
            const auto removedRoot = Slot(command.element);
            if (removedRoot.HasError())
                return Result<std::uint32_t>::Failure(removedRoot.ErrorValue());
            if (removedRoot.Value() == rootSlot)
                return Failure<std::uint32_t>(UiErrors::StructuralCommandConflict);
            auto &siblings = nodes[nodes[removedRoot.Value()].parentSlot - 1].children;
            std::erase(siblings, removedRoot.Value());
            std::vector pending{removedRoot.Value()};
            std::uint32_t removed{};
            while (!pending.empty()) {
                const auto slot = pending.back();
                pending.pop_back();
                auto &node = nodes[slot];
                pending.insert(pending.end(), node.children.begin(), node.children.end());
                node.id = {};
                node.parentSlot = 0;
                node.occupied = false;
                node.children.clear();
                if (node.generation == std::numeric_limits<std::uint32_t>::max())
                    node.retired = true;
                else
                    ++node.generation;
                ++removed;
            }
            size -= removed;
            return Result<std::uint32_t>::Success(removed);
        }

        /** @brief Checks whether a destination lies inside the moving subtree. */
        bool IsDescendant(const std::uint32_t element, std::uint32_t candidate) const noexcept {
            while (candidate != rootSlot) {
                if (candidate == element)
                    return true;
                candidate = nodes[candidate].parentSlot - 1;
            }
            return candidate == element;
        }

        /** @brief Applies one move/reorder to the detached candidate. */
        Result<std::uint32_t> Apply(const UiReparentElementCommand &command) {
            const auto element = Slot(command.element);
            if (element.HasError())
                return Result<std::uint32_t>::Failure(element.ErrorValue());
            const auto parent = Slot(command.parent);
            if (parent.HasError())
                return Result<std::uint32_t>::Failure(parent.ErrorValue());
            if (element.Value() == rootSlot || IsDescendant(element.Value(), parent.Value()))
                return Failure<std::uint32_t>(UiErrors::StructuralCommandConflict);
            const auto oldParent = nodes[element.Value()].parentSlot - 1;
            if (const auto destinationSize = nodes[parent.Value()].children.size() - (oldParent == parent.Value() ? 1U : 0U);
                command.childIndex > destinationSize)
                return Failure<std::uint32_t>(UiErrors::StructuralCommandInvalid);
            std::erase(nodes[oldParent].children, element.Value());
            auto &children = nodes[parent.Value()].children;
            children.insert(children.begin() + command.childIndex, element.Value());
            nodes[element.Value()].parentSlot = parent.Value() + 1;
            return Result<std::uint32_t>::Success(0);
        }

        /** @brief Dispatches one closed command alternative. */
        Result<std::uint32_t> Apply(const UiStructuralCommand &command) {
            return std::visit([this]<typename Command>(const Command &value) {
                return Apply(value);
            }, command);
        }
    };

    namespace {
        /** @brief Validates immutable creation inputs before allocating a candidate tree. */
        Result<void> ValidateTreeCreation(const UiElementTreeDescriptor &descriptor, const std::span<const UiElementDescriptor> elements) {
            if (!SameOwner(descriptor.instance, descriptor.canvas) || !descriptor.document.IsValid())
                return Failure(UiErrors::ElementTreeInvalid);
            if (!descriptor.documentRevision.IsValid() || !descriptor.treeRevision.IsValid())
                return Failure(UiErrors::RevisionInvalid);
            if (!descriptor.limits.IsValid() || elements.empty() || elements.size() > descriptor.limits.elements)
                return Failure(UiErrors::CapacityExceeded);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiElementTree::Create */
    Result<UiElementTree> UiElementTree::Create(UiElementSlotAllocator &slotAllocator, const UiElementTreeDescriptor &descriptor,
                                                const std::span<const UiElementDescriptor> elements) {
        if (const auto validated = ValidateTreeCreation(descriptor, elements); validated.HasError())
            return Result<UiElementTree>::Failure(validated.ErrorValue());
        if (slotAllocator.Ownership() != descriptor.instance.ownership)
            return Failure<UiElementTree>(UiErrors::HandleOwnerMismatch);
        auto elementSlots = slotAllocator.Reserve(descriptor.limits.elements);
        if (elementSlots.HasError())
            return Result<UiElementTree>::Failure(elementSlots.ErrorValue());
        try {
            auto state = std::make_unique<Storage>(descriptor, elementSlots.Value());
            if (const auto populated = state->Populate(elements); populated.HasError())
                return Result<UiElementTree>::Failure(populated.ErrorValue());
            return Result<UiElementTree>::Success(UiElementTree{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<UiElementTree>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiElementTree::UiElementTree */
    UiElementTree::UiElementTree(std::unique_ptr<Storage> state) noexcept : state_(std::move(state)) {}

    /** @copydoc UiElementTree::~UiElementTree */
    UiElementTree::~UiElementTree() = default;
    /** @copydoc UiElementTree::UiElementTree */
    UiElementTree::UiElementTree(UiElementTree &&) noexcept = default;
    /** @copydoc UiElementTree::operator= */
    UiElementTree &UiElementTree::operator=(UiElementTree &&) noexcept = default;

    /** @copydoc UiElementTree::IssuerPin */
    std::shared_ptr<const void> UiElementTree::IssuerPin() const noexcept {
        return state_ ? state_->elementSlots.authority_ : nullptr;
    }

    /** @copydoc UiElementTree::WasIssuedBy */
    bool UiElementTree::WasIssuedBy(const UiElementSlotAllocator &allocator) const noexcept {
        return state_ && allocator.ownership_.IsValid() && state_->descriptor.instance.ownership == allocator.ownership_ &&
               state_->elementSlots.authority_ == allocator.authority_;
    }

    /** @copydoc UiElementTree::State */
    UiElementTreeState UiElementTree::State() const noexcept {
        return state_ ? state_->lifecycle : UiElementTreeState::Stopped;
    }

    /** @copydoc UiElementTree::Instance */
    RuntimeUiInstanceId UiElementTree::Instance() const noexcept {
        return state_ ? state_->descriptor.instance : RuntimeUiInstanceId{};
    }

    /** @copydoc UiElementTree::Canvas */
    UiCanvasInstanceId UiElementTree::Canvas() const noexcept {
        return state_ ? state_->descriptor.canvas : UiCanvasInstanceId{};
    }

    /** @copydoc UiElementTree::SourceDocument */
    UiDocumentId UiElementTree::SourceDocument() const noexcept {
        return state_ ? state_->descriptor.document : UiDocumentId{};
    }

    /** @copydoc UiElementTree::SourceDocumentRevision */
    UiDocumentRevision UiElementTree::SourceDocumentRevision() const noexcept {
        return state_ ? state_->descriptor.documentRevision : UiDocumentRevision{};
    }

    /** @copydoc UiElementTree::Revision */
    UiRuntimeTreeRevision UiElementTree::Revision() const noexcept {
        return state_ ? state_->descriptor.treeRevision : UiRuntimeTreeRevision{};
    }

    /** @copydoc UiElementTree::Size */
    std::uint32_t UiElementTree::Size() const noexcept {
        return state_ ? state_->size : 0;
    }

    /** @copydoc UiElementTree::ReservedSlots */
    Result<UiElementSlotRange> UiElementTree::ReservedSlots() const {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return Failure<UiElementSlotRange>(UiErrors::ElementTreeLifecycleUnavailable);
        return Result<UiElementSlotRange>::Success(state_->elementSlots);
    }

    /** @copydoc UiElementTree::Root */
    Result<UiElementRecord> UiElementTree::Root() const {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return Failure<UiElementRecord>(UiErrors::ElementTreeLifecycleUnavailable);
        const auto slot = state_->rootSlot;
        return Result<UiElementRecord>::Success({state_->Handle(slot), state_->nodes[slot].id, {}});
    }

    /** @copydoc UiElementTree::Find */
    Result<UiElementHandle> UiElementTree::Find(const UiElementId id) const {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return Failure<UiElementHandle>(UiErrors::ElementTreeLifecycleUnavailable);
        if (!id.IsValid())
            return Failure<UiElementHandle>(UiErrors::IdentityInvalid);
        const auto slot = state_->FindSlot(id);
        if (slot == std::numeric_limits<std::uint32_t>::max())
            return Failure<UiElementHandle>(UiErrors::HandleStale);
        return Result<UiElementHandle>::Success(state_->Handle(slot));
    }

    /** @copydoc UiElementTree::Get */
    Result<UiElementRecord> UiElementTree::Get(const UiElementHandle handle) const {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return Failure<UiElementRecord>(UiErrors::ElementTreeLifecycleUnavailable);
        const auto slot = state_->Slot(handle);
        if (slot.HasError())
            return Result<UiElementRecord>::Failure(slot.ErrorValue());
        const auto &node = state_->nodes[slot.Value()];
        const auto parent = node.parentSlot == 0 ? UiElementHandle{} : state_->Handle(node.parentSlot - 1);
        return Result<UiElementRecord>::Success({handle, node.id, parent});
    }

    /** @copydoc UiElementTree::Children */
    Result<std::size_t> UiElementTree::Children(const UiElementHandle parent, const std::span<UiElementHandle> output) const {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return Failure<std::size_t>(UiErrors::ElementTreeLifecycleUnavailable);
        const auto slot = state_->Slot(parent);
        if (slot.HasError())
            return Result<std::size_t>::Failure(slot.ErrorValue());
        const auto &children = state_->nodes[slot.Value()].children;
        if (output.size() < children.size())
            return Failure<std::size_t>(UiErrors::CapacityExceeded);
        for (std::size_t index = 0; index < children.size(); ++index)
            output[index] = state_->Handle(children[index]);
        return Result<std::size_t>::Success(children.size());
    }

    /** @copydoc UiElementTree::Preorder */
    Result<std::size_t> UiElementTree::Preorder(const std::span<UiElementHandle> output) const {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return Failure<std::size_t>(UiErrors::ElementTreeLifecycleUnavailable);
        if (output.size() < state_->preorder.size())
            return Failure<std::size_t>(UiErrors::CapacityExceeded);
        for (std::size_t index = 0; index < state_->preorder.size(); ++index)
            output[index] = state_->Handle(state_->preorder[index]);
        return Result<std::size_t>::Success(state_->preorder.size());
    }

    /** @copydoc UiElementTree::CommitDeferred */
    Result<UiStructuralCommitResult> UiElementTree::CommitDeferred(const UiStructuralCommandBuffer &buffer,
                                                                   const UiStructuralCommitPoint point) {
        if (const auto validated = ValidateCommitRequest(buffer, point); validated.HasError())
            return Result<UiStructuralCommitResult>::Failure(validated.ErrorValue());
        if (buffer.Commands().empty())
            return Result<UiStructuralCommitResult>::Success({state_->descriptor.treeRevision, 0, 0});
        auto nextRevision = state_->descriptor.treeRevision.Next();
        if (nextRevision.HasError())
            return Result<UiStructuralCommitResult>::Failure(nextRevision.ErrorValue());
        return PublishCandidate(buffer, nextRevision.Value());
    }

    /** @copydoc UiElementTree::ValidateCommitRequest */
    Result<void> UiElementTree::ValidateCommitRequest(const UiStructuralCommandBuffer &buffer, const UiStructuralCommitPoint point) const {
        if (!state_ || state_->lifecycle != UiElementTreeState::Active)
            return Failure(UiErrors::ElementTreeLifecycleUnavailable);
        if (!IsSafePoint(point))
            return Failure(UiErrors::StructuralCommandInvalid);
        if (buffer.Instance() != state_->descriptor.instance || buffer.Canvas() != state_->descriptor.canvas)
            return Failure(UiErrors::HandleOwnerMismatch);
        if (const auto document = ValidateExpectedUiRevision(buffer.DocumentRevision(), state_->descriptor.documentRevision);
            document.HasError())
            return document;
        if (const auto tree = ValidateExpectedUiRevision(buffer.TreeRevision(), state_->descriptor.treeRevision); tree.HasError())
            return tree;
        return buffer.Commands().size() <= state_->descriptor.limits.commands ? Result<void>::Success()
                                                                              : Failure(UiErrors::CapacityExceeded);
    }

    /** @copydoc UiElementTree::PublishCandidate */
    Result<UiStructuralCommitResult> UiElementTree::PublishCandidate(const UiStructuralCommandBuffer &buffer,
                                                                     const UiRuntimeTreeRevision nextRevision) {
        try {
            auto candidate = std::make_unique<Storage>(*state_);
            std::uint32_t removed{};
            for (const auto &command : buffer.Commands()) {
                const auto applied = candidate->Apply(command);
                if (applied.HasError())
                    return Result<UiStructuralCommitResult>::Failure(applied.ErrorValue());
                removed += applied.Value();
                if (const auto derived = candidate->RebuildDerived(); derived.HasError())
                    return Result<UiStructuralCommitResult>::Failure(derived.ErrorValue());
            }
            candidate->descriptor.treeRevision = nextRevision;
            const auto result = UiStructuralCommitResult{nextRevision, static_cast<std::uint32_t>(buffer.Commands().size()), removed};
            state_.swap(candidate);
            return Result<UiStructuralCommitResult>::Success(result);
        } catch (const std::bad_alloc &) {
            return Failure<UiStructuralCommitResult>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiElementTree::BeginRetirement */
    Result<void> UiElementTree::BeginRetirement() {
        if (!state_ || state_->lifecycle != UiElementTreeState::Active)
            return Failure(UiErrors::ElementTreeLifecycleUnavailable);
        state_->lifecycle = UiElementTreeState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiElementTree::Shutdown */
    void UiElementTree::Shutdown() noexcept {
        if (!state_ || state_->lifecycle == UiElementTreeState::Stopped)
            return;
        state_->nodes.clear();
        state_->stableIndex.clear();
        state_->preorder.clear();
        state_->rootSlot = 0;
        state_->size = 0;
        state_->lifecycle = UiElementTreeState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
