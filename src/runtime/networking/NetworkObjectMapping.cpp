#include "Horo/Network/NetworkObjectMapping.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }
    }  // namespace

    NetworkObjectMappingSnapshot::NetworkObjectMappingSnapshot(const ReplicationAuthorityEpoch epoch, const Runtime::SceneRuntimeId scene,
                                                               const NetworkObjectMappingState state,
                                                               std::vector<NetworkObjectMappingEntry> entries) noexcept
        : epoch_(epoch), scene_(scene), state_(state), entries_(std::move(entries)) {}

    NetworkObjectMapping::NetworkObjectMapping(const ReplicationAuthorityEpoch epoch, const Runtime::SceneRuntimeId scene,
                                               const std::size_t maximumSlots, std::vector<SlotRecord> slots,
                                               std::vector<EntityRecord> entityIndex) noexcept
        : epoch_(epoch), scene_(scene), maximumSlots_(maximumSlots), slots_(std::move(slots)), entityIndex_(std::move(entityIndex)),
          state_(NetworkObjectMappingState::Active) {}

    /** @copydoc NetworkObjectMapping::Create */
    Result<NetworkObjectMapping> NetworkObjectMapping::Create(const ReplicationAuthorityEpoch epoch, const Runtime::SceneRuntimeId scene,
                                                              const std::size_t maximumSlots) {
        if (!epoch.IsValid() || !scene.IsValid() || maximumSlots == 0)
            return Fail<NetworkObjectMapping>(NetworkErrors::NetworkObjectMappingInvalid);
        try {
            std::vector<SlotRecord> slots;
            slots.reserve(maximumSlots);
            std::vector<EntityRecord> entityIndex;
            entityIndex.reserve(maximumSlots);
            return Result<NetworkObjectMapping>::Success(
                NetworkObjectMapping{epoch, scene, maximumSlots, std::move(slots), std::move(entityIndex)});
        } catch (const std::bad_alloc &) {
            return Fail<NetworkObjectMapping>(NetworkErrors::NetworkObjectMappingCapacityExceeded);
        }
    }

    std::vector<NetworkObjectMapping::SlotRecord>::iterator NetworkObjectMapping::LowerBound(const std::uint64_t slot) noexcept {
        return std::ranges::lower_bound(slots_, slot, {}, &SlotRecord::slot);
    }

    std::vector<NetworkObjectMapping::SlotRecord>::const_iterator NetworkObjectMapping::LowerBound(
        const std::uint64_t slot) const noexcept {
        return std::ranges::lower_bound(slots_, slot, {}, &SlotRecord::slot);
    }

    std::vector<NetworkObjectMapping::EntityRecord>::iterator NetworkObjectMapping::LowerBound(const Runtime::EntityRef entity) noexcept {
        return std::ranges::lower_bound(entityIndex_, entity, {}, &EntityRecord::entity);
    }

    std::vector<NetworkObjectMapping::EntityRecord>::const_iterator NetworkObjectMapping::LowerBound(
        const Runtime::EntityRef entity) const noexcept {
        return std::ranges::lower_bound(entityIndex_, entity, {}, &EntityRecord::entity);
    }

    Result<void> NetworkObjectMapping::RequireActive() const {
        if (state_ != NetworkObjectMappingState::Active)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingTerminal));
        return Result<void>::Success();
    }

    Result<void> NetworkObjectMapping::ValidateRegistration(const NetworkObjectMappingEntry &entry) const {
        if (const auto active = RequireActive(); active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        if (!entry.object.IsValid() || entry.object.Epoch() != epoch_ || !entry.entity.IsValid() || entry.entity.runtime != scene_ ||
            !entry.provenance.IsValid())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingInvalid));
        if (const auto entity = LowerBound(entry.entity); entity != entityIndex_.end() && entity->entity == entry.entity)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingConflict));
        return Result<void>::Success();
    }

    Result<void> NetworkObjectMapping::ReconcileSlot(const NetworkObjectMappingEntry &entry) {
        const auto found = LowerBound(entry.object.Slot());
        if (found == slots_.end() || found->slot != entry.object.Slot()) {
            if (slots_.size() == maximumSlots_)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingCapacityExceeded));
            slots_.emplace(found, entry.object.Slot(), entry.object.Generation(), entry);
            return Result<void>::Success();
        }
        if (found->live.has_value())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingConflict));
        if (found->generation == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectGenerationExhausted));
        if (entry.object.Generation() != found->generation + 1)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
        found->generation = entry.object.Generation();
        found->live = entry;
        return Result<void>::Success();
    }

    /** @copydoc NetworkObjectMapping::Register */
    Result<void> NetworkObjectMapping::Register(const NetworkObjectMappingEntry &entry) {
        if (const auto valid = ValidateRegistration(entry); valid.HasError())
            return valid;
        if (const auto reconciled = ReconcileSlot(entry); reconciled.HasError())
            return reconciled;
        const auto entity = LowerBound(entry.entity);
        entityIndex_.emplace(entity, entry.entity, entry.object);
        ++liveCount_;
        return Result<void>::Success();
    }

    /** @copydoc NetworkObjectMapping::Resolve */
    Result<Runtime::EntityRef> NetworkObjectMapping::Resolve(const NetworkObjectId object) const {
        if (const auto active = RequireActive(); active.HasError())
            return Fail<Runtime::EntityRef>(NetworkErrors::NetworkObjectMappingTerminal);
        if (!object.IsValid())
            return Fail<Runtime::EntityRef>(NetworkErrors::NetworkObjectIdentityInvalid);
        if (object.Epoch() != epoch_)
            return Fail<Runtime::EntityRef>(NetworkErrors::NetworkObjectMappingUnknown);
        const auto found = LowerBound(object.Slot());
        if (found == slots_.end() || found->slot != object.Slot() || !found->live.has_value() || found->generation != object.Generation())
            return Fail<Runtime::EntityRef>(NetworkErrors::NetworkObjectMappingUnknown);
        return Result<Runtime::EntityRef>::Success(found->live->entity);
    }

    /** @copydoc NetworkObjectMapping::ResolveEntry */
    Result<NetworkObjectMappingEntry> NetworkObjectMapping::ResolveEntry(const NetworkObjectId object) const {
        if (const auto entity = Resolve(object); entity.HasError())
            return Result<NetworkObjectMappingEntry>::Failure(entity.ErrorValue());
        return Result<NetworkObjectMappingEntry>::Success(*LowerBound(object.Slot())->live);
    }

    /** @copydoc NetworkObjectMapping::Find */
    Result<NetworkObjectId> NetworkObjectMapping::Find(const Runtime::EntityRef entity) const {
        if (const auto active = RequireActive(); active.HasError())
            return Fail<NetworkObjectId>(NetworkErrors::NetworkObjectMappingTerminal);
        if (!entity.IsValid() || entity.runtime != scene_)
            return Fail<NetworkObjectId>(NetworkErrors::NetworkObjectMappingInvalid);
        const auto found = LowerBound(entity);
        if (found == entityIndex_.end() || found->entity != entity)
            return Fail<NetworkObjectId>(NetworkErrors::NetworkObjectMappingUnknown);
        return Result<NetworkObjectId>::Success(found->object);
    }

    /** @copydoc NetworkObjectMapping::Retire */
    Result<void> NetworkObjectMapping::Retire(const NetworkObjectId object) {
        if (const auto active = RequireActive(); active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        if (!object.IsValid())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectIdentityInvalid));
        if (object.Epoch() != epoch_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
        const auto found = LowerBound(object.Slot());
        if (found == slots_.end() || found->slot != object.Slot() || !found->live.has_value() || found->generation != object.Generation())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
        const auto entity = LowerBound(found->live->entity);
        entityIndex_.erase(entity);
        found->live.reset();
        --liveCount_;
        return Result<void>::Success();
    }

    /** @copydoc NetworkObjectMapping::InvalidateScene */
    Result<void> NetworkObjectMapping::InvalidateScene(const Runtime::SceneRuntimeId scene) {
        if (const auto active = RequireActive(); active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        if (!scene.IsValid() || scene != scene_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingInvalid));
        for (auto &record : slots_)
            record.live.reset();
        entityIndex_.clear();
        liveCount_ = 0;
        state_ = NetworkObjectMappingState::SceneInvalidated;
        return Result<void>::Success();
    }

    /** @copydoc NetworkObjectMapping::BeginShutdown */
    void NetworkObjectMapping::BeginShutdown() noexcept {
        if (state_ == NetworkObjectMappingState::Active)
            state_ = NetworkObjectMappingState::ShuttingDown;
    }

    /** @copydoc NetworkObjectMapping::Snapshot */
    Result<NetworkObjectMappingSnapshot> NetworkObjectMapping::Snapshot() const {
        try {
            std::vector<NetworkObjectMappingEntry> entries;
            entries.reserve(liveCount_);
            for (const auto &record : slots_) {
                if (record.live.has_value())
                    entries.push_back(*record.live);
            }
            return Result<NetworkObjectMappingSnapshot>::Success(NetworkObjectMappingSnapshot{epoch_, scene_, state_, std::move(entries)});
        } catch (const std::bad_alloc &) {
            return Fail<NetworkObjectMappingSnapshot>(NetworkErrors::NetworkObjectMappingCapacityExceeded);
        }
    }
}  // namespace Horo::Network
