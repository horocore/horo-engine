#pragma once

#include "Horo/Network/ReplicationStateCapture.h"

#include <thread>

namespace Horo::Network {
    namespace Detail {
        /** @brief Prepared typed field envelope; the owner-thread writer resets only its written marker. */
        struct ReplicationPreparedCaptureField final {
            FieldId field;
            ReplicationValueKind kind;
            std::size_t maximumElements{};
            bool written{};
        };
    }  // namespace Detail

    /** @brief Prepared pools retain their allocation until owner-thread shutdown, including revoked external pins. */
    struct ReplicationStateCapture::Impl final {
        struct Target final {
            ReplicationCaptureTarget binding;
            std::vector<Detail::ReplicationPreparedCaptureField> fields;
            std::vector<std::shared_ptr<ReplicationCapturedState>> pool;
            ReplicationCapturedStatePin latest;
            std::size_t byteBound{};
            std::uint64_t publication{};
            bool dirty{};
            std::uint64_t lastConsideredTick{};
        };

        std::shared_ptr<const ReplicationSerializerRegistry> serializers;
        std::shared_ptr<std::atomic_bool> admission{std::make_shared<std::atomic_bool>(true)};
        ReplicationWorldActivationDescriptor world;
        ReplicationCaptureLimits limits;
        std::vector<Target> targets;
        std::size_t cursor{};
        std::vector<std::size_t> hints;
        std::size_t hintHead{};
        std::size_t hintCount{};

        struct TickWork final {
            const ReplicationWorldCaptureRead &world;
            std::uint64_t tick;
            const CancellationToken &cancellation;
            ReplicationCaptureReport report;
            std::size_t fields{};
            std::size_t bytes{};
        };

        std::uint64_t lastTick{};
        std::thread::id owner{std::this_thread::get_id()};
        bool capturing{};

        [[nodiscard]] bool Continue(const TickWork &work) const noexcept;
        [[nodiscard]] bool Consider(std::size_t index, TickWork &work);
        void Reconcile(TickWork &work);
        void ServeHints(TickWork &work);
        void Schedule(TickWork &work);
        [[nodiscard]] Result<bool> CaptureSafely(Target &target, const ReplicationWorldCaptureRead &world, std::uint64_t tick,
                                                 const CancellationToken &cancellation);
        [[nodiscard]] Result<bool> Publish(Target &target, ReplicationCapturedState &candidate,
                                           const std::shared_ptr<ReplicationCapturedState> &slot, const ReplicationWorldCaptureRead &world,
                                           const ReplicationCommittedRead &read) const;
        [[nodiscard]] Target *Find(NetworkObjectId object) noexcept;
        [[nodiscard]] const Target *Find(NetworkObjectId object) const noexcept;
        [[nodiscard]] Result<void> PrepareFields(Target &target, const ReplicationSchemaDescriptor &schema) const;
        [[nodiscard]] Result<void> PrepareSlots(Target &target, std::size_t &preparedBytes) const;
        [[nodiscard]] Result<void> PrepareTarget(const ReplicationWorldCaptureRead &read, const ReplicationCaptureTarget &binding,
                                                 std::size_t &preparedBytes);
        [[nodiscard]] Result<void> ValidateRead(Target &target, const ReplicationWorldCaptureRead &world,
                                                const ICommittedReplicationSource &source, const ReplicationCommittedRead &read,
                                                const CancellationToken &cancellation) const;
        [[nodiscard]] Result<bool> CompareCandidate(const Target &target, const ReplicationCapturedState &candidate,
                                                    const ReplicationCapturedStatePin &prior) const;
        [[nodiscard]] Result<bool> CaptureTarget(Target &target, const ReplicationWorldCaptureRead &read, std::uint64_t tick,
                                                 const CancellationToken &cancellation) const;
    };
}  // namespace Horo::Network
