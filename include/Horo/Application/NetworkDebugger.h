#pragma once
/** @file NetworkDebugger.h
 * @brief Application query and command boundary for runtime network diagnostics.
 */
#include "Horo/Network/NetworkDebugger.h"

#include <chrono>

namespace Horo::Application {
    /** @brief Presentation provenance; only Live admits capture commands. */
    enum class NetworkDebuggerState : std::uint8_t {
        Detached,
        Disabled,
        Stale,
        Live
    };

    /** @brief Immutable evidence plus application-assessed source freshness. */
    struct NetworkDebuggerProjection final {
        Network::NetworkDebuggerSnapshot snapshot;
        NetworkDebuggerState state{NetworkDebuggerState::Detached};
    };

    /** @brief Read-only capability injected into the Net pane. */
    class INetworkDebuggerQuery {
    public:
        virtual ~INetworkDebuggerQuery() = default;
        /** @brief Returns current coherent evidence and provenance. @return Value projection, never mutable runtime state. */
        [[nodiscard]] virtual NetworkDebuggerProjection Query() const = 0;
    };

    /** @brief Typed application operations; no transport/runtime mutation is exposed to presentation. */
    class INetworkDebuggerControl {
    public:
        virtual ~INetworkDebuggerControl() = default;
        /** @brief Queues a bounded action only against current live evidence.
         * @param source Expected process/session/scene identity borrowed for this call; queued commands copy it. @param revision Expected
         * immutable revision.
         * @param action Capture operation. @return True when queued; false for stale/disabled/detached evidence or backpressure. */
        [[nodiscard]] virtual bool Request(const Network::NetworkDiagnosticSource &source, std::uint64_t revision,
                                           Network::NetworkCaptureAction action) = 0;
    };

    /** @brief Host-owned application bridge; producer lifetime is owned here and outlives composed adapters.
     * Source activation/publication/detachment occur on the host network owner thread. Query and Request
     * may run on the editor thread. Freshness uses monotonic time, never simulation ticks or wall time.
     */
    class NetworkDebuggerService final : public INetworkDebuggerQuery, public INetworkDebuggerControl {
    public:
        /** @brief Selects a finite freshness timeout. @param staleAfter Positive timeout; non-positive input fails closed as stale. */
        explicit NetworkDebuggerService(std::chrono::nanoseconds staleAfter = std::chrono::seconds{2});
        /** @brief Starts an exact scene activation; this host issues a fresh session identity.
         * @param scene Runtime scene identity. @param sceneGeneration Scene activation epoch.
         * @param enabled Actual network instrumentation election. @param provider Actual selected provider kind. @return Whether activation
         * succeeds. */
        [[nodiscard]] bool Begin(std::uint64_t scene, std::uint64_t sceneGeneration, bool enabled,
                                 Network::NetworkDiagnosticProvider provider = Network::NetworkDiagnosticProvider::Other);
        /** @brief Borrows the producer for explicit host composition. @return Collector owned by this service. */
        [[nodiscard]] Network::NetworkDebugger &Producer() noexcept;
        /** @brief Publishes with the process monotonic clock. @param source Exact active identity.
         * @param metrics Optional coherent owner metrics. @return Whether publication succeeds. */
        [[nodiscard]] bool Publish(const Network::NetworkDiagnosticSource &source, const Network::NetworkMetricSnapshot *metrics = nullptr);
        /** @copydoc INetworkDebuggerQuery::Query */
        [[nodiscard]] NetworkDebuggerProjection Query() const override;
        /** @copydoc INetworkDebuggerControl::Request */
        [[nodiscard]] bool Request(const Network::NetworkDiagnosticSource &source, std::uint64_t revision,
                                   Network::NetworkCaptureAction action) override;
        /** @brief Assesses immutable evidence using supplied monotonic time, for host adapters and deterministic tests.
         * @param snapshot Coherent producer evidence. @param nowNanoseconds Current process monotonic time.
         * @param staleAfterNanoseconds Positive maximum publication age. @return Explicit provenance state. */
        [[nodiscard]] static NetworkDebuggerState Assess(const Network::NetworkDebuggerSnapshot &snapshot, std::uint64_t nowNanoseconds,
                                                         std::uint64_t staleAfterNanoseconds) noexcept;

    private:
        Network::NetworkDebugger producer_;
        std::uint64_t staleAfter_{};
        std::uint64_t processGeneration_{};
        std::uint64_t nextSession_{1};
    };
}  // namespace Horo::Application
