#pragma once

/** @file NetworkConsoleOverlay.h @brief Non-installed bounded display summary over the existing application network query. */
#include "NetworkConsoleInspection.h"

#include <array>
#include <chrono>
#include <optional>
#include <thread>

namespace Horo::Application::Internal {
    /** @brief Copied provenance and scalar counts only; no histories, payloads, addresses or mutable networking state. */
    struct NetworkConsoleSummary final {
        Network::NetworkDiagnosticSource source;
        NetworkDebuggerState state{NetworkDebuggerState::Detached};
        Network::NetworkDiagnosticProvider provider{Network::NetworkDiagnosticProvider::Unavailable};
        std::uint64_t publicationRevision{};
        std::array<std::uint64_t, 6> records{}; /**< Connection, replication, RPC, prediction, interest and capture counts. */
        std::uint64_t captureDropped{};
        bool capturePaused{};
        bool operator==(const NetworkConsoleSummary &) const = default;
    };

    /** @brief Owner-thread four-Hz display projection; hidden instances never query or activate networking. */
    class NetworkConsoleOverlay final {
    public:
        /** @brief Retains only an explicitly supplied existing query and captures this display owner; queries no service.
         * @param query Existing application owner. @param profile Actual graphical product profile.
         * @param principal Existing host grants. @return Projection or typed missing/denied capability; Server/Shipping have no overlay.
         */
        [[nodiscard]] static Result<NetworkConsoleOverlay> Create(std::shared_ptr<const INetworkDebuggerQuery> query,
                                                                  Debug::ProductProfile profile,
                                                                  const Debug::DebugCommandContext &principal);
        /** @brief Applies actual presentation eligibility; hiding drops copied data and cadence without a query.
         * @param visible Actual visibility/focus admission. @return Success or wrong-owner failure before mutation.
         */
        [[nodiscard]] Result<void> SetVisible(bool visible);
        /** @brief Reads at most once per 250 ms while visible; failed reads retain the last valid summary without partial mutation.
         * @param now Existing host monotonic time. @return Whether the display revision changed, or typed query/owner failure.
         * No private store/producer is created. Networking freshness remains assessed by the actual application query.
         */
        [[nodiscard]] Result<bool> Refresh(std::chrono::steady_clock::time_point now);
        /** @brief Borrows the copied scalar display state. @return Empty before the first visible refresh or after hiding.
         * @pre Owner thread; invalidated by refresh/hide/destruction. Retains no Scene/session/transport state.
         */
        [[nodiscard]] const std::optional<NetworkConsoleSummary> &Snapshot() const noexcept;
        /** @brief Retains the last actual query failure until a successful read or hide; throttled frames preserve it.
         * @return Original typed failure with sanitized query context, or empty. @pre Owner thread.
         */
        [[nodiscard]] const std::optional<Error> &RefreshFailure() const noexcept;
        /** @brief Returns scalar-summary invalidation identity. @return Monotonic revision; unchanged refreshes preserve it. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;

    private:
        /** @brief Retains the explicit immutable query capability without calling it. */
        explicit NetworkConsoleOverlay(std::shared_ptr<const INetworkDebuggerQuery> query);
        std::shared_ptr<const INetworkDebuggerQuery> query_;
        std::thread::id owner_;
        std::optional<NetworkConsoleSummary> snapshot_;
        std::optional<Error> refreshFailure_;
        std::optional<std::chrono::steady_clock::time_point> nextRefresh_;
        std::uint64_t revision_{};
        bool visible_{};
    };
}  // namespace Horo::Application::Internal
