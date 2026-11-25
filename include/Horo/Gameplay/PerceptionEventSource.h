#pragma once

/**
 * @file PerceptionEventSource.h
 * @brief Host-composed read-only consumption of committed Gameplay deliveries into transient AI memory.
 */

#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/PerceptionEventRouting.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Gameplay {
    /** @brief Gameplay's committed perception-filter outcome for this exact recipient delivery. */
    enum class PerceptionFilterDecision : std::uint8_t {
        Accept,
        Reject
    };

    /**
     * @brief One committed authoritative producer record, after Gameplay has selected and permitted the recipient.
     * @details Combat, contact and squad owners supply these values at the host boundary. This is not network input.
     * Team delivery identities and recipient lists belong to the producer; no Perception code computes them.
     */
    struct CommittedPerceptionDelivery final {
        AI::PerceptionEventDelivery delivery;
        PerceptionFilterDecision filter{PerceptionFilterDecision::Reject};
    };

    /** @brief Gameplay's proposed sound and exact disclosure decision for one listener. */
    struct GameplayHearingDelivery final {
        AI::AgentHandle recipient;
        AI::PerceptionListenerTypeId listener;
        AI::HearingPerceptionEvent sound; /**< The publisher assigns the first eligible sensing tick. */
        PerceptionFilterDecision filter{PerceptionFilterDecision::Reject};
    };

    /**
     * @brief One bounded owner-thread sensing window captured from authoritative Gameplay producers.
     * @details The composition root alone captures committed producer output. All borrowed owners and the frozen registry
     * outlive this window. Delivery rechecks the live host role, AI publication and Scene generations; no producer callback,
     * mutable source-system capability, topology policy or durable squad state enters Perception. Close before owner teardown.
     */
    class PerceptionEventSource final {
    public:
        static constexpr std::size_t MaximumDeliveries = 128;

        /**
         * @brief Captures one complete producer batch for an authoritative Scene window.
         * @param scene Live scene owner, borrowed read-only.
         * @param ai Live AI owner, borrowed read-only.
         * @param host Active host network composition, the authority for world roles.
         * @param world Exact selected host world; client worlds are denied under ADR-022.
         * @param registry Frozen perception descriptor registry.
         * @param filtering Current Scene-owned typed filter publication; owner outlives the window.
         * @param deliveries Already authorized recipient deliveries and producer-owned filter decisions.
         * @return Owned bounded batch or typed authority, identity or capacity failure.
         * @throws std::bad_alloc If bounded batch storage cannot be allocated.
         */
        [[nodiscard]] static Result<PerceptionEventSource> Capture(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai,
                                                                   const Network::NetworkModeComposition &host,
                                                                   Network::NetworkModeWorldKind world,
                                                                   const AI::PerceptionDescriptorRegistry &registry,
                                                                   const AI::PerceptionSpatialBroadphase &filtering,
                                                                   std::span<const CommittedPerceptionDelivery> deliveries);

        /** @brief Captures the same authoritative batch from an immutable view instead of borrowing the Scene owner.
         * @param scene Exact current structural publication. Any subsequent structural change revokes the window.
         * @param ai Live AI owner. @param host Active network authority. @param world Exact selected hosted world.
         * @param registry Frozen perception descriptors. @param filtering Exact spatial owner for this Scene.
         * @param deliveries Committed producer deliveries with their original disclosure decisions.
         * @return Owned bounded batch or typed refusal.
         */
        [[nodiscard]] static Result<PerceptionEventSource> Capture(const Runtime::RuntimeSceneView &scene, const AI::AiSceneRuntime &ai,
                                                                   const Network::NetworkModeComposition &host,
                                                                   Network::NetworkModeWorldKind world,
                                                                   const AI::PerceptionDescriptorRegistry &registry,
                                                                   const AI::PerceptionSpatialBroadphase &filtering,
                                                                   std::span<const CommittedPerceptionDelivery> deliveries);

        PerceptionEventSource(const PerceptionEventSource &) = delete;
        PerceptionEventSource &operator=(const PerceptionEventSource &) = delete;
        PerceptionEventSource(PerceptionEventSource &&) noexcept = default;
        PerceptionEventSource &operator=(PerceptionEventSource &&) = delete;

        /**
         * @brief Publishes one captured delivery into its exact recipient memory during PerceptionSensePoll.
         * @param index Producer record index in this bounded batch.
         * @param memory Exact recipient's Scene-owned transient memory.
         * @return Success or typed invalid, revoked, stale, filtered or descriptor failure, with no publication on denial.
         * @details Valid hearing deliveries for the same recipient, listener, source and simulation tick coalesce to the
         * newest emission in the captured window. Producer order breaks ties. Rejected deliveries never participate.
         */
        [[nodiscard]] Result<void> Deliver(std::size_t index, AI::AIPerceptionMemory &memory) const;

        /** @brief Borrows this captured window for synchronous consumption during the Scene's sensing phase.
         * @return A feed valid while this source remains alive and unmoved. Closing this source revokes the feed.
         */
        [[nodiscard]] AI::PerceptionEventFeed Feed() const & noexcept;
        AI::PerceptionEventFeed Feed() const && = delete;

        /** @brief Revokes this window before producer/Scene/host replacement or teardown; repeated calls are safe. */
        void Close() noexcept;

    private:
        /** @brief Captured host authority and its matching AI activation publication. */
        struct CapturedAuthority final {
            Network::NetworkModeRoleView role;
            AI::AiSceneActivationBinding binding;
        };

        /** @brief Distinguishes indexed delivery from an ordered traversal of all records. */
        enum class DeliveryMode : std::uint8_t {
            ResolveSelected,
            SelectedOnly
        };

        PerceptionEventSource(const Runtime::RuntimeSceneView &scene, const AI::AiSceneRuntime &ai,
                              const Network::NetworkModeComposition &host,
                              const CapturedAuthority &authority, const AI::PerceptionDescriptorRegistry &registry,
                              const AI::PerceptionSpatialBroadphase &filtering, std::span<const CommittedPerceptionDelivery> deliveries);
        /** @brief Returns live structural evidence for the owner overload, or the exact captured view. */
        [[nodiscard]] Runtime::RuntimeSceneView CurrentScene() const noexcept;
        /** @brief Rejects retained windows after authority revocation, travel, AI replacement or source closure. */
        [[nodiscard]] Result<void> ValidateCurrent() const;
        /** @brief Binds a declared recipient to the current active AI owner and exact memory. */
        [[nodiscard]] Result<void> ValidateRecipient(const AI::PerceptionEventDelivery &delivery,
                                                     const AI::AIPerceptionMemory &memory) const;
        /** @brief Routes a sound individually or skips superseded records during a complete sensing pass. */
        [[nodiscard]] Result<void> DeliverRecord(std::size_t index, AI::AIPerceptionMemory &memory, DeliveryMode mode) const;
        /** @brief Consumes admitted hearing for one exact Scene-owned listener at this fixed tick. */
        [[nodiscard]] Result<void> DeliverSensingWindow(std::uint64_t tick, const AI::AiControllerPerceptionEndpoints &endpoints,
                                                        AI::AIPerceptionMemory &memory) const;

        Runtime::RuntimeSceneView capturedScene_;
        const Runtime::RuntimeScene *scene_{};
        const AI::AiSceneRuntime *ai_;
        const Network::NetworkModeComposition *host_;
        Network::NetworkModeRoleView role_;
        AI::AiSceneActivationBinding binding_;
        const AI::PerceptionDescriptorRegistry *registry_;
        const AI::PerceptionSpatialBroadphase *filtering_;
        std::shared_ptr<const AI::PerceptionSpatialSnapshot> filterSnapshot_;
        std::vector<CommittedPerceptionDelivery> deliveries_;
        bool closed_{};
    };

    /** @brief Bounded Gameplay publisher for one Scene incarnation and one actual scheduler.
     * @details A trusted application owner prepares one complete batch before fixed commitment and promotes it only with
     * actual scheduler proof. The next sensing tick can borrow the committed batch. Audio owns no part of the handoff.
     */
    class GameplayHearingPublisher final {
    public:
        /** @brief Binds an explicit scheduler and Scene identity without opening a publication.
         * @param source The application's authoritative fixed scheduler.
         * @param scene View identifying the exact Scene incarnation. Later structural publications in that Scene are supported.
         */
        GameplayHearingPublisher(Runtime::RuntimeDispatchSource source, const Runtime::RuntimeSceneView &scene) noexcept;
        GameplayHearingPublisher(const GameplayHearingPublisher &) = delete;
        GameplayHearingPublisher &operator=(const GameplayHearingPublisher &) = delete;

        /** @brief Validates and stages one complete Gameplay batch for the current fixed attempt.
         * @param scene Current view from the bound Scene incarnation.
         * @param evidence The active fixed callback's scheduler-issued attempt evidence.
         * @param deliveries Already selected recipients and disclosure outcomes. An empty batch clears the previous output.
         * @return Success or typed authority, Scene, tick, or payload refusal. The committed batch is unchanged.
         */
        [[nodiscard]] Result<void> Prepare(const Runtime::RuntimeSceneView &scene, const Runtime::RuntimeDispatchEvidence &evidence,
                                           std::span<const GameplayHearingDelivery> deliveries);

        /** @brief Promotes prepared storage without allocation or user callbacks during actual scheduler commitment.
         * @param evidence Active fixed callback evidence for the prepared attempt.
         * @return True when the batch was promoted. Invalid, duplicate and uncommitted attempts leave it unchanged.
         */
        [[nodiscard]] bool Commit(const Runtime::RuntimeDispatchEvidence &evidence) noexcept;

        /** @brief Discards an uncommitted candidate during rollback of the matching attempt.
         * @param evidence Active fixed callback evidence for the aborted attempt.
         */
        void Abort(const Runtime::RuntimeDispatchEvidence &evidence) noexcept;

        /** @brief Borrows the committed records for the immediately following sensing tick.
         * @param scene Current view from the bound Scene incarnation.
         * @param tick Tick for which the host is opening sensing.
         * @return Empty when there is no batch for that tick, or a typed authority or stale Scene error.
         * The values may be borrowed until the next publication.
         */
        [[nodiscard]] Result<std::span<const CommittedPerceptionDelivery>> Read(const Runtime::RuntimeSceneView &scene,
                                                                               std::uint64_t tick) const;

    private:
        /** @brief One bounded candidate or committed sensing batch. */
        struct Batch final {
            std::array<CommittedPerceptionDelivery, PerceptionEventSource::MaximumDeliveries> records{};
            std::size_t count{};
            std::uint64_t sensingTick{};
        };

        Runtime::RuntimeDispatchSource source_;
        Runtime::RuntimeSceneView scene_;
        std::array<Batch, 2> batches_{};
        std::size_t published_{};
        Runtime::RuntimeDispatchEvidence prepared_;
    };

    /** @brief Explicit host-selected producer of a committed Gameplay window. An empty result supplies no events for the tick. */
    using PerceptionEventSourceFactory = std::function<Result<std::optional<PerceptionEventSource>>(
        const Runtime::RuntimeSceneView &, const AI::AiSceneRuntime &, const AI::PerceptionSpatialBroadphase &, std::uint64_t)>;

    /** @brief Polls native senses and committed Gameplay events together before the host publishes Blackboard values.
     * @param ai Current Scene runtime. @param scene Current structural view. @param spatial Current Spatial publication owner.
     * @param tick Exact simulation tick. @param fixedStep Host simulation quantum.
     * @param producer Optional explicit producer. It is called only for a new valid tick with current spatial evidence.
     * Its returned capture stays owned until the synchronous poll returns.
     * @return Success or original producer or sensing failure. An absent producer retains the existing native path.
     */
    [[nodiscard]] Result<void> PollPerceptionWithGameplay(AI::AiSceneRuntime &ai, const Runtime::RuntimeSceneView &scene,
                                                           const AI::PerceptionSpatialBroadphase &spatial, std::uint64_t tick,
                                                           Duration fixedStep, const PerceptionEventSourceFactory &producer);
}  // namespace Horo::Gameplay
