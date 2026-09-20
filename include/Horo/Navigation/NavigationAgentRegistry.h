#pragma once

/**
 * @file NavigationAgentRegistry.h
 * @brief Scene-bound, generation-safe logical navigation-agent registration.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavigationAgentProfiles.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Navigation {
    namespace Detail {
        struct NavigationAgentRegistryState;
    }

    class NavigationAgentRegistry;

    inline constexpr std::size_t MaximumNavigationAgentRegistrySlots = 65'536;

    /** @brief Exact runtime entity generation that owns one logical navigation agent. */
    struct NavigationAgentOwner final {
        NavigationSceneRuntimeId scene;
        std::uint32_t entityIndex{};
        std::uint32_t entityGeneration{};

        /** @brief Checks that the owner is bound to a runtime Scene and a live entity generation. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return scene.IsValid() && entityGeneration != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const NavigationAgentOwner &) const noexcept = default;
    };

    /** @brief Exact Scene/world binding retained by one detached or published agent candidate. */
    struct NavigationAgentSceneBinding final {
        NavigationWorldId world;
        NavigationSceneRuntimeId scene;
        NavigationSceneGeneration sceneGeneration;

        /** @brief Checks every binding identity before it can be published. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return world.IsValid() && scene.IsValid() && sceneGeneration.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const NavigationAgentSceneBinding &) const noexcept = default;
    };

    /** @brief Provider-neutral authored/runtime values needed to create one logical navigation agent. */
    struct NavigationAgentDescriptor final {
        NavigationAgentOwner owner;
        NavigationAgentProfileId profile;
        NavigationFilterId filter;
        std::optional<float> radiusOverride;
        NavigationAgentMovementCapability movementCapability{NavigationAgentMovementCapability::Grounded};
        bool enabled{true};

        [[nodiscard]] constexpr auto operator<=>(const NavigationAgentDescriptor &) const noexcept = default;
    };

    /**
     * @brief Validates one provider-neutral navigation-agent descriptor without resolving a provider or Scene state.
     * @param descriptor Candidate owner, profile, filter, radius, and movement capability.
     * @return Success or NavigationErrors::AgentDescriptorInvalid.
     */
    [[nodiscard]] Result<void> ValidateNavigationAgentDescriptor(const NavigationAgentDescriptor &descriptor);

    /** @brief Immutable registered navigation-agent record. */
    struct NavigationAgentRecord final {
        CrowdAgentHandle handle;
        NavigationAgentOwner owner;
        NavigationAgentProfileId profile;
        NavigationFilterId filter;
        std::optional<float> radiusOverride;
        NavigationAgentMovementCapability movementCapability{NavigationAgentMovementCapability::Grounded};
        bool enabled{true};

        [[nodiscard]] constexpr auto operator<=>(const NavigationAgentRecord &) const noexcept = default;
    };

    /** @brief Bounded policy for one logical Scene agent registry. */
    struct NavigationAgentRegistryLimits final {
        std::size_t maximumAgents{MaximumNavigationAgentRegistrySlots};

        /** @brief Validates the configured slot bound. @return True when the bound is within the hard ceiling. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumAgents > 0 && maximumAgents <= MaximumNavigationAgentRegistrySlots;
        }
    };

    /** @brief Immutable value snapshot safe to retain while a Scene/world candidate is replaced. */
    class NavigationAgentSnapshot final {
    public:
        /** @brief Constructs an empty invalid snapshot. */
        NavigationAgentSnapshot() noexcept = default;

        /** @brief Reports whether the snapshot has an exact Scene/world binding. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return binding_.IsValid();
        }

        /** @brief Returns the exact Scene/world binding captured by this snapshot. */
        [[nodiscard]] constexpr const NavigationAgentSceneBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns every registered record in stable slot order. */
        [[nodiscard]] constexpr std::span<const NavigationAgentRecord> Agents() const noexcept {
            return agents_;
        }

        /**
         * @brief Resolves one exact handle against this immutable publication.
         * @param handle Generation-safe crowd-agent handle.
         * @return Matching record or NavigationErrors::InvalidHandle.
         */
        [[nodiscard]] Result<NavigationAgentRecord> Find(CrowdAgentHandle handle) const;

    private:
        friend class NavigationAgentRegistry;
        NavigationAgentSceneBinding binding_;
        std::vector<NavigationAgentRecord> agents_;
    };

    /** @brief Detached Scene candidate owned until aggregate publication or rollback. */
    class NavigationAgentSceneCandidate final {
    public:
        /** @brief Registry-only capability required to construct a detached candidate. */
        class CreationKey final {
        private:
            friend class NavigationAgentRegistry;
            constexpr CreationKey() noexcept = default;
        };

        /**
         * @brief Creates a detached candidate with a registry-authorized creation key.
         * @param key Registry-only capability proving that the owning registry authorized construction.
         * @param registry Owning registry that will validate and publish the candidate.
         * @param binding Exact world, runtime Scene, and Scene-generation fence.
         * @param state Fully prepared detached registry state.
         * @param publicationToken Registry publication generation assigned to this candidate.
         */
        NavigationAgentSceneCandidate(CreationKey key, NavigationAgentRegistry &registry, NavigationAgentSceneBinding binding,
                                      std::unique_ptr<Detail::NavigationAgentRegistryState> state, std::uint64_t publicationToken) noexcept;
        NavigationAgentSceneCandidate(const NavigationAgentSceneCandidate &) = delete;
        NavigationAgentSceneCandidate &operator=(const NavigationAgentSceneCandidate &) = delete;
        ~NavigationAgentSceneCandidate();

        /** @brief Revalidates registry admission before aggregate Scene publication. */
        [[nodiscard]] Result<void> ValidatePublication() const;
        /** @brief Publishes the fully prepared candidate without allocation. */
        void Publish() noexcept;
        /** @brief Rolls back or retires this candidate; safe before or after publication. */
        void Shutdown() noexcept;

        /** @brief Returns the exact candidate Scene/world binding. */
        [[nodiscard]] constexpr const NavigationAgentSceneBinding &Binding() const noexcept {
            return binding_;
        }

    private:
        NavigationAgentRegistry *registry_{};
        NavigationAgentSceneBinding binding_;
        std::unique_ptr<Detail::NavigationAgentRegistryState> state_;
        std::uint64_t publicationToken_{};
        bool published_{};
    };

    /**
     * @brief Owner-thread registry for logical agents bound to one exact Scene/world generation.
     * @details Scene activation prepares a detached complete population and publishes it only after all Scene participants
     * validate. Individual registration/removal is restricted to the owner safe point and every handle carries the exact
     * world, slot, and slot generation needed to reject stale entity state.
     */
    class NavigationAgentRegistry final {
    public:
        /**
         * @brief Creates an empty registry with bounded slot storage.
         * @param limits Product bound that may not exceed the compile-time ceiling.
         * @return Registry or a typed capacity error.
         */
        [[nodiscard]] static Result<NavigationAgentRegistry> Create(const NavigationAgentRegistryLimits &limits = {});

        NavigationAgentRegistry(const NavigationAgentRegistry &) = delete;
        NavigationAgentRegistry &operator=(const NavigationAgentRegistry &) = delete;
        NavigationAgentRegistry(NavigationAgentRegistry &&) noexcept;
        NavigationAgentRegistry &operator=(NavigationAgentRegistry &&) = delete;
        ~NavigationAgentRegistry();

        /**
         * @brief Prepares a complete detached population for one Scene activation.
         * @param binding Exact world, runtime Scene, and Scene-generation fence.
         * @param descriptors Enabled agent descriptors in any order.
         * @return Candidate or a typed invalid, conflict, capacity, stale, or shutdown error; active state is unchanged.
         */
        [[nodiscard]] Result<std::unique_ptr<NavigationAgentSceneCandidate>> PrepareScene(
            NavigationAgentSceneBinding binding, std::span<const NavigationAgentDescriptor> descriptors);

        /**
         * @brief Registers one agent in the currently published Scene at its safe point.
         * @param descriptor Complete descriptor bound to the active Scene.
         * @return Generation-safe handle or a typed invalid/stale/capacity/shutdown error.
         */
        [[nodiscard]] Result<CrowdAgentHandle> RegisterAtSafePoint(const NavigationAgentDescriptor &descriptor);

        /**
         * @brief Removes one exact registered generation at its safe point.
         * @param handle Handle issued by this registry.
         * @return Success or a typed malformed, foreign, stale, or shutdown error without mutation on failure.
         */
        [[nodiscard]] Result<void> UnregisterAtSafePoint(CrowdAgentHandle handle);

        /**
         * @brief Removes the exact entity generation's agent, if present.
         * @param owner Runtime Scene and entity generation being disabled or destroyed.
         * @return Number removed, or a typed invalid/stale/shutdown error.
         */
        [[nodiscard]] Result<std::size_t> UnregisterOwnerAtSafePoint(NavigationAgentOwner owner);

        /** @brief Captures an immutable value snapshot of the published agent population. */
        [[nodiscard]] Result<NavigationAgentSnapshot> Snapshot() const;
        /** @brief Resolves one exact handle against the published population. */
        [[nodiscard]] Result<NavigationAgentRecord> Find(CrowdAgentHandle handle) const;
        /** @brief Returns the exact active Scene/world binding. */
        [[nodiscard]] Result<NavigationAgentSceneBinding> ActiveBinding() const;

        /** @brief Closes registration admission and releases active state; retained value snapshots remain valid. */
        void BeginShutdown() noexcept;

        /** @brief Reports whether registration admission is closed. */
        [[nodiscard]] constexpr bool IsShutdown() const noexcept {
            return shutdown_;
        }

    private:
        friend class NavigationAgentSceneCandidate;

        explicit NavigationAgentRegistry(NavigationAgentRegistryLimits limits) noexcept;

        [[nodiscard]] Result<void> ValidateCandidateBinding(const NavigationAgentSceneBinding &binding) const;
        void PublishCandidate(std::unique_ptr<Detail::NavigationAgentRegistryState> state, NavigationAgentSceneBinding binding,
                              std::uint64_t publicationToken) noexcept;
        void RetirePublication(std::uint64_t publicationToken) noexcept;

        NavigationAgentRegistryLimits limits_;
        std::unique_ptr<Detail::NavigationAgentRegistryState> active_;
        std::uint64_t nextPublicationToken_{1};
        std::uint64_t activePublicationToken_{};
        bool shutdown_{};
    };
}  // namespace Horo::Navigation
