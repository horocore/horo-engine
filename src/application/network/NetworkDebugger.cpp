#include "Horo/Application/NetworkDebugger.h"

namespace Horo::Application {
    namespace {
        /** @brief Samples the process steady clock for publication age only. */
        std::uint64_t Now() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }  // namespace

    /** @copydoc NetworkDebuggerService::NetworkDebuggerService */
    NetworkDebuggerService::NetworkDebuggerService(const std::chrono::nanoseconds staleAfter)
        : staleAfter_(staleAfter.count() > 0 ? static_cast<std::uint64_t>(staleAfter.count()) : 0), processGeneration_(Now()) {}

    /** @copydoc NetworkDebuggerService::Begin */
    bool NetworkDebuggerService::Begin(const std::uint64_t scene, const std::uint64_t sceneGeneration, const bool enabled,
                                       const Network::NetworkDiagnosticProvider provider) {
        if (nextSession_ == 0)
            return false;
        const bool begun = producer_.Begin({processGeneration_, nextSession_, scene, sceneGeneration}, enabled, provider);
        if (begun)
            ++nextSession_;
        return begun;
    }

    /** @copydoc NetworkDebuggerService::Producer */
    Network::NetworkDebugger &NetworkDebuggerService::Producer() noexcept {
        return producer_;
    }

    /** @copydoc NetworkDebuggerService::Publish */
    bool NetworkDebuggerService::Publish(const Network::NetworkDiagnosticSource source, const Network::NetworkMetricSnapshot *metrics) {
        return producer_.Publish(source, Now(), metrics);
    }

    /** @copydoc NetworkDebuggerService::Assess */
    NetworkDebuggerState NetworkDebuggerService::Assess(const Network::NetworkDebuggerSnapshot &snapshot, const std::uint64_t now,
                                                        const std::uint64_t staleAfter) noexcept {
        if (!snapshot.attached || !snapshot.source.Valid())
            return NetworkDebuggerState::Detached;
        if (!snapshot.enabled)
            return NetworkDebuggerState::Disabled;
        if (snapshot.publishedNanoseconds == 0 || now < snapshot.publishedNanoseconds || staleAfter == 0 ||
            now - snapshot.publishedNanoseconds > staleAfter || snapshot.metrics.closed)
            return NetworkDebuggerState::Stale;
        return NetworkDebuggerState::Live;
    }

    /** @copydoc NetworkDebuggerService::Query */
    NetworkDebuggerProjection NetworkDebuggerService::Query() const {
        auto snapshot = producer_.Snapshot();
        return {snapshot, Assess(snapshot, Now(), staleAfter_)};
    }

    /** @copydoc NetworkDebuggerService::Request */
    bool NetworkDebuggerService::Request(const Network::NetworkDiagnosticSource source, const std::uint64_t revision,
                                         const Network::NetworkCaptureAction action) {
        if (Query().state != NetworkDebuggerState::Live)
            return false;
        return producer_.Request(source, revision, action);
    }
}  // namespace Horo::Application
