#include "UiFocusGraphInternal.h"

namespace Horo::Runtime::Ui {
    using FocusGraphDetail::Failure;
    using FocusGraphDetail::InvalidOwner;
    using FocusGraphDetail::SameStaticScope;

    /** @copydoc UiFocusGraph::Create */
    Result<UiFocusGraph> UiFocusGraph::Create(const UiFocusGraphDescriptor &descriptor,
                                              const std::span<const UiFocusNodeDescriptor> nodes) {
        if (!descriptor.IsValid())
            return Failure<UiFocusGraph>(UiErrors::FocusInvalid);
        try {
            auto storage = std::make_unique<Storage>(descriptor);
            auto candidate = storage->BuildCandidate(descriptor, nodes);
            if (candidate.HasError())
                return Result<UiFocusGraph>::Failure(candidate.ErrorValue());
            storage->nodes = std::move(candidate).Value();
            storage->focusedIndex = storage->ResolveInitial();
            return Result<UiFocusGraph>::Success(UiFocusGraph{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiFocusGraph>(UiErrors::FocusCapacityExceeded);
        }
    }

    /** @copydoc UiFocusGraph::UiFocusGraph */
    UiFocusGraph::UiFocusGraph(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiFocusGraph::~UiFocusGraph */
    UiFocusGraph::~UiFocusGraph() {
        Shutdown();
    }

    /** @copydoc UiFocusGraph::UiFocusGraph */
    UiFocusGraph::UiFocusGraph(UiFocusGraph &&) noexcept = default;

    /** @copydoc UiFocusGraph::operator= */
    UiFocusGraph &UiFocusGraph::operator=(UiFocusGraph &&) noexcept = default;

    /** @copydoc UiFocusGraph::Owner */
    const UiFocusOwnerContext &UiFocusGraph::Owner() const noexcept {
        return storage_ ? storage_->descriptor.owner : InvalidOwner();
    }

    /** @copydoc UiFocusGraph::State */
    UiFocusGraphState UiFocusGraph::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiFocusGraphState::Stopped;
    }

    /** @copydoc UiFocusGraph::Snapshot */
    Result<UiFocusSnapshot> UiFocusGraph::Snapshot() const {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusSnapshot>(UiErrors::FocusLifecycleUnavailable);
        const std::optional<UiFocusModalId> modal =
            storage_->modalDepth == 0 ? std::nullopt
                                      : std::optional<UiFocusModalId>{{storage_->descriptor.owner.instance.ownership, storage_->modalDepth,
                                                                       storage_->modalSlots[storage_->modalDepth - 1].generation}};
        return Result<UiFocusSnapshot>::Success(
            UiFocusSnapshot{storage_->descriptor.owner, storage_->CurrentTarget(), modal, storage_->modalDepth});
    }

    /** @copydoc UiFocusGraph::Find */
    Result<UiElementHandle> UiFocusGraph::Find(const UiElementId id) const {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiElementHandle>(UiErrors::FocusLifecycleUnavailable);
        const std::size_t index = storage_->FindNode(id);
        if (index == Storage::InvalidIndex)
            return Failure<UiElementHandle>(UiErrors::FocusTargetUnavailable);
        return Result<UiElementHandle>::Success(storage_->nodes[index].descriptor.element);
    }

    /** @copydoc UiFocusGraph::CurrentFocus */
    Result<std::optional<UiFocusTarget>> UiFocusGraph::CurrentFocus() const {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<std::optional<UiFocusTarget>>(UiErrors::FocusLifecycleUnavailable);
        return Result<std::optional<UiFocusTarget>>::Success(storage_->CurrentTarget());
    }

    /** @copydoc UiFocusGraph::Reload */
    Result<UiFocusChange> UiFocusGraph::Reload(const UiFocusGraphDescriptor &descriptor,
                                               const std::span<const UiFocusNodeDescriptor> nodes) {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        if (!descriptor.IsValid())
            return Failure<UiFocusChange>(UiErrors::FocusInvalid);
        if (!SameStaticScope(descriptor.owner, storage_->descriptor.owner))
            return Failure<UiFocusChange>(UiErrors::FocusScopeMismatch);
        if (descriptor.owner.documentRevision.Compare(storage_->descriptor.owner.documentRevision) == UiRevisionRelation::Older ||
            descriptor.owner.treeRevision.Compare(storage_->descriptor.owner.treeRevision) == UiRevisionRelation::Older ||
            descriptor.owner.interaction.Compare(storage_->descriptor.owner.interaction) != UiRevisionRelation::Newer)
            return Failure<UiFocusChange>(UiErrors::FocusSourceStale);
        if (descriptor.nodeCapacity > storage_->descriptor.nodeCapacity || descriptor.modalCapacity > storage_->descriptor.modalCapacity ||
            descriptor.restorationCapacity > storage_->descriptor.restorationCapacity)
            return Failure<UiFocusChange>(UiErrors::FocusCapacityExceeded);

        try {
            auto candidate = storage_->BuildCandidate(descriptor, nodes);
            if (candidate.HasError())
                return Result<UiFocusChange>::Failure(candidate.ErrorValue());

            const auto previous = storage_->CurrentTarget();
            Storage::RestorationEntry oldPath;
            storage_->CollectPath(storage_->focusedIndex, oldPath);
            const std::uint32_t oldModalDepth = storage_->modalDepth;

            storage_->nodes = std::move(candidate).Value();
            storage_->descriptor = descriptor;

            std::uint32_t retainedModalDepth = oldModalDepth;
            for (std::uint32_t index = 0; index < oldModalDepth; ++index) {
                Storage::ModalSlot &modal = storage_->modalSlots[index];
                const std::size_t rootIndex = storage_->FindNode(modal.root);
                if (rootIndex == Storage::InvalidIndex) {
                    retainedModalDepth = index;
                    break;
                }
                modal.rootHandle = storage_->nodes[rootIndex].descriptor.element;
            }
            for (std::uint32_t index = retainedModalDepth; index < oldModalDepth; ++index) {
                Storage::ModalSlot &modal = storage_->modalSlots[index];
                modal.generation = modal.generation == std::numeric_limits<std::uint32_t>::max() ? 0 : modal.generation + 1;
            }
            storage_->modalDepth = retainedModalDepth;
            storage_->restorationDepth = std::min(storage_->restorationDepth, retainedModalDepth);
            storage_->focusedIndex = storage_->ResolveReload(oldPath);
            return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::Reload));
        } catch (const std::bad_alloc &) {
            return Failure<UiFocusChange>(UiErrors::FocusCapacityExceeded);
        }
    }

