#include "UiOverlayLifecycleInternal.h"

#include <utility>

namespace Horo::Runtime::Ui {
    using OverlayDetail::Failure;

    /** @copydoc UiOverlayLifecycle::Create */
    Result<UiOverlayLifecycle> UiOverlayLifecycle::Create(const UiOverlayLifecycleDescriptor &descriptor) {
        if (!descriptor.ownership.IsValid() || descriptor.maximumLayers == 0 || descriptor.maximumLayers > 64 ||
            descriptor.maximumRetiredLayers == 0 || descriptor.maximumRetiredLayers > 64)
            return Failure<UiOverlayLifecycle>(UiErrors::CapacityExceeded);
        try {
            return Result<UiOverlayLifecycle>::Success(UiOverlayLifecycle{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiOverlayLifecycle>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiOverlayLifecycle::UiOverlayLifecycle */
    UiOverlayLifecycle::UiOverlayLifecycle(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiOverlayLifecycle::~UiOverlayLifecycle */
    UiOverlayLifecycle::~UiOverlayLifecycle() {
        Shutdown();
    }

    /** @copydoc UiOverlayLifecycle::UiOverlayLifecycle */
    UiOverlayLifecycle::UiOverlayLifecycle(UiOverlayLifecycle &&) noexcept = default;

    /** @copydoc UiOverlayLifecycle::operator= */
    UiOverlayLifecycle &UiOverlayLifecycle::operator=(UiOverlayLifecycle &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiOverlayLifecycle::Show */
    Result<UiOverlayLayerId> UiOverlayLifecycle::Show(const UiOverlayLayerDescriptor &descriptor, UiHotReload &&publisher) {
        if (State() != UiOverlayLifecycleState::Active || storage_->suspended || storage_->collecting)
            return Failure<UiOverlayLayerId>(UiErrors::InstanceStateInvalid);
        auto slot = std::ranges::find_if(storage_->active, [](const auto &entry) {
            return !entry;
        });
        if (slot == storage_->active.end())
            return Failure<UiOverlayLayerId>(UiErrors::CapacityExceeded);
        if (storage_->lastIssued == std::numeric_limits<std::uint32_t>::max())
            return Failure<UiOverlayLayerId>(UiErrors::GenerationExhausted);
        auto *generation = publisher.Current();
        if (!generation)
            return Failure<UiOverlayLayerId>(UiErrors::InstanceStateInvalid);
        const auto route = OverlayDetail::Validate(*generation, descriptor, storage_->descriptor.ownership);
        if (route.HasError())
            return Result<UiOverlayLayerId>::Failure(route.ErrorValue());
        auto *canvas = generation->Canvas(descriptor.canvas);
        if (storage_->Conflicts(descriptor, *canvas))
            return Failure<UiOverlayLayerId>(UiErrors::HandleOwnerMismatch);
        if (auto focused = OverlayDetail::Focus(*canvas, descriptor); focused.HasError())
            return Result<UiOverlayLayerId>::Failure(focused.ErrorValue());
        const UiOverlayLayerId id{storage_->descriptor.ownership, 1, ++storage_->lastIssued};
        slot->emplace(Storage::Layer{id, descriptor, route.Value(), std::move(publisher), {}, false, false});
        storage_->Reconcile();
        return Result<UiOverlayLayerId>::Success(id);
    }

    /** @copydoc UiOverlayLifecycle::ApplyPresentation */
    Result<bool> UiOverlayLifecycle::ApplyPresentation(UiOverlayLayerId layer, const UiPresentationReceipt &receipt) {
        auto *publisher = Publisher(layer);
        if (!publisher)
            return Failure<bool>(UiErrors::HandleStale);
        auto *entry = storage_->Find(layer);
        if (receipt.view != entry->binding.view)
            return Failure<bool>(UiErrors::HandleOwnerMismatch);
        auto result = publisher->ApplyPresentation(entry->binding.canvas, receipt);
        if (result.HasValue() && receipt.outcome == UiPresentationOutcome::Presented)
            entry->needsPresentation = false;
        return result;
    }

    /** @copydoc UiOverlayLifecycle::InputStatus */
    UiOverlayInputStatus UiOverlayLifecycle::InputStatus(UiOverlayLayerId layer) const noexcept {
        using enum UiOverlayInputStatus;
        if (State() != UiOverlayLifecycleState::Active || storage_->collecting)
            return Retired;
        const auto *entry = std::as_const(*storage_).Find(layer);
        if (!entry)
            return Retired;
        if (!Storage::Live(*entry))
            return Retired;
        if (storage_->suspended)
            return Suspended;
        if (!entry->binding.interactive || storage_->Blocked(*entry))
            return Blocked;
        if (entry->needsPresentation || !entry->publisher.InputEligible(entry->binding.canvas, entry->binding.view))
            return AwaitingPresentation;
        return Eligible;
    }

    /** @copydoc UiOverlayLifecycle::Snapshot */
    Result<std::size_t> UiOverlayLifecycle::Snapshot(std::span<UiOverlayLayerSnapshot> output) const {
        if (State() != UiOverlayLifecycleState::Active || storage_->collecting)
            return Failure<std::size_t>(UiErrors::InstanceStateInvalid);
        if (const auto count = static_cast<std::size_t>(std::ranges::count_if(storage_->active,
                                                                              [](const auto &entry) {
            return !!entry;
        }));
            count > output.size())
            return Failure<std::size_t>(UiErrors::CapacityExceeded);
        std::array<const Storage::Layer *, 64> ordered{};
        std::size_t written{};
        for (const auto &entry : storage_->active)
            if (entry)
                ordered[written++] = &*entry;
        std::sort(ordered.begin(), ordered.begin() + written, [](const auto *a, const auto *b) {
            return Storage::Higher(*b, *a);
        });
        std::array<UiOverlayLayerSnapshot, 64> staged{};
        for (std::size_t i = 0; i < written; ++i) {
            const auto &entry = *ordered[i];
            const auto projection = Storage::Project(entry, InputStatus(entry.id));
            if (projection.HasError())
                return Result<std::size_t>::Failure(projection.ErrorValue());
            staged[i] = projection.Value();
        }
        std::copy_n(staged.begin(), written, output.begin());
        return Result<std::size_t>::Success(written);
    }

    /** @copydoc UiOverlayLifecycle::Dismiss */
    Result<void> UiOverlayLifecycle::Dismiss(UiOverlayLayerId layer) {
        if (State() != UiOverlayLifecycleState::Active || storage_->collecting)
            return Failure<void>(UiErrors::InstanceStateInvalid);
        auto slot = std::ranges::find_if(storage_->active, [layer](const auto &entry) {
            return entry && entry->id == layer;
        });
        if (slot == storage_->active.end())
            return Failure<void>(UiErrors::HandleStale);
        if ((*slot)->binding.exclusivity != UiModalExclusivity::None && storage_->Blocked(**slot))
            return Failure<void>(UiErrors::FocusModalStale);
        if (storage_->RetiredCount() >= storage_->descriptor.maximumRetiredLayers)
            return Failure<void>(UiErrors::CapacityExceeded);
        storage_->Retire(*slot);
        storage_->Reconcile();
        return Result<void>::Success();
    }

    /** @copydoc UiOverlayLifecycle::SetSuspended */
    Result<void> UiOverlayLifecycle::SetSuspended(bool suspended) {
        if (State() != UiOverlayLifecycleState::Active || storage_->collecting)
            return Failure<void>(UiErrors::InstanceStateInvalid);
        if (storage_->suspended == suspended)
            return Result<void>::Success();
        storage_->suspended = suspended;
        for (auto &entry : storage_->active)
            if (entry) {
                entry->needsPresentation = true;
                Storage::CancelCapture(*entry, UiPointerCaptureCancellationReason::Suspended);
            }
        return Result<void>::Success();
    }

    /** @copydoc UiOverlayLifecycle::Publisher */
    UiHotReload *UiOverlayLifecycle::Publisher(UiOverlayLayerId layer) noexcept {
        if (State() != UiOverlayLifecycleState::Active || storage_->collecting)
            return nullptr;
        auto *entry = storage_->Find(layer);
        return entry ? &entry->publisher : nullptr;
    }

    /** @copydoc UiOverlayLifecycle::PrepareReload */
    Result<UiHotReload::Prepared> UiOverlayLifecycle::PrepareReload(UiOverlayLayerId layer, UiReloadGeneration replacement,
                                                                    const CancellationToken &cancellation) {
        auto *publisher = Publisher(layer);
        if (!publisher)
            return Failure<UiHotReload::Prepared>(UiErrors::HandleStale);
        return publisher->Prepare(std::move(replacement), cancellation);
    }

    /** @copydoc UiOverlayLifecycle::CommitReload */
    Result<UiReloadReconciliation> UiOverlayLifecycle::CommitReload(UiOverlayLayerId layer, UiHotReload::Prepared &prepared,
                                                                    UiStructuralCommitPoint point) {
        auto *publisher = Publisher(layer);
        if (!publisher)
            return Failure<UiReloadReconciliation>(UiErrors::HandleStale);
        auto *replacement = publisher->OverlayReplacement(prepared);
        if (!replacement)
            return Failure<UiReloadReconciliation>(UiErrors::RevisionStale);
        auto *entry = storage_->Find(layer);
        const auto route = OverlayDetail::Validate(*replacement, entry->binding, storage_->descriptor.ownership);
        if (route.HasError())
            return Result<UiReloadReconciliation>::Failure(route.ErrorValue());
        if (route.Value().metadata != entry->route.metadata)
            return Failure<UiReloadReconciliation>(UiErrors::RouteOperationInvalid);
        // The private candidate may be retried after a cancelled/full-retention commit. Do not duplicate its modal trap.
        if (auto *canvas = replacement->Canvas(entry->binding.canvas);
            entry->binding.exclusivity != UiModalExclusivity::None && canvas->focus->Snapshot().Value().modalDepth == 0) {
            if (auto focused = OverlayDetail::Focus(*canvas, entry->binding); focused.HasError())
                return Result<UiReloadReconciliation>::Failure(focused.ErrorValue());
        }
        const auto result = publisher->Commit(prepared, point);
        if (result.HasError())
            return result;
        entry->route = route.Value();
        entry->needsPresentation = true;
        if (entry->blocked)
            (void)Storage::Canvas(*entry)->focus->ClearFocus();
        return result;
    }

    /** @copydoc UiOverlayLifecycle::BeginRetirement */
    Result<void> UiOverlayLifecycle::BeginRetirement() {
        using enum UiOverlayLifecycleState;
        if (State() == Stopped || storage_->collecting)
            return Failure<void>(UiErrors::InstanceStateInvalid);
        if (storage_->state == Retiring)
            return Result<void>::Success();
        for (auto &entry : storage_->active)
            if (entry)
                storage_->Retire(entry);
        storage_->state = Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiOverlayLifecycle::Shutdown */
    void UiOverlayLifecycle::Shutdown() noexcept {
        using enum UiOverlayLifecycleState;
        if (!storage_)
            return;
        if (storage_->collecting) {
            storage_->shutdownRequested = true;
            storage_->state = Stopped;
            return;
        }
        if (storage_->state == Stopped && !storage_->shutdownRequested)
            return;
        for (auto &entry : storage_->active)
            if (entry)
                storage_->Retire(entry);
        storage_->state = Stopped;
        storage_->shutdownRequested = false;
    }

    /** @copydoc UiOverlayLifecycle::CollectRetired */
    Result<std::size_t> UiOverlayLifecycle::CollectRetired() {
        if (!storage_ || storage_->collecting)
            return Failure<std::size_t>(UiErrors::InstanceStateInvalid);
        storage_->collecting = true;

        struct DrainGuard final {
            DrainGuard(UiOverlayLifecycle &ownerValue, Storage &storageValue) noexcept : owner(ownerValue), storage(storageValue) {}

            DrainGuard(const DrainGuard &) = delete;
            DrainGuard &operator=(const DrainGuard &) = delete;
            DrainGuard(DrainGuard &&) = delete;
            DrainGuard &operator=(DrainGuard &&) = delete;

            UiOverlayLifecycle &owner;
            Storage &storage;

            ~DrainGuard() {
                storage.collecting = false;
                if (storage.shutdownRequested)
                    owner.Shutdown();
            }
        };

        const DrainGuard guard{*this, *storage_};

        std::size_t reclaimed{};
        for (auto &entry : storage_->retired) {
            if (!entry)
                continue;
            if (const auto drained = entry->publisher.CollectRetired(); drained.HasError())
                return Result<std::size_t>::Failure(drained.ErrorValue());
            if (entry->publisher.CanReclaim()) {
                entry.reset();
                ++reclaimed;
            }
        }
        return Result<std::size_t>::Success(reclaimed);
    }

    /** @copydoc UiOverlayLifecycle::CanReclaim */
    bool UiOverlayLifecycle::CanReclaim() const noexcept {
        return !storage_ || (!storage_->collecting && !storage_->shutdownRequested && storage_->state != UiOverlayLifecycleState::Active &&
                             storage_->RetiredCount() == 0);
    }

    /** @copydoc UiOverlayLifecycle::State */
    UiOverlayLifecycleState UiOverlayLifecycle::State() const noexcept {
        return storage_ ? storage_->state : UiOverlayLifecycleState::Stopped;
    }

    /** @copydoc UiOverlayLifecycle::LastIssuedLayerIncarnation */
    std::uint32_t UiOverlayLifecycle::LastIssuedLayerIncarnation() const noexcept {
        return storage_ ? storage_->lastIssued : 0;
    }
}  // namespace Horo::Runtime::Ui
