#include "NetworkConsoleInspection.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Bounded ASCII output over closed enums and scalar counts; no history records or payload bytes are formatted. */
        class StatusText final {
        public:
            /** @brief Reserves only bounded explicit-command output storage. */
            explicit StatusText(const std::size_t limit) : limit_(limit) {
                text_.reserve(std::min(limit, std::size_t{512}));
            }

            /** @brief Appends finite safe text without exceeding the existing command byte budget. */
            void Append(const std::string_view value) {
                text_.append(value.substr(0, limit_ - text_.size()));
            }

            /** @brief Formats one unsigned public identity/count, independently of locale. */
            void Number(const std::uint64_t value) {
                std::array<char, 32> buffer;
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                if (result.ec == std::errc{})
                    Append({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
            }

            /** @brief Transfers the bounded owned presentation value. @return Safe command output. */
            [[nodiscard]] Debug::DebugCommandOutput Take() {
                return {std::move(text_)};
            }

        private:
            std::size_t limit_;
            std::string text_;
        };

        /** @brief Checks only the closed state/provenance and count fields consumed by this adapter. */
        bool Valid(const NetworkDebuggerProjection &projection) {
            const auto &s = projection.snapshot;
            if (projection.state > NetworkDebuggerState::Live || s.provider > Network::NetworkDiagnosticProvider::Other ||
                s.connections.size > s.connections.records.size() || s.replication.size > s.replication.records.size() ||
                s.rpc.size > s.rpc.records.size() || s.prediction.size > s.prediction.records.size() ||
                s.interest.size > s.interest.records.size() || s.capture.size > s.capture.records.size())
                return false;
            if (!s.attached || !s.source.Valid())
                return projection.state == NetworkDebuggerState::Detached;
            if (s.revision == 0)
                return false;
            if (!s.enabled)
                return projection.state == NetworkDebuggerState::Disabled;
            if (projection.state == NetworkDebuggerState::Live)
                return s.revision != 0 && s.publishedNanoseconds != 0 && !s.metrics.closed;
            return projection.state == NetworkDebuggerState::Stale;
        }

        /** @brief Retains the existing query capability; this handler never sees transport/control or mutable producer APIs. */
        class StatusCommand final : public Debug::ICommandHandler {
        public:
            /** @brief Retains the existing query and captures this binding's owner without querying it. */
            StatusCommand(std::shared_ptr<const INetworkDebuggerQuery> query, const std::size_t limit)
                : query_(std::move(query)), limit_(limit), owner_(std::this_thread::get_id()) {}

            /** @copydoc Debug::ICommandHandler::Execute */
            Result<Debug::DebugCommandOutput> Execute(const Debug::DebugCommandContext &, std::span<const Debug::ArgumentValue>,
                                                      const CancellationToken &cancellation, const JobExecutionContext *) override {
                if (std::this_thread::get_id() != owner_)
                    return Result<Debug::DebugCommandOutput>::Failure(Debug::MakeCommandError(Debug::CommandError::WrongThread));
                if (cancellation.IsCancellationRequested())
                    return Result<Debug::DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                const auto projection = ReadNetworkConsoleInspection(*query_);
                if (projection.HasError())
                    return Result<Debug::DebugCommandOutput>::Failure(projection.ErrorValue());
                static constexpr std::array<std::string_view, 4> states{"detached", "disabled", "stale", "live"};
                static constexpr std::array<std::string_view, 4> providers{"unavailable", "deterministic", "native", "other"};
                const auto &s = projection.Value().snapshot;
                StatusText text(limit_);
                text.Append("network diagnostics state=");
                text.Append(states[static_cast<std::size_t>(projection.Value().state)]);
                text.Append(" provider=");
                text.Append(providers[static_cast<std::size_t>(s.provider)]);
                text.Append(" revision=");
                text.Number(s.revision);
                text.Append(" process=");
                text.Number(s.source.process);
                text.Append(" session=");
                text.Number(s.source.session);
                text.Append(" scene=");
                text.Number(s.source.scene);
                text.Append(" generation=");
                text.Number(s.source.sceneGeneration);
                text.Append(" connections=");
                text.Number(s.connections.size);
                text.Append(" replication=");
                text.Number(s.replication.size);
                text.Append(" rpc=");
                text.Number(s.rpc.size);
                text.Append(" prediction=");
                text.Number(s.prediction.size);
                text.Append(" interest=");
                text.Number(s.interest.size);
                text.Append(" capture=");
                text.Number(s.capture.size);
                text.Append(" capture_dropped=");
                text.Number(s.capture.dropped);
                text.Append(" capture_paused=");
                text.Number(s.capturePaused ? 1 : 0);
                return Result<Debug::DebugCommandOutput>::Success(text.Take());
            }

        private:
            std::shared_ptr<const INetworkDebuggerQuery> query_;
            std::size_t limit_;
            std::thread::id owner_;
        };
    }  // namespace

    /** @copydoc ReadNetworkConsoleInspection */
    Result<NetworkDebuggerProjection> ReadNetworkConsoleInspection(const INetworkDebuggerQuery &query) {
        try {
            auto projection = query.Query();
            if (!Valid(projection))
                return Result<NetworkDebuggerProjection>::Failure(Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
            return Result<NetworkDebuggerProjection>::Success(std::move(projection));
        } catch (...) {
            return Result<NetworkDebuggerProjection>::Failure(Debug::MakeCommandError(Debug::CommandError::HandlerFailed));
        }
    }

    /** @copydoc NetworkConsoleStatusDescriptor */
    Debug::DebugCommandDescriptor NetworkConsoleStatusDescriptor() noexcept {
        return {NetworkConsoleStatusId,
                "net.debug_status",
                "Inspect bounded network diagnostic provenance",
                "net.debug_status",
                {},
                Debug::CommandPermission::Developer,
                Debug::CommandThreadPolicy::OwnerThreadNextFrame,
                Debug::CommandAvailability::NonShipping,
                Debug::CommandFlags::AuditLogged};
    }

    /** @copydoc BindNetworkConsoleInspection */
    Result<std::vector<Debug::CommandBinding>> BindNetworkConsoleInspection(std::shared_ptr<const Debug::CommandRegistry> registry,
                                                                            std::shared_ptr<const INetworkDebuggerQuery> query) {
        if (!registry)
            return Result<std::vector<Debug::CommandBinding>>::Failure(Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
        const auto resolved = registry->Resolve(NetworkConsoleStatusId, {15, true, true});
        if (resolved.HasError())
            return Result<std::vector<Debug::CommandBinding>>::Success({});
        const auto &descriptor = resolved.Value();
        if (descriptor.name != "net.debug_status" || descriptor.permissions != Debug::CommandPermission::Developer ||
            descriptor.threadPolicy != Debug::CommandThreadPolicy::OwnerThreadNextFrame ||
            descriptor.availability != Debug::CommandAvailability::NonShipping || !descriptor.arguments.empty() ||
            descriptor.flags != Debug::CommandFlags::AuditLogged)
            return Result<std::vector<Debug::CommandBinding>>::Failure(Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
        if (!query)
            return Result<std::vector<Debug::CommandBinding>>::Failure(Debug::MakeCommandError(Debug::CommandError::MissingCapability));
        std::vector<Debug::CommandBinding> bindings;
        bindings.push_back({NetworkConsoleStatusId, std::make_shared<StatusCommand>(std::move(query), registry->Limits().outputBytes)});
        return Result<std::vector<Debug::CommandBinding>>::Success(std::move(bindings));
    }
}  // namespace Horo::Application::Internal
