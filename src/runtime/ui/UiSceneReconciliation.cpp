#include "UiSceneReconciliationInternal.h"

#include <limits>
#include <new>

namespace Horo::Runtime::Ui {
    using UiSceneDetail::Failure;

    /** @copydoc UiSemanticOwner::IsValid */
    bool UiSemanticOwner::IsValid(const UiOwnershipGeneration ownership) const noexcept {
        using enum UiOwnerScopeKind;
        if (!ownership.IsValid() || kind >= Count)
            return false;
        if ((kind == Player) != player.has_value() || (kind == Scene) != scene.has_value() || (kind == Viewport) != viewport.has_value())
            return false;
        return (!player || (player->IsValid() && player->ownership == ownership)) && (!scene || scene->IsValid()) &&
               (!viewport || (viewport->IsValid() && viewport->ownership == ownership));
    }

    /** @brief Qualifies actual publisher identity and complete host-declared Scene binding lineage. */
    Result<void> UiSceneReconciliation::Storage::Validate(const UiSceneInstanceDescriptor &descriptor,
                                                          const UiReloadGeneration &generation) const {
        if (!descriptor.owner.IsValid(limits.ownership) || !descriptor.instance.IsValid() ||
            descriptor.instance.ownership != limits.ownership || descriptor.instance != generation.Instance().InstanceId() ||
            generation.Instance().State() != UiRuntimeInstanceState::Active)
            return Failure(UiErrors::HandleOwnerMismatch);
        bool sceneProviders = false;
        for (const auto &canvas : generation.Canvases()) {
            if (canvas.bindings) {
                if (const auto valid = canvas.bindings->ValidateOwner(canvas.tree); valid.HasError())
                    return valid;
                sceneProviders = sceneProviders || canvas.bindings->HasSceneProviders();
            }
        }
        if (sceneProviders != descriptor.providerScene.has_value() || (descriptor.providerScene && !descriptor.providerScene->IsValid()) ||
            (descriptor.owner.scene && descriptor.providerScene && descriptor.owner.scene != descriptor.providerScene))
            return Failure(UiErrors::BindingDescriptorInvalid);
        return Result<void>::Success();
    }

    /** @brief Burns the host-issued instance namespace before fallible private admission. */
    Result<void> UiSceneReconciliation::Storage::Issue(const UiSceneInstanceDescriptor &descriptor) {
        if (!descriptor.instance.IsValid() || descriptor.instance.ownership != limits.ownership)
            return Failure(UiErrors::HandleOwnerMismatch);
        if (descriptor.instance.slot <= issued)
            return Failure(UiErrors::HandleStale);
        issued = descriptor.instance.slot;
        return Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Create */
    Result<UiSceneReconciliation> UiSceneReconciliation::Create(const UiSceneReconciliationLimits &limits) {
        if (!limits.ownership.IsValid())
            return Failure<UiSceneReconciliation>(UiErrors::OwnershipGenerationInvalid);
        if (limits.maximumInstances == 0 || limits.maximumInstances > 64 || limits.maximumRetiredInstances == 0 ||
            limits.maximumRetiredInstances > 64 || limits.maximumPreparedTransitions == 0 || limits.maximumPreparedTransitions > 64)
            return Failure<UiSceneReconciliation>(UiErrors::CapacityExceeded);
        try {
            return Result<UiSceneReconciliation>::Success(UiSceneReconciliation{std::make_shared<Storage>(limits)});
        } catch (const std::bad_alloc &) {
            return Failure<UiSceneReconciliation>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiSceneReconciliation::Admit */
    Result<void> UiSceneReconciliation::Admit(const UiSceneInstanceDescriptor &descriptor, UiHotReload &&publisher,
                                              const UiStructuralCommitPoint point) {
        if (!storage_ || storage_.Get()->stopped || storage_.Get()->collecting || !UiSceneDetail::Cutoff(point))
            return Failure(UiErrors::InstanceStateInvalid);
        if (storage_.Get()->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure(UiErrors::GenerationExhausted);
        const auto lease = publisher.Acquire();
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        if (const auto valid = storage_.Get()->Validate(descriptor, *lease.Value().Get()); valid.HasError())
            return valid;
        if (const auto issued = storage_.Get()->Issue(descriptor); issued.HasError())
            return issued;
        const auto free = std::ranges::find_if(storage_.Get()->active, [](const auto &entry) {
            return !entry;
        });
        if (free == storage_.Get()->active.end())
            return Failure(UiErrors::CapacityExceeded);
        free->emplace(descriptor, std::move(publisher));
        ++storage_.Get()->revision;
        return Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Publisher */
    UiHotReload *UiSceneReconciliation::Publisher(const RuntimeUiInstanceId instance) noexcept {
        if (!storage_ || storage_.Get()->stopped || storage_.Get()->collecting)
            return nullptr;
        const auto slot = storage_.Get()->Find(instance);
        return slot == storage_.Get()->active.size() ? nullptr : &storage_.Get()->active[slot]->publisher;
    }

    /** @copydoc UiSceneReconciliation::Acquire */
    Result<UiReloadLease> UiSceneReconciliation::Acquire(const RuntimeUiInstanceId instance) const {
        if (!storage_ || storage_.Get()->stopped || storage_.Get()->collecting)
            return Failure<UiReloadLease>(UiErrors::InstanceStateInvalid);
        const auto slot = storage_.Get()->Find(instance);
        return slot == storage_.Get()->active.size() ? Failure<UiReloadLease>(UiErrors::HandleStale)
                                                     : storage_.Get()->active[slot]->publisher.Acquire();
    }

    /** @copydoc UiSceneReconciliation::UiSceneReconciliation */
    UiSceneReconciliation::UiSceneReconciliation(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiSceneReconciliation::~UiSceneReconciliation */
    UiSceneReconciliation::~UiSceneReconciliation() {
        Shutdown();
    }

    /** @copydoc UiSceneReconciliation::UiSceneReconciliation */
    UiSceneReconciliation::UiSceneReconciliation(UiSceneReconciliation &&) noexcept = default;

    /** @copydoc UiSceneReconciliation::operator= */
    UiSceneReconciliation &UiSceneReconciliation::operator=(UiSceneReconciliation &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiSceneReconciliation::LastIssuedInstanceSlot */
    std::uint32_t UiSceneReconciliation::LastIssuedInstanceSlot() const noexcept {
        return storage_ ? storage_.Get()->issued : 0;
    }

    /** @copydoc UiSceneReconciliation::Prepared::Prepared */
    UiSceneReconciliation::Prepared::Prepared(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiSceneReconciliation::Prepared::~Prepared */
    UiSceneReconciliation::Prepared::~Prepared() = default;
    /** @copydoc UiSceneReconciliation::Prepared::Prepared */
    UiSceneReconciliation::Prepared::Prepared(Prepared &&) noexcept = default;
    /** @copydoc UiSceneReconciliation::Prepared::operator= */
    UiSceneReconciliation::Prepared &UiSceneReconciliation::Prepared::operator=(Prepared &&) noexcept = default;

    /** @copydoc UiSceneReconciliation::Prepared::Result */
    const UiSceneReconciliationResult &UiSceneReconciliation::Prepared::Result() const noexcept {
        return storage_->result;
    }
}  // namespace Horo::Runtime::Ui
