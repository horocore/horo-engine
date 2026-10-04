#pragma once

/**
 * @file AISceneActivation.h
 * @brief Transactional SceneRuntime activation and lifecycle ownership for typed AI components.
 */

#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/AITaskLifecycle.h"
#include "Horo/AI/BlackboardInstance.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::AI {
    class AiSceneRuntime;

    inline constexpr std::size_t MaximumAiSceneAgents = 65'536;

    /** @brief Exact AI and RuntimeScene incarnation used by one detached activation candidate. */
    struct AiSceneActivationBinding final {
        AiRuntimeIncarnation incarnation;
        Runtime::SceneRuntimeId scene;

        /** @brief Checks that the AI and Scene identities are both usable. @return True for a valid binding. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return incarnation.IsValid() && scene.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const AiSceneActivationBinding &) const noexcept = default;
    };

    /** @brief Complete typed AI payload associated with one exact runtime entity generation. */
    struct AiSceneAgentDescriptor final {
        Runtime::EntityRef owner;
        AiAgentComponent agent;
        std::optional<AiControllerComponent> controller;
    };

    /** @brief Lifecycle state of an admitted scene-owned AI agent. */
    enum class AiAgentActivationState : std::uint8_t {
        Active,
        Disabled,
        Retired,
        Count,
    };

    /** @brief Immutable inspection record for one active or recently disabled AI agent slot. */
    struct AiAgentRuntimeRecord final {
        AgentHandle handle;
        Runtime::EntityRef owner;
        AiAgentComponent agent;
        std::optional<AiControllerComponent> controller;
        AiCapabilitySet stagedCapabilities;
        AiAgentActivationState state{AiAgentActivationState::Active};
        bool hasBlackboard{};
        bool hasRunningTask{};
    };

    /** @brief Immutable value snapshot of one AI scene publication. */
    class AiSceneSnapshot final {
    public:
        /** @brief Reports whether this snapshot is bound to a scene incarnation. @return True for a valid snapshot. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return binding_.IsValid();
        }

        /** @brief Returns the exact activation binding. @return Immutable AI/Scene identity pair. */
        [[nodiscard]] constexpr const AiSceneActivationBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns the retained agent records in stable slot order. @return Borrowed record span. */
        [[nodiscard]] constexpr std::span<const AiAgentRuntimeRecord> Agents() const noexcept {
            return agents_;
        }

        /**
         * @brief Resolves one exact agent handle against this publication.
         * @param handle Generation-fenced agent handle.
         * @return Matching record or AIErrors::HandleInvalid.
         */
        [[nodiscard]] Result<AiAgentRuntimeRecord> Find(AgentHandle handle) const;

    private:
        friend class AiSceneRuntime;
        AiSceneActivationBinding binding_;
        std::vector<AiAgentRuntimeRecord> agents_;
        std::vector<std::optional<std::size_t>> slotLookup_; /**< Snapshot slot index to compact record index. */
    };

    namespace Detail {
        struct AiSceneRuntimeState;
        class AiSceneStructuralParticipant;
        struct AiSceneRestoreState;
    }  // namespace Detail

    /** @brief Detached complete restore transaction; destruction rolls back without touching live or authored state. */
    class AiSceneRestoreCandidate final {
    public:
        /** @brief Runtime-only construction token; consumers cannot manufacture a candidate. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class AiSceneRuntime;
        };

        /**
         * @brief Takes ownership of runtime-validated detached restore state.
         * @param key Unforgeable token issued only by the runtime preparation boundary.
         * @param state Complete detached restore state owned until commit or rollback.
         */
        AiSceneRestoreCandidate(const ConstructionKey &key, std::unique_ptr<Detail::AiSceneRestoreState> state) noexcept;
        ~AiSceneRestoreCandidate();
        AiSceneRestoreCandidate(const AiSceneRestoreCandidate &) = delete;
        AiSceneRestoreCandidate &operator=(const AiSceneRestoreCandidate &) = delete;

    private:
        friend class AiSceneRuntime;
        std::unique_ptr<Detail::AiSceneRestoreState> state_;
    };

    /**
     * @brief Detached, transactional AI scene state prepared for aggregate RuntimeScene publication.
     * @details The candidate owns every blackboard and lifecycle object until publication or rollback. Publish is no-fail.
     */
    class AiSceneActivationCandidate final : public Runtime::SceneActivationCandidate {
    public:
        ~AiSceneActivationCandidate() override;

        /**
         * @brief Takes ownership of detached runtime state prepared for scene publication.
         * @param runtime Runtime owner that will publish or roll back the state.
         * @param binding Exact AI/Scene identity pair associated with the state.
         * @param state Detached state owned until publication or shutdown.
         * @param publicationToken Monotonic token fencing this candidate's publication.
         */
        AiSceneActivationCandidate(AiSceneRuntime &runtime, AiSceneActivationBinding binding,
                                   std::shared_ptr<Detail::AiSceneRuntimeState> state, std::uint64_t publicationToken) noexcept;

        AiSceneActivationCandidate(const AiSceneActivationCandidate &) = delete;
        AiSceneActivationCandidate &operator=(const AiSceneActivationCandidate &) = delete;

        /** @copydoc Runtime::SceneActivationCandidate::ValidatePublication */
        [[nodiscard]] Result<void> ValidatePublication() const override;
        /** @copydoc Runtime::SceneActivationCandidate::Publish */
        void Publish() noexcept override;
        /** @copydoc Runtime::SceneActivationCandidate::Shutdown */
        void Shutdown() noexcept override;

        /** @brief Returns the exact AI/Scene binding staged by this candidate. @return Immutable binding. */
        [[nodiscard]] constexpr const AiSceneActivationBinding &Binding() const noexcept {
            return binding_;
        }

    private:
        AiSceneRuntime *runtime_{};
        AiSceneActivationBinding binding_;
        std::shared_ptr<Detail::AiSceneRuntimeState> state_;
        std::uint64_t publicationToken_{};
        bool published_{};
    };

    /** @brief Bounded host policy and staged capability evidence for one AI scene runtime. */
    struct AiSceneRuntimeSettings final {
        std::size_t maximumAgents{MaximumAiSceneAgents};
        AiCapabilitySet availableCapabilities{AiCapabilitySet::Of(AiCapability::Behavior)
                                                  .Union(AiCapabilitySet::Of(AiCapability::Navigation))
                                                  .Union(AiCapabilitySet::Of(AiCapability::Perception))};

        /** @brief Checks product limits and closed capability bits. @return True when the settings are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumAgents > 0 && maximumAgents <= MaximumAiSceneAgents && availableCapabilities.IsValid();
        }
    };

    /**
     * @brief Owner-thread AI runtime bound to one exact RuntimeScene publication.
     * @details All active state is detached before publication. Handles include an AI incarnation and never outlive replacement,
     * destruction, or shutdown. Disable retires owned task work before clearing blackboard and capability state.
     */
    class AiSceneRuntime final {
    public:
        /**
         * @brief Creates an empty bounded AI scene runtime.
         * @param settings Host capability and capacity policy.
         * @return Runtime or AIErrors::AgentCapacityExceeded for invalid settings.
         */
        [[nodiscard]] static Result<AiSceneRuntime> Create(AiSceneRuntimeSettings settings = {});

        AiSceneRuntime(const AiSceneRuntime &) = delete;
        AiSceneRuntime &operator=(const AiSceneRuntime &) = delete;
        AiSceneRuntime(AiSceneRuntime &&) noexcept;
        AiSceneRuntime &operator=(AiSceneRuntime &&) = delete;
        ~AiSceneRuntime();

        /**
         * @brief Prepares the complete AI population for one RuntimeScene activation.
         * @param binding Exact AI and RuntimeScene incarnation pair.
         * @param agents Typed entity-owned component values.
         * @param descriptors Immutable controller descriptor catalog.
         * @return Detached candidate; active state is unchanged on any failure.
         */
        [[nodiscard]] Result<std::unique_ptr<AiSceneActivationCandidate>> PrepareScene(AiSceneActivationBinding binding,
                                                                                       std::span<const AiSceneAgentDescriptor> agents,
                                                                                       std::span<const AiControllerDescriptor> descriptors);

        /**
         * @brief Captures the only canonical AI population at the aggregate snapshot safe point.
         * @param scene Current Scene borrow used to revalidate every owner generation and entity-valued key.
         * @return Complete stable-identity-sorted state or typed lifecycle/storage failure.
         * @pre Called on the simulation owner; the Scene remains alive for the call.
         */
        [[nodiscard]] Result<AiCanonicalState> CaptureCanonicalState(Runtime::RuntimeSceneView scene) const;
        /**
         * @brief Validates and stages a complete population restore without changing live or authored state.
         * @param scene Current destination Scene borrow; its owner must outlive staging through commit or rollback.
         * @param expectedBinding Exact caller-selected destination AI/Scene incarnation.
         * @param state Complete canonical source population matching the destination authored agents/controllers.
         * @param migrations Bounded inert direct schema migration catalog, borrowed only during preparation.
         * @param cancellation Cooperative cancellation serviced during staging and again before publication.
         * @return Detached owned transaction or typed malformed, stale, unsupported, migration, or cancellation diagnostic.
         * @pre Simulation owner thread; restore is load-time work and may allocate bounded staging storage.
         */
        [[nodiscard]] Result<std::unique_ptr<AiSceneRestoreCandidate>> PrepareRestoreAtSafePoint(
            Runtime::RuntimeSceneView scene, AiSceneActivationBinding expectedBinding, const AiCanonicalState &state,
            std::span<const BlackboardSchemaMigration> migrations = {}, CancellationToken cancellation = {}) const;
        /**
         * @brief Revalidates all destination fences then atomically restores canonical state and retires transient work.
         * @param candidate Transaction prepared by this runtime; consumed on success or failure.
         * @return Success or typed stale/cancelled failure with all prior state and work preserved.
         * @pre Simulation owner at the aggregate restore safe point; the borrowed Scene is still alive.
         * @post Success cancels old tasks and observers before releasing their storage; asynchronous work restarts separately.
         */
        [[nodiscard]] Result<void> CommitRestoreAtSafePoint(std::unique_ptr<AiSceneRestoreCandidate> candidate);

        /**
         * @brief Starts one owned task at the AI decision safe point.
         * @param agent Exact active agent handle.
         * @param taskDefinition Persistent task identity.
         * @return Generation-fenced task handle or a typed lifecycle/capacity error.
         */
        [[nodiscard]] Result<TaskHandle> StartTaskAtSafePoint(AgentHandle agent, TaskId taskDefinition);

        /**
         * @brief Disables one agent, cancelling its task before revoking its capabilities and blackboard.
         * @param agent Exact active agent handle.
         * @return Success or a typed malformed/stale/shutdown error.
         */
        [[nodiscard]] Result<void> DisableAtSafePoint(AgentHandle agent);

        /**
         * @brief Retires the agent owned by one destroyed runtime entity generation.
         * @param owner Exact RuntimeScene entity reference.
         * @return Number retired or a typed malformed/stale/shutdown error.
         */
        [[nodiscard]] Result<std::size_t> RetireOwnerAtSafePoint(Runtime::EntityRef owner);

        /** @brief Captures an immutable value snapshot of the active publication. */
        [[nodiscard]] Result<AiSceneSnapshot> Snapshot() const;
        /** @brief Resolves one exact handle against the active publication. */
        [[nodiscard]] Result<AiAgentRuntimeRecord> Find(AgentHandle agent) const;
        /** @brief Returns the exact active AI/Scene binding. */
        [[nodiscard]] Result<AiSceneActivationBinding> ActiveBinding() const;

        /** @brief Cancels owned work and closes admission; retained external snapshots remain valid. */
        void BeginShutdown() noexcept;

        /** @brief Creates an explicit owner-lane adapter for atomic entity-group additions and retirement.
         * @param descriptors Immutable controller catalog copied into the owned adapter.
         * @return Adapter borrowing this runtime, which must outlive the Scene service using it.
         * @details Preparation never replaces existing agents/tasks. Detached candidates retain their publication
         * across shutdown and reject publication after any owner mutation. Public C++ consumers must rebuild.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(
            std::span<const AiControllerDescriptor> descriptors);

        /** @brief Reports whether the runtime rejects new activation and safe-point operations. */
        [[nodiscard]] constexpr bool IsShutdown() const noexcept {
            return shutdown_;
        }

    private:
        friend class AiSceneActivationCandidate;
        friend class Detail::AiSceneStructuralParticipant;

        explicit AiSceneRuntime(AiSceneRuntimeSettings settings) noexcept;

        [[nodiscard]] Result<void> ValidateCandidate(const AiSceneActivationBinding &binding) const;
        [[nodiscard]] bool PublishCandidate(std::shared_ptr<Detail::AiSceneRuntimeState> &state, const AiSceneActivationBinding &binding,
                                            std::uint64_t publicationToken) noexcept;
        void ShutdownState(Detail::AiSceneRuntimeState &state) const noexcept;
        void RetirePublication(std::uint64_t publicationToken) noexcept;

        AiSceneRuntimeSettings settings_;
        std::shared_ptr<Detail::AiSceneRuntimeState> active_;
        std::uint64_t nextPublicationToken_{1};
        std::uint64_t activePublicationToken_{};
        bool shutdown_{};
    };

    /**
     * @brief RuntimeScene activation participant for typed AI agent/controller components.
     * @details It resolves entity generations before publication and never mutates the authoring document.
     */
    class AiSceneActivationParticipant final : public Runtime::SceneActivationParticipant {
    public:
        /**
         * @brief Binds a scene runtime and inert descriptor catalog whose lifetimes exceed this participant.
         * @param runtime Owner-thread AI scene runtime.
         * @param descriptors Immutable controller dependency descriptors.
         */
        AiSceneActivationParticipant(AiSceneRuntime &runtime, std::span<const AiControllerDescriptor> descriptors) noexcept
            : runtime_(&runtime), descriptors_(descriptors) {}

        /** @copydoc Runtime::SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         Runtime::RuntimeSceneView scene) override;

    private:
        AiSceneRuntime *runtime_{};
        std::span<const AiControllerDescriptor> descriptors_;
    };
}  // namespace Horo::AI
