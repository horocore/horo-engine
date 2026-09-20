#pragma once

#include "Horo/Navigation/NavigationDynamicRegistry.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace Horo::Navigation::Detail {
    template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    struct PendingObstacleRegistration final {
        NavigationObstacleHandle handle;
        NavigationObstacleDescriptor descriptor;
    };

    struct PendingModifierRegistration final {
        NavigationModifierHandle handle;
        NavigationModifierDescriptor descriptor;
    };

    struct PendingObstacleUpdate final {
        NavigationObstacleHandle handle;
        NavigationDynamicRecordRevision expectedRevision;
        NavigationObstacleDescriptor descriptor;
    };

    struct PendingModifierUpdate final {
        NavigationModifierHandle handle;
        NavigationDynamicRecordRevision expectedRevision;
        NavigationModifierDescriptor descriptor;
    };

    struct PendingObstacleRemoval final {
        NavigationObstacleHandle handle;
        NavigationDynamicRecordRevision expectedRevision;
    };

    struct PendingModifierRemoval final {
        NavigationModifierHandle handle;
        NavigationDynamicRecordRevision expectedRevision;
    };

    using NavigationDynamicPendingCommand = std::variant<PendingObstacleRegistration, PendingModifierRegistration, PendingObstacleUpdate,
                                                         PendingModifierUpdate, PendingObstacleRemoval, PendingModifierRemoval>;

    struct NavigationDynamicRegistrySnapshotState final {
        std::array<NavigationObstacleRecord, NavigationDynamicRegistryHardLimits::Obstacles> obstacles{};
        std::array<NavigationModifierRecord, NavigationDynamicRegistryHardLimits::Modifiers> modifiers{};
        std::size_t obstacleCount{};
        std::size_t modifierCount{};
    };

    struct NavigationDynamicRegistryState final {
        struct ObstacleSlot final {
            std::uint32_t generation{1};
            bool active{};
            bool reserved{};
            bool retired{};
            NavigationObstacleRecord record{};
        };

        struct ModifierSlot final {
            std::uint32_t generation{1};
            bool active{};
            bool reserved{};
            bool retired{};
            NavigationModifierRecord record{};
        };

        explicit NavigationDynamicRegistryState(const NavigationDynamicRegistryLimits &limitsIn) : limits(limitsIn) {}

        NavigationDynamicRegistryLimits limits;
        NavigationDynamicSceneBinding binding;
        bool bound{};
        bool hasPublicationTick{};
        std::uint64_t lastPublicationTick{};
        std::array<ObstacleSlot, NavigationDynamicRegistryHardLimits::Obstacles> obstacles{};
        std::array<ModifierSlot, NavigationDynamicRegistryHardLimits::Modifiers> modifiers{};
        std::array<NavigationDynamicPendingCommand, NavigationDynamicRegistryHardLimits::PendingCommands> pending{};
        mutable std::shared_ptr<const NavigationDynamicRegistrySnapshotState> cachedSnapshot;
    };

    [[nodiscard]] inline bool SameOwnerProvenance(const NavigationDynamicProvenance &left,
                                                  const NavigationDynamicProvenance &right) noexcept {
        return left.world == right.world && left.scene == right.scene && left.sceneGeneration == right.sceneGeneration &&
               left.owner == right.owner && left.ownerGeneration == right.ownerGeneration && left.source == right.source &&
               left.authoredModifier == right.authoredModifier;
    }

    [[nodiscard]] inline NavigationDynamicSceneBinding Binding(const NavigationWorldActivationDescriptor &activation) noexcept {
        return {
            .world = activation.world,
            .scene = activation.scene,
            .sceneGeneration = activation.sceneGeneration,
        };
    }

    [[nodiscard]] inline bool Matches(const NavigationDynamicProvenance &provenance,
                                      const NavigationDynamicSceneBinding &binding) noexcept {
        return provenance.world == binding.world && provenance.scene == binding.scene &&
               provenance.sceneGeneration == binding.sceneGeneration;
    }

    [[nodiscard]] inline bool UpdateIntervalAllowed(const std::uint64_t previous, const std::uint64_t next,
                                                    const std::uint64_t minimumInterval) noexcept {
        return next >= previous && next - previous >= minimumInterval;
    }

    [[nodiscard]] inline bool ValidShape(const NavigationDynamicShape &shape) noexcept {
        return std::visit([](const auto &value) {
            return value.IsValid();
        }, shape);
    }

    [[nodiscard]] inline bool ValidPriority(const std::int32_t priority) noexcept {
        return priority >= -1'000'000 && priority <= 1'000'000;
    }

    [[nodiscard]] inline bool ValidOverride(const NavigationModifierDescriptor &descriptor) noexcept {
        const bool validArea = descriptor.area.has_value() && descriptor.area->IsValid();
        const bool validCost =
            descriptor.traversalCost.has_value() && std::isfinite(*descriptor.traversalCost) && *descriptor.traversalCost >= 0.0F;
        using enum NavigationDynamicModifierOperation;
        switch (descriptor.operation) {
            case Exclude:
                return !descriptor.area.has_value() && !descriptor.traversalCost.has_value();
            case OverrideArea:
                return validArea && !descriptor.traversalCost.has_value();
            case OverrideAreaAndCost:
                return validArea && validCost;
            case Count:
                return false;
        }
        return false;
    }

    template <typename SlotArray> [[nodiscard]] std::optional<std::size_t> FindFreeSlot(SlotArray &slots) noexcept {
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (!slots[index].active && !slots[index].reserved && !slots[index].retired)
                return index;
        }
        return std::nullopt;
    }

    template <typename SlotArray, typename Handle>
    [[nodiscard]] Result<std::size_t> ResolveSlot(const SlotArray &slots, const Handle handle,
                                                  const NavigationDynamicSceneBinding &binding) {
        if (!handle.IsValid() || !binding.IsValid() || handle.world != binding.world || handle.slot.index >= slots.size())
            return Failure<std::size_t>(NavigationErrors::InvalidHandle);
        if (const auto &slot = slots[handle.slot.index]; !slot.active || slot.generation != handle.slot.generation)
            return Failure<std::size_t>(NavigationErrors::DynamicRegistryStale);
        return Result<std::size_t>::Success(handle.slot.index);
    }

    template <typename SlotArray> void BurnReservedSlot(SlotArray &slots, const std::uint32_t index) noexcept {
        auto &slot = slots[index];
        slot.reserved = false;
        if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
            slot.retired = true;
            return;
        }
        ++slot.generation;
    }

    template <typename SlotArray, typename Id> [[nodiscard]] bool HasActiveId(const SlotArray &slots, const Id id) noexcept {
        return std::ranges::any_of(slots, [id](const auto &slot) {
            return slot.active && slot.record.id == id;
        });
    }

    template <typename Registration, typename Id>
    [[nodiscard]] bool HasPendingRegistrationId(const std::span<const NavigationDynamicPendingCommand> pending, const Id id) noexcept {
        return std::ranges::any_of(pending, [id](const auto &command) {
            return std::visit([id]<typename Value>(const Value &value) {
                using Type = std::remove_cvref_t<Value>;
                if constexpr (std::is_same_v<Type, Registration>)
                    return value.descriptor.id == id;
                return false;
            }, command);
        });
    }

    template <typename Handle>
    [[nodiscard]] bool HasPendingHandle(const std::span<const NavigationDynamicPendingCommand> pending, const Handle handle) noexcept {
        return std::ranges::any_of(pending, [handle](const auto &command) {
            return std::visit([handle](const auto &value) {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value.handle)>, Handle>)
                    return value.handle == handle;
                return false;
            }, command);
        });
    }

    template <typename Revision> [[nodiscard]] bool RevisionWillAdvance(const Revision revision) noexcept {
        return revision.IsValid() && revision.Value() != std::numeric_limits<std::uint64_t>::max();
    }

    template <typename Value>
    inline constexpr bool IsObstaclePendingValue = std::is_same_v<std::remove_cvref_t<Value>, PendingObstacleRegistration> ||
                                                   std::is_same_v<std::remove_cvref_t<Value>, PendingObstacleUpdate> ||
                                                   std::is_same_v<std::remove_cvref_t<Value>, PendingObstacleRemoval>;

    template <typename State, typename Value> decltype(auto) SlotsFor(State &state) noexcept {
        using Type = std::remove_cvref_t<Value>;
        static_assert(IsObstaclePendingValue<Type> || std::is_same_v<Type, PendingModifierRegistration> ||
                          std::is_same_v<Type, PendingModifierUpdate> || std::is_same_v<Type, PendingModifierRemoval>,
                      "unsupported dynamic registry command");
        if constexpr (IsObstaclePendingValue<Type>)
            return (state.obstacles);
        else
            return (state.modifiers);
    }

    template <typename Handle, typename Registration, typename Descriptor>
    [[nodiscard]] Result<Handle> StageRegistration(NavigationDynamicRegistryState &state, std::size_t &pendingCount, const bool shutdown,
                                                   const Descriptor &descriptor) {
        if (shutdown)
            return Failure<Handle>(NavigationErrors::DynamicRegistryShuttingDown);
        if (!descriptor.IsValid())
            return Failure<Handle>(NavigationErrors::DynamicRegistryInvalid);
        const auto pending = std::span{state.pending}.first(pendingCount);
        if (state.bound && !Matches(descriptor.provenance, state.binding))
            return Failure<Handle>(NavigationErrors::DynamicRegistryStale);
        auto &slots = SlotsFor<NavigationDynamicRegistryState, Registration>(state);
        if (HasActiveId(slots, descriptor.id) || HasPendingRegistrationId<Registration>(pending, descriptor.id))
            return Failure<Handle>(NavigationErrors::DynamicRegistryConflict);
        const auto registeredCount = std::ranges::count_if(slots, [](const auto &slot) {
            return slot.active || slot.reserved;
        });
        if (const auto maximumRecords =
                IsObstaclePendingValue<Registration> ? state.limits.maximumObstacles : state.limits.maximumModifiers;
            registeredCount >= maximumRecords || pendingCount >= state.limits.maximumPendingCommands)
            return Failure<Handle>(NavigationErrors::DynamicRegistryCapacityExceeded);
        const auto slotIndex = FindFreeSlot(slots);
        if (!slotIndex.has_value())
            return Failure<Handle>(NavigationErrors::DynamicRegistryCapacityExceeded);
        auto &slot = slots[*slotIndex];
        slot.reserved = true;
        const Handle handle{
            .world = descriptor.provenance.world,
            .slot = {.index = static_cast<std::uint32_t>(*slotIndex), .generation = slot.generation},
        };
        state.pending[pendingCount++] = Registration{.handle = handle, .descriptor = descriptor};
        return Result<Handle>::Success(handle);
    }

    template <typename Handle, typename Update, typename Descriptor>
    [[nodiscard]] Result<void> StageUpdate(NavigationDynamicRegistryState &state, std::size_t &pendingCount, const bool shutdown,
                                           const Handle handle, const NavigationDynamicRecordRevision expectedRevision,
                                           const Descriptor &descriptor) {
        if (shutdown)
            return Failure<void>(NavigationErrors::DynamicRegistryShuttingDown);
        if (!descriptor.IsValid() || !expectedRevision.IsValid())
            return Failure<void>(NavigationErrors::DynamicRegistryInvalid);
        const auto pending = std::span{state.pending}.first(pendingCount);
        auto &slots = SlotsFor<NavigationDynamicRegistryState, Update>(state);
        const auto index = ResolveSlot(slots, handle, state.binding);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        const auto &current = slots[index.Value()].record;
        if (current.revision != expectedRevision || descriptor.id != current.id ||
            !SameOwnerProvenance(descriptor.provenance, current.provenance))
            return Failure<void>(NavigationErrors::DynamicRegistryStale);
        if (descriptor.provenance.sourceRevision.Value() <= current.provenance.sourceRevision.Value())
            return Failure<void>(NavigationErrors::DynamicRegistryStale);
        if (!UpdateIntervalAllowed(current.lastUpdateTick, descriptor.updateTick, state.limits.minimumUpdateIntervalTicks))
            return Failure<void>(NavigationErrors::DynamicRegistryUpdateRateExceeded);
        if (const bool hasPending = HasPendingHandle(pending, handle); hasPending || pendingCount >= state.limits.maximumPendingCommands)
            return Failure<void>(hasPending ? NavigationErrors::DynamicRegistryConflict
                                            : NavigationErrors::DynamicRegistryCapacityExceeded);
        state.pending[pendingCount++] = Update{
            .handle = handle,
            .expectedRevision = expectedRevision,
            .descriptor = descriptor,
        };
        return Result<void>::Success();
    }

    template <typename Handle, typename Removal>
    [[nodiscard]] Result<void> StageRemoval(NavigationDynamicRegistryState &state, std::size_t &pendingCount, const bool shutdown,
                                            const Handle handle, const NavigationDynamicRecordRevision expectedRevision) {
        if (shutdown)
            return Failure<void>(NavigationErrors::DynamicRegistryShuttingDown);
        if (!expectedRevision.IsValid())
            return Failure<void>(NavigationErrors::DynamicRegistryInvalid);
        const auto pending = std::span{state.pending}.first(pendingCount);
        auto &slots = SlotsFor<NavigationDynamicRegistryState, Removal>(state);
        const auto index = ResolveSlot(slots, handle, state.binding);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        if (slots[index.Value()].record.revision != expectedRevision)
            return Failure<void>(NavigationErrors::DynamicRegistryStale);
        if (HasPendingHandle(pending, handle))
            return Failure<void>(NavigationErrors::DynamicRegistryConflict);
        if (pendingCount >= state.limits.maximumPendingCommands)
            return Failure<void>(NavigationErrors::DynamicRegistryCapacityExceeded);
        state.pending[pendingCount++] = Removal{.handle = handle, .expectedRevision = expectedRevision};
        return Result<void>::Success();
    }

    [[nodiscard]] inline bool CommandMatchesBinding(const NavigationDynamicPendingCommand &command,
                                                    const NavigationDynamicSceneBinding &binding, const std::uint64_t targetTick) noexcept {
        return std::visit([&](const auto &value) {
            if constexpr (requires { value.descriptor; })
                return Matches(value.descriptor.provenance, binding) && value.descriptor.updateTick <= targetTick;
            return value.handle.world == binding.world;
        }, command);
    }

    template <typename Record, typename Descriptor> void AssignCommonRecordFields(Record &record, const Descriptor &descriptor) noexcept {
        record.provenance = descriptor.provenance;
        record.shape = descriptor.shape;
        record.layers = descriptor.layers;
        record.priority = descriptor.priority;
        record.lastUpdateTick = descriptor.updateTick;
        record.enabled = descriptor.enabled;
    }

    template <typename Slot, typename Pending>
    void PopulateRegistryRecord(Slot &slot, const Pending &pending, const NavigationDynamicRecordRevision revision) noexcept {
        auto &record = slot.record;
        record = {};
        record.handle = pending.handle;
        record.id = pending.descriptor.id;
        AssignCommonRecordFields(record, pending.descriptor);
        record.revision = revision;
        if constexpr (requires { pending.descriptor.operation; }) {
            record.operation = pending.descriptor.operation;
            record.area = pending.descriptor.area;
            record.traversalCost = pending.descriptor.traversalCost;
        }
        slot.active = true;
        slot.reserved = false;
    }

    template <typename Slot, typename Pending> void UpdateRegistryRecord(Slot &slot, const Pending &pending) noexcept {
        auto &record = slot.record;
        AssignCommonRecordFields(record, pending.descriptor);
        record.revision = NavigationDynamicRecordRevision::Create(pending.expectedRevision.Value() + 1U).Value();
        if constexpr (requires { pending.descriptor.operation; }) {
            record.operation = pending.descriptor.operation;
            record.area = pending.descriptor.area;
            record.traversalCost = pending.descriptor.traversalCost;
        }
    }

    template <typename State, typename Value>
    void ApplyPendingCommand(State &state, const Value &value, NavigationDynamicCommitResult &result) noexcept {
        using Type = std::remove_cvref_t<Value>;
        auto &slot = SlotsFor<State, Type>(state)[value.handle.slot.index];
        if constexpr (std::is_same_v<Type, PendingObstacleRegistration> || std::is_same_v<Type, PendingModifierRegistration>) {
            PopulateRegistryRecord(slot, value, NavigationDynamicRecordRevision::Create(1).Value());
            ++result.registrations;
        } else if constexpr (std::is_same_v<Type, PendingObstacleUpdate> || std::is_same_v<Type, PendingModifierUpdate>) {
            UpdateRegistryRecord(slot, value);
            ++result.updates;
        } else {
            slot.active = false;
            slot.reserved = false;
            slot.record = {};
            if (slot.generation == std::numeric_limits<std::uint32_t>::max())
                slot.retired = true;
            else
                ++slot.generation;
            ++result.removals;
        }
    }

    template <typename State, typename Value> void ReleasePendingRegistration(State &state, const Value &value) noexcept {
        using Type = std::remove_cvref_t<Value>;
        if constexpr (std::is_same_v<Type, PendingObstacleRegistration> || std::is_same_v<Type, PendingModifierRegistration>) {
            auto &slots = SlotsFor<State, Type>(state);
            auto &slot = slots[value.handle.slot.index];
            if (slot.reserved && !slot.active)
                BurnReservedSlot(slots, value.handle.slot.index);
        }
    }

    template <typename SlotArray> void ClearActiveSlots(SlotArray &slots) noexcept {
        for (auto &slot : slots) {
            if (!slot.active)
                continue;
            slot.active = false;
            slot.reserved = false;
            slot.record = {};
            ++slot.generation;
        }
    }

    template <typename SlotArray> [[nodiscard]] bool HasActiveGenerationAtLimit(const SlotArray &slots) noexcept {
        return std::ranges::any_of(slots, [](const auto &slot) {
            return slot.active && slot.generation == std::numeric_limits<std::uint32_t>::max();
        });
    }

    [[nodiscard]] inline bool CommandRevisionWillAdvance(const NavigationDynamicPendingCommand &command) noexcept {
        return std::visit([]<typename Value>(const Value &value) {
            using Type = std::remove_cvref_t<Value>;
            if constexpr (std::is_same_v<Type, PendingObstacleUpdate> || std::is_same_v<Type, PendingModifierUpdate>)
                return RevisionWillAdvance(value.expectedRevision);
            return true;
        }, command);
    }

    template <typename Record> [[nodiscard]] bool RecordOrder(const Record &left, const Record &right) noexcept {
        if (left.priority != right.priority)
            return left.priority > right.priority;
        if (left.id != right.id)
            return left.id < right.id;
        return left.handle < right.handle;
    }

    template <typename Record, std::size_t Capacity> void SortRecords(std::array<Record, Capacity> &records, const std::size_t count) {
        std::ranges::sort(std::span{records}.first(count), RecordOrder<Record>);
    }

    template <typename Record>
    [[nodiscard]] std::span<const Record> SnapshotRecords(
        const std::shared_ptr<const NavigationDynamicRegistrySnapshotState> &records) noexcept {
        if (!records)
            return {};
        if constexpr (std::is_same_v<Record, NavigationObstacleRecord>)
            return std::span{records->obstacles}.first(records->obstacleCount);
        else
            return std::span{records->modifiers}.first(records->modifierCount);
    }

    template <typename Record, typename Handle>
    [[nodiscard]] Result<Record> FindSnapshotRecord(const std::span<const Record> records, const Handle handle) {
        if (!handle.IsValid())
            return Failure<Record>(NavigationErrors::InvalidHandle);
        const auto found = std::ranges::find_if(records, [handle](const auto &record) {
            return record.handle == handle;
        });
        if (found == records.end())
            return Failure<Record>(NavigationErrors::DynamicRegistryStale);
        return Result<Record>::Success(*found);
    }
}  // namespace Horo::Navigation::Detail