    /** @copydoc UiFocusGraph::SetFocus */
    Result<UiFocusChange> UiFocusGraph::SetFocus(const UiElementHandle element) {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        if (!element.IsValid() || element.ownership != storage_->descriptor.owner.instance.ownership)
            return Failure<UiFocusChange>(UiErrors::FocusSourceStale);
        const std::size_t index = storage_->FindNode(element);
        if (index == Storage::InvalidIndex)
            return Failure<UiFocusChange>(UiErrors::FocusTargetUnavailable);
        if (!storage_->IsWithinModal(index))
            return Failure<UiFocusChange>(UiErrors::FocusModalBoundaryViolation);
        if (!storage_->IsAllowed(index))
            return Failure<UiFocusChange>(UiErrors::FocusTargetUnavailable);

        const auto previous = storage_->CurrentTarget();
        storage_->focusedIndex = index;
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::Explicit));
    }

    /** @copydoc UiFocusGraph::FocusDefault */
    Result<UiFocusChange> UiFocusGraph::FocusDefault() {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        const auto previous = storage_->CurrentTarget();
        storage_->focusedIndex = storage_->ResolveInitial();
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::Default));
    }

    /** @copydoc UiFocusGraph::Move */
    Result<UiFocusChange> UiFocusGraph::Move(const UiNavigationDirection direction) {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        if (!FocusGraphDetail::DirectionIndex(direction).has_value())
            return Failure<UiFocusChange>(UiErrors::FocusInvalid);
        if (!storage_->focusedIndex.has_value())
            return FocusDefault();

        const std::size_t currentIndex = *storage_->focusedIndex;
        if (currentIndex >= storage_->nodes.size() || !storage_->IsAllowed(currentIndex)) {
            Storage::RestorationEntry recovery;
            storage_->CollectPath(storage_->focusedIndex, recovery);
            recovery.focused = {};
            const auto previous = storage_->CurrentTarget();
            storage_->focusedIndex = storage_->ResolveRestoration(recovery);
            return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::InvalidTarget));
        }

        const UiElementId targetId = storage_->nodes[currentIndex].descriptor.links.Target(direction);
        if (!targetId.IsValid())
            return Result<UiFocusChange>::Success(storage_->NoTarget(UiFocusChangeReason::InvalidTarget));

        const auto target = storage_->ResolveAllowed(targetId);
        if (target.has_value()) {
            const auto previous = storage_->CurrentTarget();
            storage_->focusedIndex = target;
            return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::Link));
        }

        const std::size_t targetIndex = storage_->FindNode(targetId);
        if (targetIndex != Storage::InvalidIndex && !storage_->IsWithinModal(targetIndex))
            return Result<UiFocusChange>::Success(storage_->NoTarget(UiFocusChangeReason::InvalidTarget));

        Storage::RestorationEntry recovery;
        storage_->CollectPath(storage_->focusedIndex, recovery);
        recovery.focused = {};
        const auto previous = storage_->CurrentTarget();
        const auto recovered = storage_->ResolveRestoration(recovery);
        if (!recovered.has_value())
            return Result<UiFocusChange>::Success(storage_->NoTarget(UiFocusChangeReason::InvalidTarget));
        storage_->focusedIndex = recovered;
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::InvalidTarget));
    }

    /** @copydoc UiFocusGraph::PushModal */
    Result<UiFocusModalActivation> UiFocusGraph::PushModal(const UiFocusModalDescriptor &descriptor) {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusModalActivation>(UiErrors::FocusLifecycleUnavailable);
        if (!descriptor.IsValid())
            return Failure<UiFocusModalActivation>(UiErrors::FocusInvalid);
        if (storage_->modalDepth >= storage_->modalSlots.size() || storage_->restorationDepth >= storage_->restorations.size())
            return Failure<UiFocusModalActivation>(UiErrors::FocusModalCapacityExceeded);
        if (descriptor.root.ownership != storage_->descriptor.owner.instance.ownership)
            return Failure<UiFocusModalActivation>(UiErrors::FocusSourceStale);

        const std::size_t rootIndex = storage_->FindNode(descriptor.root);
        if (rootIndex == Storage::InvalidIndex)
            return Failure<UiFocusModalActivation>(UiErrors::FocusTargetUnavailable);
        if (!storage_->IsWithinModal(rootIndex))
            return Failure<UiFocusModalActivation>(UiErrors::FocusModalBoundaryViolation);
        const UiElementId rootId = descriptor.rootId.IsValid() ? descriptor.rootId : storage_->nodes[rootIndex].descriptor.id;
        if (rootId != storage_->nodes[rootIndex].descriptor.id)
            return Failure<UiFocusModalActivation>(UiErrors::FocusSourceStale);

        Storage::ModalSlot &slot = storage_->modalSlots[storage_->modalDepth];
        if (slot.generation == 0 || slot.generation == std::numeric_limits<std::uint32_t>::max())
            return Failure<UiFocusModalActivation>(UiErrors::GenerationExhausted);

        const auto previous = storage_->CurrentTarget();
        Storage::RestorationEntry restoration;
        storage_->CollectPath(storage_->focusedIndex, restoration);
        storage_->restorations[storage_->restorationDepth++] = restoration;

        slot.root = rootId;
        slot.rootHandle = descriptor.root;
        slot.defaultFocus = descriptor.defaultFocus;
        slot.policy = descriptor.policy;
        ++storage_->modalDepth;
        storage_->focusedIndex = storage_->ResolveInitial();

        const UiFocusModalId id{storage_->descriptor.owner.instance.ownership, storage_->modalDepth, slot.generation};
        return Result<UiFocusModalActivation>::Success(
            UiFocusModalActivation{id, storage_->BuildChange(previous, UiFocusChangeReason::ModalOpened)});
    }

    /** @copydoc UiFocusGraph::PopModal */
    Result<UiFocusChange> UiFocusGraph::PopModal(const UiFocusModalId modal) {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        if (!modal.IsValid() || modal.ownership != storage_->descriptor.owner.instance.ownership || storage_->modalDepth == 0 ||
            modal.slot != storage_->modalDepth || modal.generation != storage_->modalSlots[storage_->modalDepth - 1].generation)
            return Failure<UiFocusChange>(UiErrors::FocusModalStale);

        Storage::ModalSlot &slot = storage_->modalSlots[storage_->modalDepth - 1];
        if (slot.generation == std::numeric_limits<std::uint32_t>::max())
            return Failure<UiFocusChange>(UiErrors::GenerationExhausted);
        const auto previous = storage_->CurrentTarget();
        const Storage::RestorationEntry restoration = storage_->restorations[storage_->restorationDepth - 1];
        --storage_->modalDepth;
        --storage_->restorationDepth;
        ++slot.generation;
        storage_->focusedIndex = storage_->ResolveRestoration(restoration);
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::ModalClosed));
    }

    /** @copydoc UiFocusGraph::ClearFocus */
    Result<UiFocusChange> UiFocusGraph::ClearFocus() {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        const auto previous = storage_->CurrentTarget();
        storage_->focusedIndex.reset();
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::Explicit));
    }

    /** @copydoc UiFocusGraph::BeginRetirement */
    Result<void> UiFocusGraph::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure(UiErrors::FocusLifecycleUnavailable);
        storage_->focusedIndex.reset();
        storage_->modalDepth = 0;
        storage_->restorationDepth = 0;
        storage_->nodes.clear();
        storage_->modalSlots.clear();
        storage_->restorations.clear();
        storage_->lifecycle = UiFocusGraphState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiFocusGraph::Shutdown */
    void UiFocusGraph::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiFocusGraphState::Stopped)
            return;
        storage_->focusedIndex.reset();
        storage_->modalDepth = 0;
        storage_->restorationDepth = 0;
        storage_->nodes.clear();
        storage_->modalSlots.clear();
        storage_->restorations.clear();
        storage_->lifecycle = UiFocusGraphState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
