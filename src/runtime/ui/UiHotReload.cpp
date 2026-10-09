#include "UiHotReloadInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Requires the same actual authored source and advancing committed asset/document publication. */
        [[nodiscard]] bool Newer(const UiReloadGeneration &old, const UiReloadGeneration &replacement) noexcept {
            return old.Asset() == replacement.Asset() && old.Instance().InstanceId() == replacement.Instance().InstanceId() &&
                   old.Instance().DocumentId() == replacement.Instance().DocumentId() &&
                   old.Instance().DocumentRevision() < replacement.Instance().DocumentRevision() &&
                   old.RegistryRevision() <= replacement.RegistryRevision();
        }
    }  // namespace

    /** @copydoc UiHotReload::Create */
    Result<UiHotReload> UiHotReload::Create(UiReloadGeneration initial, UiHotReloadLimits limits) {
        if (!limits.IsValid() || !initial.storage_)
            return Failure<UiHotReload>(UiErrors::CapacityExceeded);
        if (const auto valid = UiReloadDetail::Validate(initial.Instance(), initial.Canvases()); valid.HasError())
            return Result<UiHotReload>::Failure(valid.ErrorValue());
        const auto slots = UiReloadDetail::Namespace(initial.Canvases());
        if (slots.HasError())
            return Result<UiHotReload>::Failure(slots.ErrorValue());
        try {
            auto storage = std::make_shared<Storage>(limits);
            storage->current = std::make_shared<UiReloadGeneration>(std::move(initial));
            storage->issuedNamespaceEnd = slots.Value().second;
            if (const auto active = storage->current->storage_->instance.Activate(); active.HasError())
                return Result<UiHotReload>::Failure(active.ErrorValue());
            return Result<UiHotReload>::Success(UiHotReload{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiHotReload>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiHotReload::Prepare */
    Result<UiHotReload::Prepared> UiHotReload::Prepare(UiReloadGeneration replacement, const CancellationToken &cancellation) {
        if (!storage_ || storage_->stopped || storage_->collecting)
            return Failure<Prepared>(UiErrors::InstanceStateInvalid);
        if (!replacement.storage_ || !Newer(*storage_->current, replacement))
            return Failure<Prepared>(UiErrors::RevisionStale);
        if (const auto valid = UiReloadDetail::Validate(replacement.Instance(), replacement.Canvases()); valid.HasError())
            return Result<Prepared>::Failure(valid.ErrorValue());
        const auto slots = UiReloadDetail::Namespace(replacement.Canvases());
        if (slots.HasError())
            return Result<Prepared>::Failure(slots.ErrorValue());
        if (slots.Value().first < storage_->issuedNamespaceEnd)
            return Failure<Prepared>(UiErrors::HandleStale);
        // Burn the entire actual reservation before any cancellation/reconciliation failure. Retired history never resets this fence.
        storage_->issuedNamespaceEnd = slots.Value().second;
        if (cancellation.IsCancellationRequested())
            return Failure<Prepared>(UiErrors::AssetLoadCancelled);
        if (storage_->preparedCount == storage_->limits.maximumPreparedGenerations)
            return Failure<Prepared>(UiErrors::CapacityExceeded);
        auto stamps = UiReloadDetail::CaptureSource(*storage_->current);
        if (stamps.HasError())
            return Result<Prepared>::Failure(stamps.ErrorValue());
        const auto reconciliation = UiReloadDetail::Reconcile(*storage_->current, replacement);
        if (reconciliation.HasError())
            return Result<Prepared>::Failure(reconciliation.ErrorValue());
        try {
            auto prepared = std::make_unique<Prepared::Storage>();
            prepared->publisher = storage_.PublisherPin();
            prepared->source = storage_->current;
            prepared->replacement = std::make_shared<UiReloadGeneration>(std::move(replacement));
            prepared->cancellation = cancellation;
            prepared->reconciliation = reconciliation.Value();
            prepared->sourceStamps = std::move(stamps).Value();
            ++storage_->preparedCount;
            prepared->admitted = true;
            return Result<Prepared>::Success(Prepared{std::move(prepared)});
        } catch (const std::bad_alloc &) {
            return Failure<Prepared>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiHotReload::AnimationReplacement */
    UiReloadGeneration *UiHotReload::AnimationReplacement(const Prepared &prepared) noexcept {
        if (!storage_ || !prepared.storage_ || prepared.storage_->consumed || !storage_.Matches(prepared.storage_->publisher))
            return nullptr;
        return prepared.storage_->replacement.get();
    }

    /** @copydoc UiHotReload::OverlayReplacement */
    UiReloadGeneration *UiHotReload::OverlayReplacement(const Prepared &prepared) noexcept {
        return AnimationReplacement(prepared);
    }

    /** @copydoc UiHotReload::Commit */
    Result<UiReloadReconciliation> UiHotReload::Commit(Prepared &prepared, const UiStructuralCommitPoint point) {
        if (!storage_ || storage_->stopped || storage_->collecting || !prepared.storage_ || prepared.storage_->consumed)
            return Failure<UiReloadReconciliation>(UiErrors::InstanceStateInvalid);
        auto &candidate = *prepared.storage_;
        if (!storage_.Matches(candidate.publisher) || candidate.source != storage_->current ||
            !UiReloadDetail::MatchesSource(*storage_->current, candidate.sourceStamps))
            return Failure<UiReloadReconciliation>(UiErrors::RevisionStale);
        if (point != UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands &&
            point != UiStructuralCommitPoint::CommitDeferredLifecycleChanges)
            return Failure<UiReloadReconciliation>(UiErrors::InstanceStateInvalid);
        if (candidate.cancellation.IsCancellationRequested())
            return Failure<UiReloadReconciliation>(UiErrors::AssetLoadCancelled);
        const auto end = storage_->retired.begin() + storage_->limits.maximumRetiredGenerations;
        const auto free = std::find(storage_->retired.begin(), end, nullptr);
        if (free == end)
            return Failure<UiReloadReconciliation>(UiErrors::CapacityExceeded);
        if (const auto activated = candidate.replacement->storage_->instance.Activate(); activated.HasError())
            return Result<UiReloadReconciliation>::Failure(activated.ErrorValue());
        UiReloadDetail::Retire(*storage_->current);
        (void)storage_->current->storage_->instance.BeginRetirement();
        *free = std::move(storage_->current);
        storage_->current = candidate.replacement;
        candidate.consumed = true;
        return Result<UiReloadReconciliation>::Success(candidate.reconciliation);
    }

    /** @copydoc UiHotReload::Acquire */
    Result<UiReloadLease> UiHotReload::Acquire() const {
        if (!storage_ || storage_->stopped || storage_->collecting)
            return Failure<UiReloadLease>(UiErrors::InstanceStateInvalid);
        return Result<UiReloadLease>::Success(UiReloadLease{storage_->current});
    }

    /** @copydoc UiHotReload::Current */
    UiReloadGeneration *UiHotReload::Current() noexcept {
        return storage_ && !storage_->stopped && !storage_->collecting ? storage_->current.get() : nullptr;
    }

    /** @copydoc UiHotReload::OverlayCurrent */
    const UiReloadGeneration *UiHotReload::OverlayCurrent() const noexcept {
        return storage_ && !storage_->stopped && !storage_->collecting ? storage_->current.get() : nullptr;
    }

    /** @copydoc UiHotReload::IsCurrent */
    bool UiHotReload::IsCurrent(const UiReloadLease &lease) const noexcept {
        return storage_ && !storage_->stopped && storage_->current == lease.generation_;
    }

    /** @copydoc UiHotReload::CollectRetired */
    Result<std::size_t> UiHotReload::CollectRetired() {
        if (!storage_ || storage_->collecting)
            return Failure<std::size_t>(UiErrors::InstanceStateInvalid);
        storage_->collecting = true;
        std::size_t reclaimed = 0;
        for (auto &generation : storage_->retired) {
            if (!generation)
                continue;
            if (const auto drained = UiReloadDetail::DrainBindings(*generation); drained.HasError()) {
                storage_->collecting = false;
                return Result<std::size_t>::Failure(drained.ErrorValue());
            }
            if (generation.use_count() == 1 && UiReloadDetail::Drained(*generation)) {
                generation.reset();
                ++reclaimed;
            }
        }
        storage_->collecting = false;
        return Result<std::size_t>::Success(reclaimed);
    }

    /** @copydoc UiHotReload::CanReclaim */
    bool UiHotReload::CanReclaim() const noexcept {
        return !storage_ ||
               (storage_->stopped && storage_->preparedCount == 0 && std::ranges::all_of(storage_->retired, [](const auto &generation) {
            return !generation;
        }));
    }

    /** @copydoc UiHotReload::Shutdown */
    void UiHotReload::Shutdown() noexcept {
        if (!storage_ || storage_->stopped)
            return;
        storage_->stopped = true;
        UiReloadDetail::Retire(*storage_->current);
        (void)storage_->current->storage_->instance.BeginRetirement();
        // The extra reserved slot belongs exclusively to shutdown, even when ordinary retention capacity is full.
        storage_->retired.back() = std::move(storage_->current);
    }

    /** @copydoc UiHotReload::UiHotReload */
    UiHotReload::UiHotReload(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiHotReload::~UiHotReload */
    UiHotReload::~UiHotReload() {
        Shutdown();
    }

    /** @copydoc UiHotReload::UiHotReload */
    UiHotReload::UiHotReload(UiHotReload &&) noexcept = default;

    /** @copydoc UiHotReload::operator= */
    UiHotReload &UiHotReload::operator=(UiHotReload &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiHotReload::Prepared::Prepared */
    UiHotReload::Prepared::Prepared(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiHotReload::Prepared::~Prepared */
    UiHotReload::Prepared::~Prepared() = default;
    /** @copydoc UiHotReload::Prepared::Prepared */
    UiHotReload::Prepared::Prepared(Prepared &&) noexcept = default;
    /** @copydoc UiHotReload::Prepared::operator= */
    UiHotReload::Prepared &UiHotReload::Prepared::operator=(Prepared &&) noexcept = default;

    /** @copydoc UiHotReload::Prepared::Reconciliation */
    const UiReloadReconciliation &UiHotReload::Prepared::Reconciliation() const noexcept {
        return storage_->reconciliation;
    }
}  // namespace Horo::Runtime::Ui
