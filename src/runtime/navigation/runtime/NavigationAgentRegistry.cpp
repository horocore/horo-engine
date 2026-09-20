#include "Horo/Navigation/NavigationAgentRegistry.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <new>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace Horo::Navigation {
    namespace Detail {
        struct NavigationAgentSlot final {
            std::uint32_t generation{1};
            bool retired{};
            std::optional<NavigationAgentRecord> record;
        };

        struct NavigationAgentOwnerHash final {
            [[nodiscard]] std::size_t operator()(const NavigationAgentOwner &owner) const noexcept {
                std::size_t result = std::hash<std::uint64_t>{}(owner.scene.Value());
                result ^=
                    std::hash<std::uint32_t>{}(owner.entityIndex) + static_cast<std::size_t>(0x9e3779b9U) + (result << 6U) + (result >> 2U);
                result ^= std::hash<std::uint32_t>{}(owner.entityGeneration) + static_cast<std::size_t>(0x9e3779b9U) + (result << 6U) +
                          (result >> 2U);
                return result;
            }
        };

        struct NavigationAgentRegistryState final {
            NavigationAgentSceneBinding binding;
            std::vector<NavigationAgentSlot> slots;
            std::vector<std::uint32_t> freeSlots;
            std::unordered_map<NavigationAgentOwner, std::uint32_t, NavigationAgentOwnerHash> ownerSlots;
            std::size_t activeCount{};
        };
    }  // namespace Detail

    namespace {
        [[nodiscard]] Result<void> InvalidDescriptor() {
            return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        }

        [[nodiscard]] Result<void> StaleRegistry() {
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        }

        [[nodiscard]] Result<void> ClosedRegistry() {
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryShuttingDown));
        }

        [[nodiscard]] Result<void> ValidateKnownMovementCapability(const NavigationAgentMovementCapability capability) {
            if (capability != NavigationAgentMovementCapability::Grounded)
                return InvalidDescriptor();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AdvanceSlotGeneration(Detail::NavigationAgentSlot &slot) {
            if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
                slot.retired = true;
                return Result<void>::Success();
            }
            ++slot.generation;
            return Result<void>::Success();
        }

        /** @brief Populates a detached registry state from validated, owner-sorted descriptors. */
        void PopulateCandidateState(Detail::NavigationAgentRegistryState &state, const NavigationAgentSceneBinding &binding,
                                    const std::vector<NavigationAgentDescriptor> &ordered, const std::size_t maximumAgents) {
            state.binding = binding;
            state.slots.resize(maximumAgents);
            state.freeSlots.reserve(maximumAgents);
            state.ownerSlots.reserve(maximumAgents);
            state.activeCount = ordered.size();
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                const NavigationAgentDescriptor &descriptor = ordered[index];
                state.ownerSlots.try_emplace(descriptor.owner, static_cast<std::uint32_t>(index));
                state.slots[index].record = NavigationAgentRecord{
                    .handle = CrowdAgentHandle{binding.world, Horo::Handle<CrowdAgentHandleTag>{static_cast<std::uint32_t>(index), 1}},
                    .owner = descriptor.owner,
                    .profile = descriptor.profile,
                    .filter = descriptor.filter,
                    .radiusOverride = descriptor.radiusOverride,
                    .movementCapability = descriptor.movementCapability,
                    .enabled = descriptor.enabled,
                };
            }
            for (std::size_t index = maximumAgents; index > ordered.size(); --index)
                state.freeSlots.push_back(static_cast<std::uint32_t>(index - 1));
        }

    }  // namespace

    /** @copydoc ValidateNavigationAgentDescriptor */
    Result<void> ValidateNavigationAgentDescriptor(const NavigationAgentDescriptor &descriptor) {
        if (!descriptor.owner.IsValid() || !descriptor.profile.IsValid() || !descriptor.filter.IsValid())
            return InvalidDescriptor();
        if (descriptor.radiusOverride.has_value() && (!std::isfinite(*descriptor.radiusOverride) || *descriptor.radiusOverride <= 0.0F))
            return InvalidDescriptor();
        return ValidateKnownMovementCapability(descriptor.movementCapability);
    }

    /** @copydoc NavigationAgentSnapshot::Find */
    Result<NavigationAgentRecord> NavigationAgentSnapshot::Find(const CrowdAgentHandle handle) const {
        if (!IsValid() || ValidateNavigationHandleOwner(handle, Binding().world).HasError())
            return Result<NavigationAgentRecord>::Failure(MakeError(NavigationErrors::InvalidHandle));
        const auto found = std::ranges::find(agents_, handle, &NavigationAgentRecord::handle);
        if (found == agents_.end())
            return Result<NavigationAgentRecord>::Failure(MakeError(NavigationErrors::InvalidHandle));
        return Result<NavigationAgentRecord>::Success(*found);
    }

    /** @copydoc NavigationAgentSceneCandidate::NavigationAgentSceneCandidate */
    NavigationAgentSceneCandidate::NavigationAgentSceneCandidate(NavigationAgentRegistry &registry,
                                                                 const NavigationAgentSceneBinding binding,
                                                                 std::unique_ptr<Detail::NavigationAgentRegistryState> state,
                                                                 const std::uint64_t publicationToken) noexcept
        : registry_(&registry), binding_(binding), state_(std::move(state)), publicationToken_(publicationToken) {}

    NavigationAgentSceneCandidate::~NavigationAgentSceneCandidate() {
        Shutdown();
    }

    /** @copydoc NavigationAgentSceneCandidate::ValidatePublication */
    Result<void> NavigationAgentSceneCandidate::ValidatePublication() const {
        if (!registry_ || !state_ || state_->binding != binding_ || published_)
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        return registry_->ValidateCandidateBinding(binding_);
    }

    /** @copydoc NavigationAgentSceneCandidate::Publish */
    void NavigationAgentSceneCandidate::Publish() noexcept {
        if (!registry_ || published_ || !state_)
            return;
        registry_->PublishCandidate(std::move(state_), binding_, publicationToken_);
        published_ = true;
    }

    /** @copydoc NavigationAgentSceneCandidate::Shutdown */
    void NavigationAgentSceneCandidate::Shutdown() noexcept {
        if (!registry_)
            return;
        if (published_)
            registry_->RetirePublication(publicationToken_);
        state_.reset();
        registry_ = nullptr;
        published_ = false;
    }

    /** @copydoc NavigationAgentRegistry::Create */
    Result<NavigationAgentRegistry> NavigationAgentRegistry::Create(const NavigationAgentRegistryLimits &limits) {
        if (!limits.IsValid())
            return Result<NavigationAgentRegistry>::Failure(MakeError(NavigationErrors::AgentRegistryCapacityExceeded));
        return Result<NavigationAgentRegistry>::Success(NavigationAgentRegistry{limits});
    }

    NavigationAgentRegistry::NavigationAgentRegistry(const NavigationAgentRegistryLimits limits) noexcept : limits_(limits) {}

    NavigationAgentRegistry::NavigationAgentRegistry(NavigationAgentRegistry &&other) noexcept
        : limits_(other.limits_), active_(std::move(other.active_)), nextPublicationToken_(other.nextPublicationToken_),
          activePublicationToken_(other.activePublicationToken_), shutdown_(other.shutdown_) {
        other.shutdown_ = true;
        other.activePublicationToken_ = 0;
    }

    NavigationAgentRegistry::~NavigationAgentRegistry() {
        BeginShutdown();
    }

    /** @brief Checks candidate binding and rejects reuse of a live world incarnation. */
    Result<void> NavigationAgentRegistry::ValidateCandidateBinding(const NavigationAgentSceneBinding &binding) const {
        if (shutdown_)
            return ClosedRegistry();
        if (!binding.IsValid())
            return StaleRegistry();
        if (active_ && active_->binding.world == binding.world)
            return StaleRegistry();
        return Result<void>::Success();
    }

    /** @copydoc NavigationAgentRegistry::PrepareScene */
    Result<std::unique_ptr<NavigationAgentSceneCandidate>> NavigationAgentRegistry::PrepareScene(
        NavigationAgentSceneBinding binding, const std::span<const NavigationAgentDescriptor> descriptors) {
        if (const Result<void> valid = ValidateCandidateBinding(binding); valid.HasError())
            return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(valid.ErrorValue());
        if (descriptors.size() > limits_.maximumAgents)
            return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(
                MakeError(NavigationErrors::AgentRegistryCapacityExceeded));

        try {
            std::vector<NavigationAgentDescriptor> ordered;
            ordered.assign(descriptors.begin(), descriptors.end());
            for (const NavigationAgentDescriptor &descriptor : ordered) {
                if (const Result<void> valid = ValidateNavigationAgentDescriptor(descriptor); valid.HasError())
                    return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(valid.ErrorValue());
                if (descriptor.owner.scene != binding.scene)
                    return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
            }
            std::ranges::sort(ordered, {}, &NavigationAgentDescriptor::owner);
            if (std::ranges::adjacent_find(ordered, {}, &NavigationAgentDescriptor::owner) != ordered.end())
                return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(MakeError(NavigationErrors::AgentRegistryConflict));

            auto state = std::make_unique<Detail::NavigationAgentRegistryState>();
            PopulateCandidateState(*state, binding, ordered, limits_.maximumAgents);
            if (nextPublicationToken_ == std::numeric_limits<std::uint64_t>::max())
                return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(MakeError(NavigationErrors::GenerationExhausted));
            const std::uint64_t token = nextPublicationToken_++;
            return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Success(std::unique_ptr<NavigationAgentSceneCandidate>(
                new NavigationAgentSceneCandidate(*this, binding, std::move(state),
                                                  token)));  // NOSONAR(cpp:S5950) Registry-private constructor cannot use make_unique.
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<NavigationAgentSceneCandidate>>::Failure(
                MakeError(NavigationErrors::AgentRegistryCapacityExceeded));
        }
    }

    /** @brief Copies a descriptor into a free active slot without allocating. */
    Result<CrowdAgentHandle> NavigationAgentRegistry::RegisterAtSafePoint(const NavigationAgentDescriptor &descriptor) {
        if (shutdown_)
            return Result<CrowdAgentHandle>::Failure(MakeError(NavigationErrors::AgentRegistryShuttingDown));
        if (const Result<void> valid = ValidateNavigationAgentDescriptor(descriptor); valid.HasError())
            return Result<CrowdAgentHandle>::Failure(valid.ErrorValue());
        if (!active_ || descriptor.owner.scene != active_->binding.scene)
            return Result<CrowdAgentHandle>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        if (active_->ownerSlots.contains(descriptor.owner))
            return Result<CrowdAgentHandle>::Failure(MakeError(NavigationErrors::AgentRegistryConflict));
        if (active_->freeSlots.empty())
            return Result<CrowdAgentHandle>::Failure(MakeError(NavigationErrors::AgentRegistryCapacityExceeded));

        const std::uint32_t index = active_->freeSlots.back();
        try {
            const auto [_, inserted] = active_->ownerSlots.try_emplace(descriptor.owner, index);
            if (!inserted)
                return Result<CrowdAgentHandle>::Failure(MakeError(NavigationErrors::AgentRegistryConflict));
        } catch (const std::bad_alloc &) {
            return Result<CrowdAgentHandle>::Failure(MakeError(NavigationErrors::AgentRegistryCapacityExceeded));
        }
        active_->freeSlots.pop_back();
        Detail::NavigationAgentSlot &slot = active_->slots[index];
        slot.record = NavigationAgentRecord{
            .handle = CrowdAgentHandle{active_->binding.world, Horo::Handle<CrowdAgentHandleTag>{index, slot.generation}},
            .owner = descriptor.owner,
            .profile = descriptor.profile,
            .filter = descriptor.filter,
            .radiusOverride = descriptor.radiusOverride,
            .movementCapability = descriptor.movementCapability,
            .enabled = descriptor.enabled,
        };
        ++active_->activeCount;
        return Result<CrowdAgentHandle>::Success(slot.record->handle);
    }

    /** @copydoc NavigationAgentRegistry::UnregisterAtSafePoint */
    Result<void> NavigationAgentRegistry::UnregisterAtSafePoint(const CrowdAgentHandle handle) {
        if (shutdown_)
            return ClosedRegistry();
        if (!active_ || ValidateNavigationHandleOwner(handle, active_->binding.world).HasError() ||
            handle.slot.index >= active_->slots.size())
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidHandle));
        Detail::NavigationAgentSlot &slot = active_->slots[handle.slot.index];
        if (!slot.record || slot.generation != handle.slot.generation)
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidHandle));
        active_->ownerSlots.erase(slot.record->owner);
        slot.record.reset();
        --active_->activeCount;
        if (const Result<void> advanced = AdvanceSlotGeneration(slot); advanced.HasError())
            return advanced;
        if (!slot.retired)
            active_->freeSlots.push_back(handle.slot.index);
        return Result<void>::Success();
    }

    /** @copydoc NavigationAgentRegistry::UnregisterOwnerAtSafePoint */
    Result<std::size_t> NavigationAgentRegistry::UnregisterOwnerAtSafePoint(const NavigationAgentOwner owner) {
        if (shutdown_)
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::AgentRegistryShuttingDown));
        if (!owner.IsValid() || !active_ || owner.scene != active_->binding.scene)
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        const auto found = active_->ownerSlots.find(owner);
        if (found == active_->ownerSlots.end())
            return Result<std::size_t>::Success(0);
        const std::uint32_t index = found->second;
        if (index >= active_->slots.size() || !active_->slots[index].record)
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        const CrowdAgentHandle handle = active_->slots[index].record->handle;
        if (const Result<void> removed = UnregisterAtSafePoint(handle); removed.HasError())
            return Result<std::size_t>::Failure(removed.ErrorValue());
        return Result<std::size_t>::Success(1);
    }

    /** @copydoc NavigationAgentRegistry::Snapshot */
    Result<NavigationAgentSnapshot> NavigationAgentRegistry::Snapshot() const {
        if (shutdown_)
            return Result<NavigationAgentSnapshot>::Failure(MakeError(NavigationErrors::AgentRegistryShuttingDown));
        if (!active_)
            return Result<NavigationAgentSnapshot>::Failure(MakeError(NavigationErrors::NoNavigationData));
        NavigationAgentSnapshot snapshot;
        snapshot.binding_ = active_->binding;
        try {
            snapshot.agents_.reserve(active_->activeCount);
            for (const Detail::NavigationAgentSlot &slot : active_->slots)
                if (slot.record)
                    snapshot.agents_.push_back(*slot.record);
        } catch (const std::bad_alloc &) {
            return Result<NavigationAgentSnapshot>::Failure(MakeError(NavigationErrors::AgentRegistryCapacityExceeded));
        }
        return Result<NavigationAgentSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc NavigationAgentRegistry::Find */
    Result<NavigationAgentRecord> NavigationAgentRegistry::Find(const CrowdAgentHandle handle) const {
        if (shutdown_)
            return Result<NavigationAgentRecord>::Failure(MakeError(NavigationErrors::AgentRegistryShuttingDown));
        if (!active_ || ValidateNavigationHandleOwner(handle, active_->binding.world).HasError() ||
            handle.slot.index >= active_->slots.size())
            return Result<NavigationAgentRecord>::Failure(MakeError(NavigationErrors::InvalidHandle));
        const Detail::NavigationAgentSlot &slot = active_->slots[handle.slot.index];
        if (!slot.record || slot.generation != handle.slot.generation)
            return Result<NavigationAgentRecord>::Failure(MakeError(NavigationErrors::InvalidHandle));
        return Result<NavigationAgentRecord>::Success(*slot.record);
    }

    /** @copydoc NavigationAgentRegistry::ActiveBinding */
    Result<NavigationAgentSceneBinding> NavigationAgentRegistry::ActiveBinding() const {
        if (shutdown_)
            return Result<NavigationAgentSceneBinding>::Failure(MakeError(NavigationErrors::AgentRegistryShuttingDown));
        if (!active_)
            return Result<NavigationAgentSceneBinding>::Failure(MakeError(NavigationErrors::NoNavigationData));
        return Result<NavigationAgentSceneBinding>::Success(active_->binding);
    }

    /** @brief Installs a prepared state with no allocation after every participant has validated. */
    void NavigationAgentRegistry::PublishCandidate(std::unique_ptr<Detail::NavigationAgentRegistryState> state,
                                                   const NavigationAgentSceneBinding binding,
                                                   const std::uint64_t publicationToken) noexcept {
        if (shutdown_ || !state || state->binding != binding)
            return;
        active_ = std::move(state);
        activePublicationToken_ = publicationToken;
    }

    /** @brief Removes the exact published state when its owning Scene candidate retires. */
    void NavigationAgentRegistry::RetirePublication(const std::uint64_t publicationToken) noexcept {
        if (activePublicationToken_ != publicationToken)
            return;
        active_.reset();
        activePublicationToken_ = 0;
    }

    /** @copydoc NavigationAgentRegistry::BeginShutdown */
    void NavigationAgentRegistry::BeginShutdown() noexcept {
        if (shutdown_)
            return;
        shutdown_ = true;
        active_.reset();
        activePublicationToken_ = 0;
    }
}  // namespace Horo::Navigation
