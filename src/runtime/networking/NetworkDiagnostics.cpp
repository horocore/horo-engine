#include "Horo/Network/NetworkDiagnostics.h"

#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Network {
    namespace {
        [[nodiscard]] Error InvalidLogState() {
            return MakeError(NetworkErrors::NetworkLifecycleInvalid);
        }

        [[nodiscard]] Error StaleLogGeneration() {
            return MakeError(NetworkErrors::NetworkLifecycleOperationStale);
        }

        [[nodiscard]] constexpr bool ValidIdentity(const NetworkLogIdentity &identity) noexcept {
            return identity.hostOperation.IsValid() && identity.connection.IsValid() && identity.session.IsValid();
        }

        [[nodiscard]] bool MatchesIdentity(const NetworkTerminalRecord &terminal, const NetworkLogIdentity &identity) {
            return std::ranges::all_of(terminal.Context(), [&](const NetworkFailureContextEntry &entry) {
                if (entry.key == NetworkFailureContextKey::Connection &&
                    std::get<TransportHandleDiagnostic>(entry.value) != identity.connection.Diagnostic())
                    return false;
                if (entry.key == NetworkFailureContextKey::SessionGeneration &&
                    std::get<std::uint64_t>(entry.value) != identity.session.Value())
                    return false;
                return true;
            });
        }

        [[nodiscard]] constexpr const char *Category(const NetworkLogKind kind) noexcept {
            using enum NetworkLogKind;
            switch (kind) {
                case Failure:
                    return "network.security.failure";
                case SuppressedSummary:
                    return "network.security.suppressed";
                case SessionTerminal:
                    return "network.session.terminal";
            }
            return "network.diagnostic.invalid";
        }
    }  // namespace

    /** @copydoc NetworkLogPlayerTokenIssuer::Issue */
    Result<NetworkLogPlayerToken> NetworkLogPlayerTokenIssuer::Issue() {
        if (last_ == std::numeric_limits<std::uint64_t>::max())
            return Result<NetworkLogPlayerToken>::Failure(InvalidLogState());
        return Result<NetworkLogPlayerToken>::Success(NetworkLogPlayerToken{++last_});
    }

    /** @copydoc NetworkLogStream::Create */
    Result<NetworkLogStream> NetworkLogStream::Create(const NetworkLogIdentity &identity, const NetworkLogPolicy policy,
                                                      INetworkLogSink *sink) {
        if (!ValidIdentity(identity) || policy.windowTicks == 0 || policy.firstPerKind == 0 || policy.firstPerKind > 8 ||
            (policy.enabled && sink == nullptr))
            return Result<NetworkLogStream>::Failure(InvalidLogState());
        return Result<NetworkLogStream>::Success(NetworkLogStream{identity, policy, sink});
    }

    /** @brief Transfers sole publication ownership and disables the source stream. */
    NetworkLogStream::NetworkLogStream(NetworkLogStream &&other) noexcept
        : identity_(other.identity_), policy_(other.policy_), sink_(other.sink_), buckets_(std::move(other.buckets_)),
          lastTick_(other.lastTick_), sinkFailures_(other.sinkFailures_), finished_(other.finished_) {
        other.finished_ = true;
        other.sink_ = nullptr;
    }

    /** @brief Rejects stale generations and time reversal before any sink work. */
    Result<void> NetworkLogStream::Validate(const ConnectionHandle connection, const NetworkOperationGeneration session,
                                            const std::uint64_t tick) const {
        if (connection != identity_.connection || session != identity_.session)
            return Result<void>::Failure(StaleLogGeneration());
        if (tick == 0 || tick < lastTick_)
            return Result<void>::Failure(InvalidLogState());
        return Result<void>::Success();
    }

    /** @brief Keeps observability failures from changing gameplay/network outcomes. */
    void NetworkLogStream::Emit(const NetworkLogRecord &record) noexcept {
        if (!policy_.enabled)
            return;
        try {
            sink_->Emit(record);
        } catch (...) {
            // A failed sink cannot change the network outcome; retain bounded evidence for host qualification.
            if (sinkFailures_ != std::numeric_limits<std::uint64_t>::max())
                ++sinkFailures_;
        }
    }

    /** @brief Publishes one bounded count and latest tick, never a copy of hostile input. */
    void NetworkLogStream::EmitSummary(Bucket &bucket) {
        if (bucket.suppressed == 0 || !bucket.latest)
            return;
        NetworkLogRecord summary = *bucket.latest;
        summary.kind = NetworkLogKind::SuppressedSummary;
        summary.tick = bucket.windowStart;
        summary.latestTick = bucket.latestTick;
        summary.suppressedCount = bucket.suppressed;
        Emit(summary);
        bucket.suppressed = 0;
        bucket.latest.reset();
    }

    /** @copydoc NetworkLogStream::Failure */
    Result<void> NetworkLogStream::Failure(const ConnectionHandle observedConnection, const NetworkOperationGeneration observedSession,
                                           const NetworkTerminalRecord &terminal, const std::uint64_t tick) {
        if (auto valid = Validate(observedConnection, observedSession, tick); valid.HasError())
            return valid;
        if (finished_)
            return Result<void>::Failure(InvalidLogState());
        if (!MatchesIdentity(terminal, identity_))
            return Result<void>::Failure(StaleLogGeneration());
        lastTick_ = tick;
        if (!policy_.enabled)
            return Result<void>::Success();

        const auto index = static_cast<std::size_t>(terminal.Kind());
        if (index >= buckets_.size())
            return Result<void>::Failure(InvalidLogState());
        Bucket &bucket = buckets_[index];
        if (bucket.windowStart == 0 || tick - bucket.windowStart >= policy_.windowTicks) {
            EmitSummary(bucket);
            bucket = Bucket{};
            bucket.windowStart = tick;
        }

        const NetworkLogRecord record{NetworkLogKind::Failure,
                                      identity_,
                                      PeerSessionTerminalKind::Count,
                                      true,
                                      terminal.Layer(),
                                      terminal.Kind(),
                                      terminal.Disposition(),
                                      tick,
                                      tick,
                                      0,
                                      terminal.BackendEvidence()};
        if (bucket.emitted < policy_.firstPerKind) {
            ++bucket.emitted;
            Emit(record);
        } else {
            if (bucket.suppressed != std::numeric_limits<std::uint64_t>::max())
                ++bucket.suppressed;
            bucket.latestTick = tick;
            bucket.latest = record;
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkLogStream::Finish */
    Result<void> NetworkLogStream::Finish(const PeerSessionTerminalSnapshot &terminal) {
        if (auto valid = Validate(terminal.connection, terminal.sessionGeneration, terminal.terminalTick); valid.HasError())
            return valid;
        if (finished_ || terminal.kind == PeerSessionTerminalKind::Count)
            return Result<void>::Failure(InvalidLogState());
        if (terminal.failure && !MatchesIdentity(*terminal.failure, identity_))
            return Result<void>::Failure(StaleLogGeneration());
        lastTick_ = terminal.terminalTick;
        for (Bucket &bucket : buckets_)
            EmitSummary(bucket);
        finished_ = true;
        NetworkLogRecord record;
        record.kind = NetworkLogKind::SessionTerminal;
        record.identity = identity_;
        record.terminalKind = terminal.kind;
        record.tick = terminal.terminalTick;
        record.latestTick = terminal.terminalTick;
        if (terminal.failure) {
            record.hasFailure = true;
            record.layer = terminal.failure->Layer();
            record.failure = terminal.failure->Kind();
            record.disposition = terminal.failure->Disposition();
            record.backendEvidence = terminal.failure->BackendEvidence();
        }
        Emit(record);
        return Result<void>::Success();
    }

    /** @copydoc NetworkLogStream::Replace */
    Result<void> NetworkLogStream::Replace(const NetworkLogIdentity &next) {
        if (!finished_ || (policy_.enabled && sink_ == nullptr) || !ValidIdentity(next) ||
            next.connection.Slot() != identity_.connection.Slot() ||
            identity_.connection.Generation() == std::numeric_limits<std::uint32_t>::max() ||
            next.connection.Generation() != identity_.connection.Generation() + 1)
            return Result<void>::Failure(StaleLogGeneration());
        identity_ = next;
        buckets_ = {};
        lastTick_ = 0;
        sinkFailures_ = 0;
        finished_ = false;
        return Result<void>::Success();
    }

    /** @copydoc NetworkLogStream::Flush */
    Result<void> NetworkLogStream::Flush(const std::uint64_t tick) {
        if (tick == 0 || tick < lastTick_)
            return Result<void>::Failure(InvalidLogState());
        lastTick_ = tick;
        if (finished_ || !policy_.enabled)
            return Result<void>::Success();
        for (Bucket &bucket : buckets_) {
            if (bucket.windowStart != 0 && tick - bucket.windowStart >= policy_.windowTicks) {
                EmitSummary(bucket);
                bucket = Bucket{};
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkTelemetryLogSink::Emit */
    void NetworkTelemetryLogSink::Emit(const NetworkLogRecord &record) {
        constexpr auto level = Log::Level::Warn;
        const char *category = Category(record.kind);
        if (!Log::Logger::IsEnabled(level) || !Telemetry::Runtime::IsEventEnabled(category, level))
            return;
        std::vector<Telemetry::Field> fields;
        fields.reserve(16);
        const auto add = [&fields](const char *key, const std::uint64_t value) {
            fields.emplace_back(key, value, Telemetry::FieldPrivacy::Public);
        };
        add("host_operation", record.identity.hostOperation.Value());
        add("connection_slot", record.identity.connection.Slot());
        add("connection_generation", record.identity.connection.Generation());
        add("session_generation", record.identity.session.Value());
        if (record.identity.scene.IsValid())
            add("scene_runtime", record.identity.scene.value);
        if (record.identity.playerToken.IsValid())
            add("player_token", record.identity.playerToken.Value());
        add("tick", record.tick);
        if (record.kind == NetworkLogKind::SessionTerminal)
            add("terminal_kind", static_cast<std::uint64_t>(record.terminalKind));
        if (record.hasFailure) {
            add("layer", static_cast<std::uint64_t>(record.layer));
            add("failure_kind", static_cast<std::uint64_t>(record.failure));
            add("disposition", static_cast<std::uint64_t>(record.disposition));
        }
        if (record.kind == NetworkLogKind::SuppressedSummary) {
            add("suppressed_count", record.suppressedCount);
            add("latest_tick", record.latestTick);
        }
        if (record.backendEvidence.observed) {
            add("backend_detail_bytes_bounded", record.backendEvidence.observedBytes);
            fields.emplace_back("backend_detail_truncated", record.backendEvidence.truncated, Telemetry::FieldPrivacy::Public);
            fields.emplace_back("backend_detail_malformed", record.backendEvidence.malformed, Telemetry::FieldPrivacy::Public);
        }
        Telemetry::Record envelope;
        envelope.subsystem = category;
        envelope.payload = Telemetry::LogRecord{level, category, "Network diagnostic", std::move(fields)};
        static_cast<void>(Telemetry::Runtime::EmitRecord(std::move(envelope)));
    }
}  // namespace Horo::Network
