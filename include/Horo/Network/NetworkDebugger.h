#pragma once
/** @file NetworkDebugger.h
 * @brief Bounded immutable network diagnostic publication, with exact host lifetime fences.
 */
#include "Horo/Network/NetworkMetrics.h"
#include "Horo/Network/NetworkTransport.h"

#include <array>
#include <mutex>
#include <thread>

namespace Horo::Network {
    /** @brief Host-issued identities; sceneGeneration distinguishes repeated activation of the same scene. */
    struct NetworkDiagnosticSource final {
        std::uint64_t process{};
        std::uint64_t session{};
        std::uint64_t scene{};
        std::uint64_t sceneGeneration{};
        /** @brief Checks that every host lifetime dimension is present. @return True for a fully stamped source. */
        [[nodiscard]] bool Valid() const noexcept;
        bool operator==(const NetworkDiagnosticSource &) const = default;
    };
    /** @brief Closed evidence categories; payloads, addresses and credentials are never retained. */
    enum class NetworkCaptureKind : std::uint8_t {
        Connection,
        Replication,
        Rpc,
        Prediction,
        Interest,
        Count
    };

    /** @brief One sanitized transport event, not an admission or gameplay authority grant. */
    struct NetworkConnectionRecord final {
        ConnectionHandle connection{};
        NetworkTransportEventKind event{NetworkTransportEventKind::Failed};
        std::uint64_t bytes{};
        std::uint64_t peerSessionGeneration{}; /**< Zero for transport-only evidence. */
        bool gameplayAdmitted{};               /**< True only after the real session owner publishes Active. */
    };

    /** @brief Actual committed mapping or state-copy totals; never inferred wire replication. */
    struct NetworkReplicationRecord final {
        std::uint64_t registered{}, retired{}, considered{}, published{}, failed{}, deferred{};
    };

    /** @brief Exactly-once execution accounting from an admitted RPC dispatcher. */
    struct NetworkRpcRecord final {
        std::uint64_t accepted{}, succeeded{}, failed{}, cancelled{};
    };

    /** @brief Measured timing projection; does not claim rollback or replay is implemented. */
    struct NetworkPredictionRecord final {
        std::uint64_t localTick{}, serverTick{}, sampleAgeTicks{};
        bool hasMapping{}, stale{};
    };

    /** @brief Owner-provided interest accounting; unavailable until an actual scheduler publishes evidence. */
    struct NetworkInterestRecord final {
        std::uint64_t considered{}, relevant{}, deferred{};
    };

    /** @brief Fixed metadata-only capture entry; no peer-controlled text or payload bytes. */
    struct NetworkCaptureRecord final {
        std::uint64_t sequence{};
        NetworkCaptureKind kind{NetworkCaptureKind::Connection};
        ConnectionHandle connection{};
        std::uint64_t amount{};
    };

    /** @brief Finite oldest-first retained history; overflow drops the incoming record and increments dropped. */
    template <typename T, std::size_t Capacity> struct NetworkDiagnosticHistory final {
        std::array<T, Capacity> records{};
        std::size_t size{};
        std::uint64_t dropped{};
    };
    /** @brief Explicit host-selected provider evidence; in-memory measurements never qualify native sockets. */
    enum class NetworkDiagnosticProvider : std::uint8_t {
        Unavailable,
        Deterministic,
        Native,
        Other
    };

    /** @brief Complete safe-point copy. Retaining this value never retains a runtime owner or mutable state. */
    struct NetworkDebuggerSnapshot final {
        NetworkDiagnosticSource source{};
        NetworkDiagnosticProvider provider{NetworkDiagnosticProvider::Unavailable};
        std::uint64_t revision{}, publishedNanoseconds{};
        NetworkMetricSnapshot metrics{};
        NetworkDiagnosticHistory<NetworkConnectionRecord, 32> connections;
        NetworkDiagnosticHistory<NetworkReplicationRecord, 32> replication;
        NetworkDiagnosticHistory<NetworkRpcRecord, 32> rpc;
        NetworkDiagnosticHistory<NetworkPredictionRecord, 32> prediction;
        NetworkDiagnosticHistory<NetworkInterestRecord, 32> interest;
        NetworkDiagnosticHistory<NetworkCaptureRecord, 256> capture;
        bool attached{}, enabled{}, capturePaused{};
    };
    /** @brief Typed command stamped with the immutable projection the user acted on. */
    enum class NetworkCaptureAction : std::uint8_t {
        Pause,
        Resume,
        Clear
    };

