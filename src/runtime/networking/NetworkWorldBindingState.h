#pragma once
/** @file NetworkWorldBindingState.h @brief Bounded private world-control publication/outbox ownership. */
#include "NetworkModeWorldState.h"
#include "NetworkWorldBindingController.h"

namespace Horo::Network {
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
            std::shared_ptr<PeerSessionLifecycle> lifecycle;
            ConnectionHandle connection;
            NetworkOperationGeneration generation;
            bool sendsWorld{};
            std::uint32_t sequence{};
            std::optional<NetworkWorldBindingRecord> sent;
            std::optional<NetworkWorldBindingRecord> pending;
            std::array<std::byte, 256> frame{};
            std::size_t frameBytes{};
            std::uint64_t deadline{};          // Current queued frame's bounded send deadline.
            std::uint64_t preparedDeadline{};  // Native-accepted PREPARE fence; unrelated traffic cannot extend it.
            std::optional<Error> failure;
        };

        Impl(const NetworkModePlan &plan, NetworkWorldBindingSetup setup)
            : worlds(plan, setup.policy, setup.schemaFingerprint), setup(std::move(setup)), version(plan.protocolVersion), role(plan.mode) {
        }

        [[nodiscard]] Result<void> CheckOwner() const;
        [[nodiscard]] Result<void> ValidatePeer(const PeerSessionLifecycle &peer, NetworkOperationGeneration generation,
                                                std::uint64_t nowTick) const;
        [[nodiscard]] Result<void> Queue(Peer &peer, const NetworkWorldBindingRecord &record, std::uint64_t nowTick) const;
        [[nodiscard]] Result<void> QueueFrame(Peer &peer, MessageEnvelopePayloadView message, std::uint64_t nowTick) const;
        /** @brief Queue only an advancing server publication, preserving pending traffic and late-join initial identity. */
        [[nodiscard]] Result<void> QueueWorldUpdate(Peer &peer, const NetworkWorldBindingRecord *prepared,
                                                    const NetworkWorldBindingRecord *current, std::uint64_t nowTick) const;
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
