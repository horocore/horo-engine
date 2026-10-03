#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Network/NetworkDiagnostics.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Network {
    namespace {
        struct Probe final : INetworkLogSink {
            std::vector<NetworkLogRecord> records;

            void Emit(const NetworkLogRecord &record) override {
                records.push_back(record);
            }
        };

        struct ThrowingSink final : INetworkLogSink {
            void Emit(const NetworkLogRecord &) override {
                throw std::runtime_error("sink failed");
            }
        };

        class CollectingSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
                std::lock_guard lock(mutex);
                records.push_back(record);
            }

            void Flush() override {}

            std::mutex mutex;
            std::vector<Telemetry::Record> records;
        };

        class LogFixture final {
        public:
            LogFixture()
                : directory(std::filesystem::temp_directory_path() /
                            ("horo-network-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
                std::filesystem::create_directories(directory);
            }

            ~LogFixture() {
                Log::Logger::Shutdown();
                std::error_code ignored;
                std::filesystem::remove_all(directory, ignored);
            }

            std::filesystem::path directory;
        };

        [[nodiscard]] NetworkLogIdentity Identity(const std::uint32_t generation = 1, const std::uint64_t session = 7) {
            NetworkLogPlayerTokenIssuer issuer;
            return {NetworkOperationGeneration::Create(5).Value(), ConnectionHandle::Create(2, generation).Value(),
                    NetworkOperationGeneration::Create(session).Value(), Runtime::SceneRuntimeId{11}, issuer.Issue().Value()};
        }

        [[nodiscard]] NetworkTerminalRecord Malformed() {
            return MakeNetworkTerminalRecord(NetworkFailureLayer::Protocol, NetworkFailureKind::ProtocolMalformed).Value();
        }

        [[nodiscard]] PeerSessionTerminalSnapshot Terminal(const std::uint64_t tick, const std::uint32_t connectionGeneration = 1,
                                                           const std::uint64_t sessionGeneration = 7) {
            return {Identity(connectionGeneration, sessionGeneration).connection,
                    Identity(connectionGeneration, sessionGeneration).session,
                    PeerSessionTerminalKind::ProtocolRejected,
                    {},
                    tick,
                    Malformed()};
        }

        [[nodiscard]] NetworkLogStream Stream(Probe &probe, const NetworkLogPolicy policy = {10, 2, true}) {
            return NetworkLogStream::Create(Identity(), policy, &probe).Value();
        }

        /** @brief Waits for bounded best-effort ingestion before asserting the adapter's persisted content. */
        [[nodiscard]] bool EmitAcceptedDiagnostic(NetworkTelemetryLogSink &sink, const NetworkLogRecord &record) {
            const auto acceptedBefore = Telemetry::Runtime::GetStatistics().acceptedRecords;
            // A producer may lose the queue try-lock to the writer even when the queue is empty.
            for (unsigned attempt = 0; attempt < 1000; ++attempt) {
                sink.Emit(record);
                if (Telemetry::Runtime::GetStatistics().acceptedRecords > acceptedBefore)
                    return true;
                std::this_thread::yield();
            }
            return false;
        }
    }  // namespace

    TEST_CASE("Host player log pseudonyms are issued without principal input", "[network][diagnostics]") {
        NetworkLogPlayerTokenIssuer issuer;
        const auto first = issuer.Issue();
        const auto second = issuer.Issue();
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().IsValid());
        REQUIRE(second.Value().Value() != first.Value().Value());
        static_assert(!std::is_copy_constructible_v<NetworkLogPlayerTokenIssuer>);
    }

    TEST_CASE("Network diagnostics reject malformed configuration and stale generations", "[network][diagnostics]") {
        Probe probe;
        REQUIRE(NetworkLogStream::Create({}, {10, 1, true}, &probe).HasError());
        REQUIRE(NetworkLogStream::Create(Identity(), {0, 1, true}, &probe).HasError());
        REQUIRE(NetworkLogStream::Create(Identity(), {10, 0, true}, &probe).HasError());
        REQUIRE(NetworkLogStream::Create(Identity(), {10, 9, true}, &probe).HasError());
        REQUIRE(NetworkLogStream::Create(Identity(), {10, 1, true}, nullptr).HasError());

        auto stream = Stream(probe);
        const auto failure = Malformed();
        REQUIRE(stream.Failure(Identity(2).connection, Identity().session, failure, 1).HasError());
        REQUIRE(stream.Failure(Identity().connection, Identity(1, 8).session, failure, 1).HasError());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 0).HasError());
        const std::array mismatched{NetworkFailureContextEntry{NetworkFailureContextKey::Connection, Identity(2).connection.Diagnostic()},
                                    NetworkFailureContextEntry{NetworkFailureContextKey::SessionGeneration, Identity().session.Value()}};
        const auto misattributed =
            MakeNetworkTerminalRecord(NetworkFailureLayer::Protocol, NetworkFailureKind::ProtocolMalformed, mismatched);
        REQUIRE(misattributed.HasValue());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, misattributed.Value(), 1).HasError());
        REQUIRE(probe.records.empty());
    }

    TEST_CASE("Peer-controlled malformed traffic is bounded per kind and window", "[network][diagnostics]") {
        Probe probe;
        auto stream = Stream(probe);
        const auto failure = Malformed();
        for (std::uint64_t tick = 1; tick <= 1000; ++tick)
            REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, tick).HasValue());
        // 100 windows, at most two ordinary records and one aggregate per window.
        REQUIRE(probe.records.size() <= 300);
        REQUIRE(probe.records[0].kind == NetworkLogKind::Failure);
        REQUIRE(probe.records[1].kind == NetworkLogKind::Failure);
        REQUIRE(probe.records[2].kind == NetworkLogKind::SuppressedSummary);
        REQUIRE(probe.records[2].suppressedCount == 8);
        REQUIRE(probe.records[2].latestTick == 10);
        REQUIRE(probe.records[2].identity.connection.Generation() == 1);
        REQUIRE(stream.Flush(1011).HasValue());
        REQUIRE(probe.records.back().kind == NetworkLogKind::SuppressedSummary);
    }

    TEST_CASE("Terminal publication fences late callbacks and replacement", "[network][diagnostics]") {
        Probe probe;
        auto stream = Stream(probe);
        const auto failure = Malformed();
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 1).HasValue());
        REQUIRE(stream.Finish(Terminal(2)).HasValue());
        REQUIRE(stream.Finish(Terminal(3)).HasError());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 3).HasError());
        REQUIRE(stream.Replace(Identity(3, 8)).HasError());
        REQUIRE(stream.Replace(Identity(2, 1)).HasValue());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 4).HasError());
        REQUIRE(stream.Failure(Identity(2, 1).connection, Identity(2, 1).session, failure, 1).HasValue());
        REQUIRE(probe.records.size() == 3);
        REQUIRE(probe.records[1].kind == NetworkLogKind::SessionTerminal);
        REQUIRE(probe.records[2].identity.connection.Generation() == 2);
        REQUIRE(probe.records[2].identity.session.Value() == 1);
    }

    TEST_CASE("Disabled network instrumentation does not call a sink or change terminal semantics", "[network][diagnostics]") {
        auto stream = NetworkLogStream::Create(Identity(), {10, 1, false}, nullptr).Value();
        const auto failure = Malformed();
        for (std::uint64_t tick = 1; tick <= 100; ++tick)
            REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, tick).HasValue());
        REQUIRE(stream.Finish(Terminal(101)).HasValue());
        REQUIRE(stream.IsFinished());
        Probe probe;
        auto gated = Stream(probe, {10, 1, false});
        REQUIRE(gated.Failure(Identity().connection, Identity().session, failure, 1).HasValue());
        REQUIRE(probe.records.empty());
    }

    TEST_CASE("Private backend text cannot enter the structured network record", "[network][diagnostics]") {
        Probe probe;
        auto stream = Stream(probe);
        constexpr std::string_view hostile = "password=private\nBearer secret\xff";
        const auto normalized =
            NormalizePrivateBackendFailure(NetworkFailureLayer::Transport, NetworkFailureKind::TransportUnavailable, hostile);
        REQUIRE(normalized.HasValue());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, normalized.Value(), 1).HasValue());
        REQUIRE(probe.records.size() == 1);
        REQUIRE(probe.records[0].layer == NetworkFailureLayer::Transport);
        REQUIRE(probe.records[0].backendEvidence.malformed);
        REQUIRE(probe.records[0].identity.scene.value == 11);
        REQUIRE(probe.records[0].identity.playerToken.IsValid());
        static_assert(!std::is_constructible_v<NetworkLogPlayerToken, std::uint64_t>);
        static_assert(!std::is_constructible_v<NetworkLogRecord, std::string>);
    }

    TEST_CASE("Suppression summary and clock reversal do not publish hostile records", "[network][diagnostics]") {
        Probe probe;
        auto stream = Stream(probe, {std::numeric_limits<std::uint64_t>::max(), 1, true});
        const auto failure = Malformed();
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 5).HasValue());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 6).HasValue());
        REQUIRE(stream.Flush(4).HasError());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, failure, 4).HasError());
        REQUIRE(stream.Finish(Terminal(7)).HasValue());
        REQUIRE(probe.records.size() == 3);
        REQUIRE(probe.records[1].suppressedCount == 1);
    }

    TEST_CASE("Graceful session terminal omits failure evidence", "[network][diagnostics]") {
        Probe probe;
        auto stream = Stream(probe);
        PeerSessionTerminalSnapshot closed{Identity().connection, Identity().session, PeerSessionTerminalKind::LocalClose, {}, 3, {}};
        REQUIRE(stream.Finish(closed).HasValue());
        REQUIRE(probe.records.size() == 1);
        REQUIRE(probe.records[0].kind == NetworkLogKind::SessionTerminal);
        REQUIRE(probe.records[0].terminalKind == PeerSessionTerminalKind::LocalClose);
        REQUIRE_FALSE(probe.records[0].hasFailure);
    }

    TEST_CASE("Moving a network log stream transfers sole publication ownership", "[network][diagnostics]") {
        Probe probe;
        auto original = Stream(probe);
        auto owner = std::move(original);
        const auto failure = Malformed();
        REQUIRE(original.Failure(Identity().connection, Identity().session, failure, 1).HasError());
        REQUIRE(original.Replace(Identity(2, 8)).HasError());
        REQUIRE(owner.Failure(Identity().connection, Identity().session, failure, 1).HasValue());
        REQUIRE(probe.records.size() == 1);
    }

    TEST_CASE("Observability sink failure does not change a terminal result", "[network][diagnostics]") {
        ThrowingSink sink;
        auto stream = NetworkLogStream::Create(Identity(), {10, 1, true}, &sink).Value();
        REQUIRE(stream.Failure(Identity().connection, Identity().session, Malformed(), 1).HasValue());
        REQUIRE(stream.Finish(Terminal(2)).HasValue());
        REQUIRE(stream.IsFinished());
        REQUIRE(stream.SinkFailureCount() == 2);
        REQUIRE(stream.Replace(Identity(2, 1)).HasValue());
        REQUIRE(stream.SinkFailureCount() == 0);
        REQUIRE(stream.Failure(Identity(2, 1).connection, Identity(2, 1).session, Malformed(), 1).HasValue());
        REQUIRE(stream.SinkFailureCount() == 1);
    }

    TEST_CASE("Foundation logger adapter exports only allowlisted numeric correlation", "[network][diagnostics]") {
        Log::Logger::Shutdown();
        LogFixture fixture;
        const auto sink = std::make_shared<CollectingSink>();
        Log::LoggerConfiguration configuration;
        configuration.logDirectory = fixture.directory;
        configuration.baseName = "network";
        configuration.additionalSinks.push_back(sink);
        configuration.echoToStderr = false;
        REQUIRE(Log::Logger::Init(configuration));
        NetworkTelemetryLogSink networkSink;
        Probe probe;
        auto stream = Stream(probe);
        constexpr std::string_view hostile = "Bearer private-password account-name";
        const auto normalized =
            NormalizePrivateBackendFailure(NetworkFailureLayer::Transport, NetworkFailureKind::TransportUnavailable, hostile);
        REQUIRE(normalized.HasValue());
        REQUIRE(stream.Failure(Identity().connection, Identity().session, normalized.Value(), 1).HasValue());
        REQUIRE(probe.records.size() == 1);
        REQUIRE(Log::Logger::Flush());
        {
            const Log::LogContext ambient("private.session", "ambient-secret");
            REQUIRE(EmitAcceptedDiagnostic(networkSink, probe.records.front()));
        }
        REQUIRE(Log::Logger::Flush());
        Log::Logger::Shutdown();

        std::ifstream file(fixture.directory / "network.jsonl", std::ios::binary);
        const std::string persisted{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        REQUIRE(persisted.find("network.security.failure") != std::string::npos);
        REQUIRE(persisted.find("connection_generation") != std::string::npos);
        REQUIRE(persisted.find("session_generation") != std::string::npos);
        REQUIRE(persisted.find("scene_runtime") != std::string::npos);
        REQUIRE(persisted.find("player_token") != std::string::npos);
        REQUIRE(persisted.find("Bearer") == std::string::npos);
        REQUIRE(persisted.find("private-password") == std::string::npos);
        REQUIRE(persisted.find("account-name") == std::string::npos);
        REQUIRE(persisted.find("ambient-secret") == std::string::npos);
        std::lock_guard lock(sink->mutex);
        REQUIRE_FALSE(sink->records.empty());
    }
}  // namespace Horo::Network