    /** @brief Single-owner producer and bounded cross-thread publication/command mailbox.
     * All recording and lifecycle methods run on the constructing thread. Readers copy only the
     * published value under a short mutex; commands enter an eight-slot mailbox under that mutex.
     * Shutdown detaches before dependencies die. Producer adapters cache their source at binding,
     * so old callbacks cannot stamp themselves as a replacement session. No recording allocates.
     */
    class NetworkDebugger final {
    public:
        NetworkDebugger();
        NetworkDebugger(const NetworkDebugger &) = delete;
        NetworkDebugger &operator=(const NetworkDebugger &) = delete;
        /** @brief Begins a fresh host lifetime, clearing retained data. @param source Valid host identity with a strictly newer
         * process/session/scene-generation tuple.
         * @param enabled Host instrumentation election. @param provider Actual host-selected provider kind. @return False on wrong thread
         * or invalid/reused identity. */
        [[nodiscard]] bool Begin(NetworkDiagnosticSource source, bool enabled,
                                 NetworkDiagnosticProvider provider = NetworkDiagnosticProvider::Other);
        /** @brief Returns the bound source on the owner thread. @return Exact source or absent on another thread. */
        [[nodiscard]] NetworkDiagnosticSource Source() const noexcept;
        /** @brief Records a sanitized actual transport event. @param source Cached producer identity.
         * @param record Actual event evidence. @return Whether admitted by the generation/collection gate. */
        [[nodiscard]] bool Observe(NetworkDiagnosticSource source, const NetworkConnectionRecord &record) noexcept;
        /** @brief Records actual committed replication accounting. @param source Cached producer identity.
         * @param record Owner report. @return Whether admitted. */
        [[nodiscard]] bool Observe(NetworkDiagnosticSource source, const NetworkReplicationRecord &record) noexcept;
        /** @brief Records actual RPC accounting. @param source Cached producer identity.
         * @param record Owner report. @return Whether admitted. */
        [[nodiscard]] bool Observe(NetworkDiagnosticSource source, const NetworkRpcRecord &record) noexcept;
        /** @brief Records timing evidence. @param source Cached producer identity.
         * @param record Owner report. @return Whether admitted. */
        [[nodiscard]] bool Observe(NetworkDiagnosticSource source, const NetworkPredictionRecord &record) noexcept;
        /** @brief Records actual interest accounting. @param source Cached producer identity.
         * @param record Owner report. @return Whether admitted. */
        [[nodiscard]] bool Observe(NetworkDiagnosticSource source, const NetworkInterestRecord &record) noexcept;
        /** @brief Publishes at an owner safe point and consumes bounded typed commands.
         * @param source Exact producer lifetime. @param nowNanoseconds Monotonic host time.
         * @param metrics Optional already-published metric evidence. @return False on invalid/stale input. */
        [[nodiscard]] bool Publish(NetworkDiagnosticSource source, std::uint64_t nowNanoseconds,
                                   const NetworkMetricSnapshot *metrics = nullptr) noexcept;
        /** @brief Copies only the coherent last publication. @return Immutable value projection. */
        [[nodiscard]] NetworkDebuggerSnapshot Snapshot() const;
        /** @brief Queues one action against an exact currently published revision.
         * @param source Expected lifetime. @param revision Expected revision. @param action Closed action.
         * @return False if detached, disabled, stale, invalid or mailbox full. */
        [[nodiscard]] bool Request(NetworkDiagnosticSource source, std::uint64_t revision, NetworkCaptureAction action);
        /** @brief Detaches and publishes final evidence before producer dependencies are released. */
        void Detach() noexcept;

    private:
        struct Command {
            NetworkDiagnosticSource source;
            NetworkCaptureAction action;
        };

        [[nodiscard]] bool Admits(NetworkDiagnosticSource source) const noexcept;
        void Capture(NetworkCaptureKind kind, ConnectionHandle connection = {}, std::uint64_t amount = 0) noexcept;
        const std::thread::id owner_;
        NetworkDebuggerSnapshot current_;
        mutable std::mutex mutex_;
        NetworkDebuggerSnapshot published_;
        std::array<Command, 8> commands_{};
        std::size_t commandCount_{};
        std::uint64_t sequence_{};
    };
}  // namespace Horo::Network
