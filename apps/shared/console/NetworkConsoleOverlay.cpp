#include "NetworkConsoleOverlay.h"

namespace Horo::Application::Internal {
    /** @copydoc NetworkConsoleOverlay::NetworkConsoleOverlay */
    NetworkConsoleOverlay::NetworkConsoleOverlay(std::shared_ptr<const INetworkDebuggerQuery> query)
        : query_(std::move(query)), owner_(std::this_thread::get_id()) {}

    /** @copydoc NetworkConsoleOverlay::Create */
    Result<NetworkConsoleOverlay> NetworkConsoleOverlay::Create(std::shared_ptr<const INetworkDebuggerQuery> query,
                                                                const Debug::ProductProfile profile,
                                                                const Debug::DebugCommandContext &principal) {
        if (!query || (profile != Debug::ProductProfile::Editor && profile != Debug::ProductProfile::Development &&
                       profile != Debug::ProductProfile::Profile && profile != Debug::ProductProfile::Diagnostics))
            return Result<NetworkConsoleOverlay>::Failure(Debug::MakeCommandError(Debug::CommandError::MissingCapability));
        if (!Debug::IsConsolePermissionGranted(Debug::CommandPermission::Developer, principal, profile))
            return Result<NetworkConsoleOverlay>::Failure(Debug::MakeCommandError(Debug::CommandError::PermissionDenied));
        return Result<NetworkConsoleOverlay>::Success(NetworkConsoleOverlay{std::move(query)});
    }

    /** @copydoc NetworkConsoleOverlay::SetVisible */
    Result<void> NetworkConsoleOverlay::SetVisible(const bool visible) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(Debug::MakeCommandError(Debug::CommandError::WrongThread));
        if (visible_ == visible)
            return Result<void>::Success();
        visible_ = visible;
        nextRefresh_.reset();
        if (!visible) {
            refreshFailure_.reset();
            if (snapshot_) {
                snapshot_.reset();
                ++revision_;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkConsoleOverlay::Refresh */
    Result<bool> NetworkConsoleOverlay::Refresh(const std::chrono::steady_clock::time_point now) {
        if (std::this_thread::get_id() != owner_)
            return Result<bool>::Failure(Debug::MakeCommandError(Debug::CommandError::WrongThread));
        if (!visible_ || (nextRefresh_ && now < *nextRefresh_))
            return Result<bool>::Success(false);
        nextRefresh_ = now + std::chrono::milliseconds{250};
        const auto read = ReadNetworkConsoleInspection(*query_);
        if (read.HasError()) {
            refreshFailure_ = read.ErrorValue();
            return Result<bool>::Failure(read.ErrorValue());
        }
        refreshFailure_.reset();
        const auto &projection = read.Value();
        const auto &s = projection.snapshot;
        const NetworkConsoleSummary summary{s.source,
                                            projection.state,
                                            s.provider,
                                            s.revision,
                                            {s.connections.size, s.replication.size, s.rpc.size, s.prediction.size, s.interest.size,
                                             s.capture.size},
                                            s.capture.dropped,
                                            s.capturePaused};
        if (snapshot_ && *snapshot_ == summary)
            return Result<bool>::Success(false);
        snapshot_ = summary;
        ++revision_;
        return Result<bool>::Success(true);
    }

    /** @copydoc NetworkConsoleOverlay::Snapshot */
    const std::optional<NetworkConsoleSummary> &NetworkConsoleOverlay::Snapshot() const noexcept {
        return snapshot_;
    }

    /** @copydoc NetworkConsoleOverlay::RefreshFailure */
    const std::optional<Error> &NetworkConsoleOverlay::RefreshFailure() const noexcept {
        return refreshFailure_;
    }

    /** @copydoc NetworkConsoleOverlay::Revision */
    std::uint64_t NetworkConsoleOverlay::Revision() const noexcept {
        return revision_;
    }
}  // namespace Horo::Application::Internal
