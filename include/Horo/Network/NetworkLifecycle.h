#pragma once

/**
 * @file NetworkLifecycle.h
 * @brief Bounded owner-thread listener and connection lifecycle contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkFailure.h"
#include "Horo/Network/NetworkHandles.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::Network {
    /** @brief Absolute prepared listener slots accepted by one lifecycle registry. */
    inline constexpr std::size_t MaximumNetworkLifecycleListeners = 1024;
    /** @brief Absolute prepared connection slots accepted by one lifecycle registry. */
    inline constexpr std::size_t MaximumNetworkLifecycleConnections = 4096;

    /** @brief Non-zero generation fencing one asynchronous listener or connection operation. */
    class NetworkOperationGeneration final {
    public:
        /** @brief Constructs the reserved invalid generation. */
        constexpr NetworkOperationGeneration() = default;
        /** @brief Creates a valid operation generation. @param value Non-zero generation. @return Typed generation or invalid input. */
        [[nodiscard]] static Result<NetworkOperationGeneration> Create(std::uint64_t value);

        /** @brief Reports whether this generation may fence work. @return True for non-zero values. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        /** @brief Returns the opaque owner-issued generation for safe numeric diagnostics. @return Non-zero value for a valid generation.
         */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        constexpr auto operator<=>(const NetworkOperationGeneration &) const noexcept = default;

    private:
        explicit constexpr NetworkOperationGeneration(const std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    /** @brief Owner-visible listener lifecycle. */
    enum class NetworkListenerState : std::uint8_t {
        Binding,
        Listening,
        Closing,
        Closed,
        Failed,
        Cancelled,
        ShuttingDown,
        Count
    };

    /** @brief Owner-visible asynchronous connection lifecycle before session activation. */
    enum class NetworkConnectionState : std::uint8_t {
        Created,
        Resolving,
        Connecting,
        AuthenticationReady,
        Closing,
        Closed,
        Failed,
        Cancelled,
        TimedOut,
        ShuttingDown,
        Count
    };

    /** @brief Exactly-once durable outcome retained for a listener or connection generation. */
    enum class NetworkLifecycleTerminalKind : std::uint8_t {
        Closed,
        Failed,
        Cancelled,
        TimedOut,
        Shutdown,
        Count
    };

    /** @brief Immutable typed terminal result with optional canonical failure evidence. */
    class NetworkLifecycleTerminal final {
    public:
        /**
         * @brief Validates a terminal kind and its canonical evidence.
         * @param kind Closed, Failed, Cancelled, TimedOut, or Shutdown.
         * @param failure Required for non-Closed outcomes and forbidden for Closed.
         * @return Immutable terminal result or NetworkErrors::NetworkLifecycleInvalid.
         */
        [[nodiscard]] static Result<NetworkLifecycleTerminal> Create(NetworkLifecycleTerminalKind kind,
                                                                     std::optional<NetworkTerminalRecord> failure = {});

        /** @brief Returns the closed terminal category. @return Stable typed outcome. */
        [[nodiscard]] constexpr NetworkLifecycleTerminalKind Kind() const noexcept {
            return kind_;
        }

        /** @brief Returns canonical failure evidence. @return Null only for a graceful Closed result. */
        [[nodiscard]] const NetworkTerminalRecord *Failure() const noexcept;

    private:
        NetworkLifecycleTerminal(NetworkLifecycleTerminalKind kind, std::optional<NetworkTerminalRecord> failure) noexcept;
        NetworkLifecycleTerminalKind kind_{NetworkLifecycleTerminalKind::Closed};
        std::optional<NetworkTerminalRecord> failure_;
    };

    /** @brief Immutable listener lifecycle projection for one exact handle and operation generation. */
    struct NetworkListenerSnapshot final {
        ListenerHandle handle{};                                   /**< Exact current listener generation. */
        NetworkOperationGeneration operation{};                    /**< Exact asynchronous operation generation. */
        NetworkListenerState state{NetworkListenerState::Binding}; /**< Current owner-thread state. */
        std::optional<NetworkLifecycleTerminal> terminal;          /**< First terminal result, once published. */
    };

    /** @brief Immutable connection lifecycle projection for one exact handle and operation generation. */
    struct NetworkConnectionSnapshot final {
        ConnectionHandle handle{};                                     /**< Exact current connection generation. */
        NetworkOperationGeneration operation{};                        /**< Exact asynchronous operation generation. */
        NetworkConnectionState state{NetworkConnectionState::Created}; /**< Current owner-thread state. */
        std::uint64_t deadlineTick{};                                  /**< Monotonic deadline; zero after ready/terminal. */
        std::optional<NetworkLifecycleTerminal> terminal;              /**< First terminal result, once published. */
    };

    /** @brief Setup-time finite capacities for one owner-thread lifecycle registry. */
    struct NetworkLifecycleLimits final {
        std::size_t maximumListeners{};   /**< Positive prepared listener slot-index range. */
        std::size_t maximumConnections{}; /**< Positive prepared connection slot-index range. */
    };

    /**
     * @brief Bounded listener and pre-session connection state owner.
     *
     * All methods are called by one owner thread after normalized I/O completion drain. Native callbacks retain
     * only Horo handles and operation generations. Storage is prepared by Create; admission, transitions, timeout
     * scans, cancellation, and shutdown do not grow it. Handle slots directly index prepared storage, and a slot may be
     * reused only by its exact next handle generation.
     */
    class NetworkLifecycleRegistry final {
    public:
        /** @brief Creates prepared lifecycle storage. @param limits Positive finite capacities. @return Registry or typed error. */
        [[nodiscard]] static Result<NetworkLifecycleRegistry> Create(const NetworkLifecycleLimits &limits);

        /** @brief Admits a listener bind operation. @param handle Exact owner-issued handle. @param operation Non-zero work generation.
         * @return Success or typed invalid, stale, capacity, or shutdown failure. */
        [[nodiscard]] Result<void> AdmitListener(ListenerHandle handle, NetworkOperationGeneration operation);
        /** @brief Publishes successful bind. @return Success or typed stale/transition failure. */
        [[nodiscard]] Result<void> MarkListening(ListenerHandle handle, NetworkOperationGeneration operation);
        /** @brief Requests idempotent bounded listener close. @return Success or typed stale failure. */
        [[nodiscard]] Result<void> RequestListenerClose(ListenerHandle handle, NetworkOperationGeneration operation);
        /** @brief Publishes the first listener terminal result. @return Success or typed stale/duplicate/transition failure. */
        [[nodiscard]] Result<void> CompleteListener(ListenerHandle handle, NetworkOperationGeneration operation,
                                                    NetworkLifecycleTerminal terminal);
        /** @brief Cancels an active listener with canonical typed evidence. @return Success or typed stale/duplicate failure. */
        [[nodiscard]] Result<void> CancelListener(ListenerHandle handle, NetworkOperationGeneration operation);

        /**
         * @brief Admits one asynchronous connection operation.
         * @param handle Exact owner-issued connection handle.
         * @param operation Non-zero work generation.
         * @param requiresResolution Whether the initial state is Resolving rather than Created.
         * @param deadlineTick Positive monotonic deadline owned by the caller's clock domain.
         * @return Success or typed invalid, stale, capacity, or shutdown failure.
         */
        [[nodiscard]] Result<void> AdmitConnection(ConnectionHandle handle, NetworkOperationGeneration operation, bool requiresResolution,
                                                   std::uint64_t deadlineTick);
        /** @brief Advances through Created/Resolving, Connecting, and AuthenticationReady. @return Success or typed stale/transition
         * failure. */
        [[nodiscard]] Result<void> AdvanceConnection(ConnectionHandle handle, NetworkOperationGeneration operation,
                                                     NetworkConnectionState next);
        /** @brief Requests idempotent bounded connection close. @return Success or typed stale failure. */
        [[nodiscard]] Result<void> RequestConnectionClose(ConnectionHandle handle, NetworkOperationGeneration operation);
        /** @brief Publishes the first connection terminal result. @return Success or typed stale/duplicate/transition failure. */
        [[nodiscard]] Result<void> CompleteConnection(ConnectionHandle handle, NetworkOperationGeneration operation,
                                                      NetworkLifecycleTerminal terminal);
        /** @brief Cancels active resolve/connect work with canonical typed evidence. @return Success or typed stale/duplicate failure. */
        [[nodiscard]] Result<void> CancelConnection(ConnectionHandle handle, NetworkOperationGeneration operation);

        /** @brief Expires every active connection whose deadline is reached. @param nowTick Monotonic current tick.
         * @return Number terminalized, bounded by configured connection capacity. */
        [[nodiscard]] Result<std::size_t> ExpireConnections(std::uint64_t nowTick);
        /** @brief Terminalizes all active records once and closes admission; idempotent. @return Number newly terminalized. */
        [[nodiscard]] Result<std::size_t> Shutdown();

        /** @brief Looks up an exact listener generation. @return Snapshot or typed stale failure. */
        [[nodiscard]] Result<NetworkListenerSnapshot> Listener(ListenerHandle handle) const;
        /** @brief Looks up an exact connection generation. @return Snapshot or typed stale failure. */
        [[nodiscard]] Result<NetworkConnectionSnapshot> Connection(ConnectionHandle handle) const;

        /** @brief Reports whether admission is permanently closed. @return True after first Shutdown. */
        [[nodiscard]] constexpr bool IsShuttingDown() const noexcept {
            return shuttingDown_;
        }

    private:
        struct ListenerEntry final {
            ListenerHandle handle{};
            NetworkOperationGeneration operation{};
            NetworkListenerState state{NetworkListenerState::Binding};
            std::optional<NetworkLifecycleTerminal> terminal;
        };

        struct ConnectionEntry final {
            ConnectionHandle handle{};
            NetworkOperationGeneration operation{};
            NetworkConnectionState state{NetworkConnectionState::Created};
            std::uint64_t deadlineTick{};
            std::optional<NetworkLifecycleTerminal> terminal;
        };

        NetworkLifecycleRegistry(std::vector<std::optional<ListenerEntry>> listeners,
                                 std::vector<std::optional<ConnectionEntry>> connections) noexcept;

        std::vector<std::optional<ListenerEntry>> listeners_;
        std::vector<std::optional<ConnectionEntry>> connections_;
        bool shuttingDown_{};
    };
}  // namespace Horo::Network
