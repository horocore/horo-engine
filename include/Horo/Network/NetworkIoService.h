#pragma once

/**
 * @file NetworkIoService.h
 * @brief Bounded transport I/O polling and owner-thread completion handoff.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkFailure.h"
#include "Horo/Network/PacketBuffer.h"
#include "Horo/Network/TransportCapabilities.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace Horo::Network {
    class NetworkMetrics;
    /** @brief Absolute prepared completion capacity accepted by one host-scoped service. */
    inline constexpr std::size_t MaximumNetworkIoQueuedCompletions = 4096;
    /** @brief Absolute normalized completion budget accepted by one backend poll. */
    inline constexpr std::size_t MaximumNetworkIoCompletionsPerPoll = 1024;
    /** @brief Absolute callback budget accepted by one owner-thread drain. */
    inline constexpr std::size_t MaximumNetworkIoCompletionsPerDrain = 1024;

    /** @brief Closed completion vocabulary transferred from transport I/O to its declared owner thread. */
    enum class NetworkIoCompletionKind : std::uint8_t {
        OperationSucceeded,
        PacketReceived,
        OperationCancelled,
        OperationFailed,
        Count
    };

    /** @brief Setup-time queue and per-call work bounds for one host-scoped I/O service. */
    struct NetworkIoServiceLimits final {
        std::size_t maximumQueuedCompletions{};   /**< Positive prepared queue capacity. */
        std::size_t maximumCompletionsPerPoll{};  /**< Positive maximum records one backend poll may publish. */
        std::size_t maximumCompletionsPerDrain{}; /**< Positive maximum callbacks one owner drain may invoke. */
    };

    /** @brief Move-only immutable completion whose payload and failure evidence own their storage. */
    class NetworkIoCompletion final {
    public:
        /**
         * @brief Creates a non-packet terminal or success completion for one connection generation.
         * @param kind OperationSucceeded or OperationCancelled.
         * @param connection Exact connection generation observed by the backend.
         * @return Immutable completion or NetworkErrors::NetworkIoCompletionInvalid.
         */
        [[nodiscard]] static Result<NetworkIoCompletion> MakeOperation(NetworkIoCompletionKind kind, ConnectionHandle connection);
        /**
         * @brief Creates one owned inbound packet completion.
         * @param connection Exact connection generation observed by the backend.
         * @param channel Backend-neutral channel already admitted by the connection.
         * @param payload Owned bounded packet lease transferred into the completion.
         * @return Immutable completion or NetworkErrors::NetworkIoCompletionInvalid.
         */
        [[nodiscard]] static Result<NetworkIoCompletion> MakePacket(ConnectionHandle connection, ChannelId channel, PacketBuffer payload);
        /**
         * @brief Creates one typed terminal failure completion.
         * @param connection Exact connection generation observed by the backend.
         * @param failure Canonical immutable failure evidence matching the connection when context names it.
         * @return Immutable completion or NetworkErrors::NetworkIoCompletionInvalid.
         */
        [[nodiscard]] static Result<NetworkIoCompletion> MakeFailure(ConnectionHandle connection, const NetworkTerminalRecord &failure);

        NetworkIoCompletion(NetworkIoCompletion &&) noexcept = default;
        NetworkIoCompletion &operator=(NetworkIoCompletion &&) = delete;
        NetworkIoCompletion(const NetworkIoCompletion &) = delete;
        NetworkIoCompletion &operator=(const NetworkIoCompletion &) = delete;

        /** @brief Returns service-assigned FIFO sequence. @return Non-zero after successful publication. */
        [[nodiscard]] constexpr std::uint64_t Sequence() const noexcept {
            return sequence_;
        }

        /** @brief Returns typed completion kind. @return Closed completion kind. */
        [[nodiscard]] constexpr NetworkIoCompletionKind Kind() const noexcept {
            return kind_;
        }

        /** @brief Returns exact connection generation. @return Backend-neutral connection handle. */
        [[nodiscard]] constexpr ConnectionHandle Connection() const noexcept {
            return connection_;
        }

        /** @brief Returns packet channel. @return Meaningful only for PacketReceived. */
        [[nodiscard]] constexpr ChannelId Channel() const noexcept {
            return channel_;
        }

        /** @brief Returns immutable packet bytes. @return Empty for non-packet completions. */
        [[nodiscard]] std::span<const std::byte> Payload() const noexcept {
            return payload_.Bytes();
        }

        /** @brief Returns typed terminal failure. @return Failure record only for OperationFailed. */
        [[nodiscard]] const NetworkTerminalRecord *Failure() const noexcept;

    private:
        friend struct NetworkIoServiceState;
        explicit NetworkIoCompletion(NetworkIoCompletionKind kind, ConnectionHandle connection) noexcept;

        std::uint64_t sequence_{};
        NetworkIoCompletionKind kind_{NetworkIoCompletionKind::OperationSucceeded};
        ConnectionHandle connection_{};
        ChannelId channel_{};
        PacketBuffer payload_{};
        std::optional<NetworkTerminalRecord> failure_;
    };

    struct NetworkIoServiceState;

    /** @brief Generation-scoped producer passed only to one bounded backend poll. */
    class NetworkIoCompletionProducer final {
    public:
        /**
         * @brief Publishes one owned completion to the bounded FIFO.
         * @param completion Valid immutable completion transferred on success.
         * @return Success or typed stale-poll, capacity, malformed, cancellation, or shutdown failure.
         */
        [[nodiscard]] Result<void> Publish(NetworkIoCompletion completion) const;

    private:
        friend class NetworkIoService;
        NetworkIoCompletionProducer(std::shared_ptr<NetworkIoServiceState> state, std::uint64_t pollGeneration) noexcept;
        std::shared_ptr<NetworkIoServiceState> state_;
        std::uint64_t pollGeneration_{};
    };

    /** @brief Private-backend polling seam; implementations retain no producer reference after Poll returns. */
    class INetworkIoPollSource {
    public:
        virtual ~INetworkIoPollSource() = default;
        /**
         * @brief Performs one bounded backend poll on the transport-owned I/O thread.
         * @param producer Generation-scoped normalized completion publisher.
         * @param maximumCompletions Inclusive publication budget for this call.
         * @param cancellation Borrowed cooperative cancellation state.
         * @return Success or the exact backend-neutral polling failure.
         */
        [[nodiscard]] virtual Result<void> Poll(const NetworkIoCompletionProducer &producer, std::size_t maximumCompletions,
                                                const CancellationToken &cancellation) noexcept = 0;
        /** @brief Thread-safe signal that wakes a blocked Poll during shutdown. */
        virtual void RequestStop() noexcept = 0;
        /** @brief Releases backend state after all Poll calls have returned. */
        virtual void Shutdown() noexcept = 0;
    };

    /** @brief Non-owning callback used only on the stack of one owner-thread drain. */
    class INetworkIoCompletionConsumer {
    public:
        virtual ~INetworkIoCompletionConsumer() = default;
        /** @brief Consumes one ordered immutable completion without retaining service state. */
        virtual void Consume(NetworkIoCompletion completion) noexcept = 0;
    };

    /**
     * @brief Host-scoped unique owner of backend polling and bounded cross-thread completion handoff.
     *
     * The concrete transport owns the thread that invokes PollBackend. The service owns the injected
     * backend and normalized queue. Only DrainOwnerThread invokes application-facing callbacks, and it
     * invokes them after releasing internal synchronization. Shutdown closes publication before waking
     * and draining the backend, so late producers cannot observe a consumer or republish work.
     */
    class NetworkIoService final {
    public:
        /**
         * @brief Prepares one service and binds its owner to the calling thread.
         * @param backend Unique backend polling implementation; native types remain behind this interface.
         * @param limits Positive finite queue and work bounds.
         * @param metrics Optional owner-thread observer. Host shuts down and destroys this service before metrics;
         *                late producers retain only a separate admission flag, never this pointer.
         * @return Stable-address unique service or typed invalid/capacity/allocation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<NetworkIoService>> Create(std::unique_ptr<INetworkIoPollSource> backend,
                                                                              const NetworkIoServiceLimits &limits,
                                                                              NetworkMetrics *metrics = nullptr);
        ~NetworkIoService();
        NetworkIoService(const NetworkIoService &) = delete;
        NetworkIoService &operator=(const NetworkIoService &) = delete;
        NetworkIoService(NetworkIoService &&) = delete;
        NetworkIoService &operator=(NetworkIoService &&) = delete;

        /**
         * @brief Performs one serialized bounded backend poll on a transport-owned I/O thread.
         * @param maximumCompletions Positive call budget no larger than the configured poll bound.
         * @param cancellation Caller-owned cooperative cancellation state.
         * @return Success or typed invalid, cancelled, busy, shutdown, or backend failure.
         */
        [[nodiscard]] Result<void> PollBackend(std::size_t maximumCompletions, const CancellationToken &cancellation = {});
        /**
         * @brief Drains ordered completions on the thread that created this service.
         * @param consumer Borrowed callback target retained only for each synchronous call.
         * @param maximumCompletions Positive call budget no larger than the configured drain bound.
         * @return Number consumed or typed wrong-thread, invalid, or shutdown failure.
         */
        [[nodiscard]] Result<std::size_t> DrainOwnerThread(INetworkIoCompletionConsumer &consumer, std::size_t maximumCompletions) const;
        /** @brief Stops admission, wakes and releases the backend, and discards undrained records; idempotent. */
        void Shutdown() noexcept;
        /** @brief Returns current bounded queue depth. @return Snapshot count under synchronization. */
        [[nodiscard]] std::size_t QueuedCompletions() const noexcept;
        /** @brief Reports terminal shutdown. @return True once admission has closed. */
        [[nodiscard]] bool IsShuttingDown() const noexcept;

    private:
        struct ConstructionKey final {};

    public:
        /** @internal Factory-only constructor exposed for std::make_unique access. */
        NetworkIoService(ConstructionKey, std::unique_ptr<INetworkIoPollSource> backend, std::shared_ptr<NetworkIoServiceState> state,
                         NetworkMetrics *metrics) noexcept;

    private:
        std::unique_ptr<INetworkIoPollSource> backend_;
        std::shared_ptr<NetworkIoServiceState> state_;
        std::mutex pollMutex_;
        NetworkMetrics *metrics_{};
    };
}  // namespace Horo::Network
