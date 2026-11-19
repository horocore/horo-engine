#pragma once
/** @file NetworkWorldBindingState.h @brief Bounded private world-control publication/outbox ownership. */
#include "Horo/Network/NetworkInitialStateStream.h"
#include "NetworkModeWorldState.h"
#include "NetworkWorldBindingController.h"

namespace Horo::Network {
    /** @brief Verifies the optional route is distinct and registered with an appropriate bounded payload. */
    [[nodiscard]] bool ValidPrefabLifecycleRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs);
    /** @brief Verifies the authored lane's identity and finite page budget against the registry. */
    [[nodiscard]] bool ValidAuthoredSceneRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs);
    /** @brief Verifies the selected field synchronization route and its structural prerequisites. */
    [[nodiscard]] bool ValidInitialStateRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs);
    /** @brief Checks negotiated post-initial full-state framing and explicit bounded allowance. */
    [[nodiscard]] bool ValidStateUpdateRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs);
    /** @brief Checks that the world and selected required features are present in either negotiated or required features. */
    [[nodiscard]] bool WorldBindingFeaturesSupported(const NetworkWorldBindingPolicy &policy, std::span<const ProtocolFeatureId> features);

    /** @brief Recipient and object grants pinned by the exact reliable initial snapshot. */
    struct NetworkInitialUpdateCohort final {
        std::vector<ReplicationInitialOwnership> ownership;
        NetworkPeerId recipient;
        std::uint64_t descriptorGeneration{};
    };

    struct NetworkWorldBindingController::Impl final {
        struct BusyScope final {
            explicit BusyScope(bool &ownerBusy) noexcept : busy(ownerBusy) {
                ownerBusy = true;
            }

            ~BusyScope() {
                busy = false;
            }

            BusyScope(const BusyScope &) = delete;
            BusyScope &operator=(const BusyScope &) = delete;
            bool &busy;
        };

        struct Peer final {
            struct InitialTransfer final {
                NetworkInitialStateStamp stamp;
                std::optional<ReplicationInitialStateSnapshot> snapshot;
                std::unique_ptr<NetworkInitialStateInbox> inbox;
                Runtime::SceneRuntimeId clientScene;
                NetworkPeerId recipient;
                std::size_t record{};
                std::size_t offset{};
                std::uint64_t deadline{};
                std::uint64_t receivedTick{};
                bool manifestQueued{};
                bool sent{};
                bool published{};
                bool confirmed{};

                /** @brief The active authority world is still publishing the retained snapshot. */
                [[nodiscard]] bool CanSend(const NetworkWorldBindingRecord *current) const noexcept {
                    return current && current->worldSession == stamp.session && !sent;
                }

                /** @brief Pending work expires at its original fixed deadline. */
                [[nodiscard]] bool ExpiredAt(std::uint64_t tick) const noexcept {
                    return !confirmed && deadline != 0 && tick >= deadline;
                }
            };

            struct PrefabChange final {
                NetworkObjectId occurrence;
                bool spawn;
            };

            struct AuthoredProgress final {
                NetworkSessionGeneration session;
                std::size_t next{};
                bool complete{};
            };

            std::shared_ptr<PeerSessionLifecycle> lifecycle;
            ConnectionHandle connection;
            NetworkOperationGeneration generation;
            bool sendsWorld{};
            std::uint32_t sequence{};
            std::optional<NetworkWorldBindingRecord> sent;
            std::optional<NetworkWorldBindingRecord> pending;
            NetworkSessionGeneration establishedWorld; /**< Last native-accepted Initial or Commit session. */
            std::array<std::byte, 256> frame{};
            std::vector<std::byte> extendedFrame;
            std::optional<PrefabChange> prefabPending;
            AuthoredProgress authoredSent;
            NetworkSessionGeneration authoredPublished; /**< Last complete inventory for an established world. */
            std::optional<AuthoredProgress> authoredPending;
            std::vector<NetworkObjectId> knownPrefabs;
            NetworkSessionGeneration prefabSession;
            std::uint64_t prefabRevision{};
            Peer::InitialTransfer initial;
            std::shared_ptr<const NetworkInitialUpdateCohort> initialUpdateCohort;
            std::uint32_t updateSequence{};
            std::uint64_t lastUpdateTick{};
            std::uint64_t lastUpdateFlush{};
            bool initialFramePending{};
            std::size_t frameBytes{};
            std::uint64_t deadline{};          // Current queued frame's bounded send deadline.
            std::uint64_t preparedDeadline{};  // Native-accepted PREPARE fence; unrelated traffic cannot extend it.
            std::optional<Error> failure;
        };

        Impl(const NetworkModePlan &plan, NetworkWorldBindingSetup setup)
            : worlds(plan, setup.policy, setup.schemaFingerprint, setup.maximumPendingTicks), setup(std::move(setup)),
              version(plan.protocolVersion), role(plan.mode) {}

        [[nodiscard]] Result<void> CheckOwner() const;
        /** @brief Registers both selected routes atomically and revokes the first on a partially failed attach. */
        [[nodiscard]] Result<void> RegisterRoutes(InboundMessageDispatcher &router,
                                                  const std::shared_ptr<NetworkWorldBindingController> &owner) const;
        [[nodiscard]] Result<void> ValidatePeer(const PeerSessionLifecycle &peer, NetworkOperationGeneration generation,
                                                std::uint64_t nowTick) const;
        [[nodiscard]] Result<void> Queue(Peer &peer, const NetworkWorldBindingRecord &record, std::uint64_t nowTick) const;
        [[nodiscard]] Result<void> QueueFrame(Peer &peer, MessageEnvelopePayloadView message, std::uint64_t nowTick,
                                              std::span<std::byte> extendedStorage = {}) const;
        /** @brief Queue only an advancing server publication, preserving pending traffic and late-join initial identity. */
        [[nodiscard]] Result<void> QueueWorldUpdate(Peer &peer, const NetworkWorldBindingRecord *prepared,
                                                    const NetworkWorldBindingRecord *current, std::uint64_t nowTick) const;
        /** @brief Whether this peer has no remaining authored page debt on the specified world. */
        [[nodiscard]] bool AuthoredSent(const Peer &peer, NetworkSessionGeneration session) const noexcept;
        /** @brief Queues the next authored page for the binding already sent to this peer. */
        [[nodiscard]] Result<void> QueueAuthoredUpdate(Peer &peer, std::uint64_t nowTick) const;
        /** @brief Fits the next page to both the codec budget and negotiated enclosing frame. */
        [[nodiscard]] Result<void> QueueFittedAuthoredPage(Peer &peer, const ReplicationAuthoredSceneInventory &inventory,
                                                           std::size_t first, std::uint64_t nowTick) const;
        /** @brief Writes one page into the same bounded reliable sequence as world and Prefab control. */
        [[nodiscard]] Result<void> QueueAuthoredPage(Peer &peer, const NetworkAuthoredScenePage &page, std::uint64_t nowTick) const;
        /** @brief Queues the next structural delta after the exact world binding was accepted by native send. */
        [[nodiscard]] Result<void> QueuePrefabUpdate(Peer &peer, const NetworkWorldBindingRecord *prepared,
                                                     const NetworkWorldBindingRecord *current, std::uint64_t nowTick);
        /** @brief Serializes one occurrence into the shared reliable sequence slot. */
        [[nodiscard]] Result<void> QueuePrefabRecord(Peer &peer, const NetworkPrefabLifecycleRecord &record, std::size_t expectedGroups,
                                                     std::uint64_t nowTick) const;
        /** @brief Advances the known inventory only after the original frame is accepted. */
        static void CompletePrefabSend(Peer &peer) noexcept;
        /** @brief Advances authored inventory progress only after native delivery accepts its exact frame. */
        static void CompleteAuthoredSend(Peer &peer) noexcept;
        /** @brief Advances the world handoff and its established authored generation. */
        void CompleteBindingSend(Peer &peer, std::uint64_t tick) const noexcept;
        /** @brief Guards normal traffic until the exact current occurrence is confirmed. */
        [[nodiscard]] bool InitialConfirmed(const Peer &peer) const noexcept;
        [[nodiscard]] Result<void> AcceptStateUpdate(const MessageEnvelope &message, const InboundMessageContext &context);
        [[nodiscard]] Result<void> FlushStateUpdates(Peer &peer, std::uint64_t now);
        void ApplyStateUpdates(std::uint64_t localSimulationTick, const CancellationToken &cancellation);
        [[nodiscard]] Result<NetworkInitialStateTarget> PrepareStateUpdates(const ReplicationInboundAuthority &authority,
                                                                            ReplicationWorldLifecycle &world,
                                                                            std::span<const ReplicationInitialOwnership> ownership,
                                                                            std::uint64_t now);
        [[nodiscard]] Result<void> RetainUpdateCohort(Peer &peer, const ReplicationInitialStateSnapshot &snapshot);
        /** @brief Accepts a frame from the authority or an exact publication receipt from its client. */
        [[nodiscard]] Result<void> AcceptInitial(const MessageEnvelope &message, const InboundMessageContext &context);
        /** @brief Confirms only a receipt for a fully sent snapshot on its bound client peer. */
        [[nodiscard]] static Result<void> AcceptInitialReceipt(Peer &peer, std::span<const std::byte> payload, std::uint64_t tick);
        /** @brief Forwards a field frame only to its exact authenticated world and receiving inbox. */
        [[nodiscard]] Result<void> ReceiveInitial(Peer &peer, const MessageEnvelope &message, const InboundMessageContext &context);
        /** @brief Acquires the next complete capture and advances the single reliable outbox by one frame. */
        [[nodiscard]] Result<void> QueueInitial(Peer &peer, const NetworkWorldBindingRecord *current, std::uint64_t tick);
        /** @brief Queues the receiver's receipt only after the initial transaction publishes. */
        [[nodiscard]] Result<void> QueueInitialReceipt(Peer &peer, std::uint64_t tick);
        /** @brief Serializes the next manifest or contiguous segment within the negotiated frame budget. */
        [[nodiscard]] Result<void> QueueInitialPayload(Peer &peer, std::uint64_t tick);
        /** @brief Chooses a bounded capture only after the peer has the matching structural inventory. */
        [[nodiscard]] Result<void> CaptureInitial(Peer &peer, const NetworkWorldBindingRecord &current, std::uint64_t tick);
        /** @brief Encodes a frame in the same reliable outbox as world and Prefab messages. */
        [[nodiscard]] Result<void> QueueInitialFrame(Peer &peer, std::span<const std::byte> payload, std::uint64_t tick);
        /** @brief Applies complete received fields after the structural owner phase. */
        void ApplyInitial(std::uint64_t localSimulationTick, const CancellationToken &cancellation);
        /** @brief Applies one complete snapshot using the exact configured Scene/Gameplay owner. */
        [[nodiscard]] Result<void> ApplyClientInitial(Peer &peer, std::uint64_t localSimulationTick, const CancellationToken &cancellation);
        /** @brief Commits a fully reconstructed client baseline and publishes its initial owner facts before receipt. */
        [[nodiscard]] Result<void> CommitClientInitial(Peer &peer, ReplicationWorldLifecycle &world,
                                                       const ReplicationWorldWorkRequest &request, std::uint64_t tick);
        /** @brief Acquires the explicit Scene transaction owner behind the synchronous provider boundary. */
        [[nodiscard]] Result<NetworkInitialStateTarget> PrepareInitialTarget(const ReplicationInboundAuthority &authority,
                                                                             ReplicationWorldLifecycle &world,
                                                                             std::span<const ReplicationInitialOwnership> ownership,
                                                                             std::uint64_t tick);
        /** @brief Binds the first accepted frame to the live local recipient and exact receiving Scene. */
        [[nodiscard]] Result<void> OpenInitialInbox(Peer &peer, NetworkInitialStateStamp stamp, Runtime::SceneRuntimeId scene,
                                                    std::uint64_t tick);
        /** @brief Projects the exact receipt and apply authority for an admitted client peer. */
        [[nodiscard]] ReplicationInboundAuthority InitialAuthority(const Peer &peer) const;
        /** @brief Releases captured storage and advances receipt eligibility only after native acceptance. */
        static void CompleteInitialSend(Peer &peer) noexcept;
        [[nodiscard]] Result<void> Send(Peer &peer, std::uint64_t nowTick);
        void Retire(Peer &peer, std::uint64_t nowTick) noexcept;
        void ClearPending() noexcept;
        void ReleaseCommitReservations(const NetworkWorldBindingRecord &commit) noexcept;

        Detail::NetworkModeWorldState worlds;
        const NetworkWorldBindingSetup setup;
        const ProtocolVersion version;
        const NetworkProjectRole role;
        std::array<Peer, 64> peers;
        INetworkTransport *transport{};
        InboundMessageDispatcher *dispatcher{};
        std::uint64_t nowTick{};
        const std::thread::id owner{std::this_thread::get_id()};
        bool busy{};
        bool detached{};
        bool stopped{};
    };
}  // namespace Horo::Network
