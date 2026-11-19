#pragma once
/** @file NetworkWorldBindingState.h @brief Bounded private world-control publication/outbox ownership. */
#include "NetworkModeWorldState.h"
#include "NetworkWorldBindingController.h"

namespace Horo::Network {
    /** @brief Verifies the optional route is distinct and registered with an appropriate bounded payload. */
    [[nodiscard]] bool ValidPrefabLifecycleRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs);
    /** @brief Verifies the authored lane's identity and finite page budget against the registry. */
    [[nodiscard]] bool ValidAuthoredSceneRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs);

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
