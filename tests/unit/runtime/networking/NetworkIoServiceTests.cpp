#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkIoService.h"
#include "Horo/Network/NetworkMetrics.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Network {
    using TestSupport::RequireError;

    namespace {
        ConnectionHandle Connection(const std::uint32_t slot = 1, const std::uint32_t generation = 1) {
            return ConnectionHandle::Create(slot, generation).Value();
        }

        class ScriptedPollSource final : public INetworkIoPollSource {
        public:
            enum class Script : std::uint8_t {
                Success,
                Packet,
                Overproduce,
                RetainProducer,
                Fail
            };

            explicit ScriptedPollSource(const Script selected, PacketBufferPool *pool = nullptr) : script(selected), packetPool(pool) {}

            Result<void> Poll(const NetworkIoCompletionProducer &producer, const std::size_t maximumCompletions,
                              const CancellationToken &) noexcept override {
                ++pollCalls;
                if (script == Script::RetainProducer)
                    retainedProducer = producer;
                if (script == Script::Fail)
                    return Result<void>::Failure(MakeError(NetworkErrors::NameResolutionFailed));
                if (script == Script::RetainProducer)
                    return Result<void>::Success();
                const std::size_t requested = script == Script::Overproduce ? maximumCompletions + 1 : maximumCompletions;
                for (std::size_t index = 0; index < requested; ++index) {
                    Result<NetworkIoCompletion> candidate = MakeCandidate(index);
                    if (candidate.HasError())
                        return Result<void>::Failure(candidate.ErrorValue());
                    auto published = producer.Publish(std::move(candidate).Value());
                    if (published.HasError()) {
                        lastPublishError = published.ErrorValue().code.Value();
                        if (script != Script::Overproduce)
                            return published;
                    }
                }
                return Result<void>::Success();
            }

            void RequestStop() noexcept override {
                stopRequested = true;
            }

            void Shutdown() noexcept override {
                shutdownCalled = true;
            }

            Result<NetworkIoCompletion> MakeCandidate(const std::size_t index) {
                if (script != Script::Packet)
                    return NetworkIoCompletion::MakeOperation(NetworkIoCompletionKind::OperationSucceeded,
                                                              Connection(static_cast<std::uint32_t>(index + 1)));
                const std::array bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
                auto payload = packetPool->Acquire(bytes);
                if (payload.HasError())
                    return Result<NetworkIoCompletion>::Failure(payload.ErrorValue());
                return NetworkIoCompletion::MakePacket(Connection(), ChannelId{}, std::move(payload).Value());
            }

            Script script;
            PacketBufferPool *packetPool{};
            std::optional<NetworkIoCompletionProducer> retainedProducer;
            std::optional<std::string> lastPublishError;
            std::size_t pollCalls{};
            bool stopRequested{};
            bool shutdownCalled{};
        };

        struct Consumed final {
            std::uint64_t sequence;
            NetworkIoCompletionKind kind;
            ConnectionHandle connection;
            std::vector<std::byte> payload;
        };

        class RecordingConsumer final : public INetworkIoCompletionConsumer {
        public:
            void Consume(NetworkIoCompletion completion) noexcept override {
                records.push_back({completion.Sequence(),
                                   completion.Kind(),
                                   completion.Connection(),
                                   {completion.Payload().begin(), completion.Payload().end()}});
            }

            std::vector<Consumed> records;
        };

        struct ServiceFixture final {
            std::unique_ptr<NetworkIoService> service;
            ScriptedPollSource *source;
        };

        ServiceFixture MakeService(const ScriptedPollSource::Script script, const NetworkIoServiceLimits limits = {4, 4, 4},
                                   PacketBufferPool *pool = nullptr, NetworkMetrics *metrics = nullptr) {
            auto source = std::make_unique<ScriptedPollSource>(script, pool);
            auto *sourcePointer = source.get();
            auto created = NetworkIoService::Create(std::move(source), limits, metrics);
            REQUIRE(created.HasValue());
            return {std::move(created).Value(), sourcePointer};
        }
    }  // namespace

    TEST_CASE("Network I/O service publishes immutable FIFO records only through owner drain", "[unit][network][io]") {
        auto fixture = MakeService(ScriptedPollSource::Script::Success);
        REQUIRE(fixture.service->PollBackend(2).HasValue());
        REQUIRE(fixture.service->QueuedCompletions() == 2);

        RecordingConsumer consumer;
        const auto drained = fixture.service->DrainOwnerThread(consumer, 1);
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value() == 1);
        REQUIRE(consumer.records.size() == 1);
        REQUIRE(consumer.records[0].sequence == 1);
        REQUIRE(consumer.records[0].kind == NetworkIoCompletionKind::OperationSucceeded);
        REQUIRE(consumer.records[0].connection == Connection());
        REQUIRE(consumer.records[0].payload.empty());
        REQUIRE(fixture.service->DrainOwnerThread(consumer, 2).Value() == 1);
        REQUIRE(consumer.records[1].sequence == 2);
        REQUIRE(consumer.records[1].connection == Connection(2));
        REQUIRE(fixture.service->QueuedCompletions() == 0);
    }

    TEST_CASE("Network I/O packet handoff owns prepared bytes across producer return", "[unit][network][io]") {
        auto preparedPool = PacketBufferPool::Create({1, 8, 8});
        REQUIRE(preparedPool.HasValue());
        auto pool = std::move(preparedPool).Value();
        auto fixture = MakeService(ScriptedPollSource::Script::Packet, {1, 1, 1}, &pool);
        REQUIRE(fixture.service->PollBackend(1).HasValue());
        REQUIRE(pool.Outstanding() == 1);

        RecordingConsumer consumer;
        REQUIRE(fixture.service->DrainOwnerThread(consumer, 1).Value() == 1);
        REQUIRE(consumer.records[0].payload == std::vector{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}});
        REQUIRE(pool.Outstanding() == 0);
    }

    TEST_CASE("Network I/O poll and queue budgets reject excess publication deterministically", "[unit][network][io]") {
        auto fixture = MakeService(ScriptedPollSource::Script::Overproduce, {1, 1, 1});
        REQUIRE(fixture.service->PollBackend(1).HasValue());
        REQUIRE(fixture.source->lastPublishError == NetworkErrors::NetworkIoCompletionQueueFull.code.Value());
        REQUIRE(fixture.service->QueuedCompletions() == 1);
        RequireError(fixture.service->PollBackend(0), NetworkErrors::NetworkIoServiceInvalid);
        RequireError(fixture.service->PollBackend(2), NetworkErrors::NetworkIoServiceInvalid);

        RecordingConsumer consumer;
        RequireError(fixture.service->DrainOwnerThread(consumer, 0), NetworkErrors::NetworkIoServiceInvalid);
        RequireError(fixture.service->DrainOwnerThread(consumer, 2), NetworkErrors::NetworkIoServiceInvalid);
    }

    TEST_CASE("Network I/O metrics publish owner-drained queue and capacity drops", "[unit][network][io][metrics]") {
        NetworkMetrics metrics{77, true};
        auto fixture = MakeService(ScriptedPollSource::Script::Overproduce, {1, 1, 1}, nullptr, &metrics);
        REQUIRE(fixture.service->PollBackend(1).HasValue());
        RecordingConsumer consumer;
        REQUIRE(fixture.service->DrainOwnerThread(consumer, 1).Value() == 1);
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.queueDepth[static_cast<std::size_t>(NetworkMetricQueue::Completion)] == 0);
        REQUIRE(snapshot.drops[static_cast<std::size_t>(NetworkMetricDrop::Capacity)] == 1);
        fixture.service->Shutdown();
        REQUIRE(metrics.Close());
    }

    TEST_CASE("Inbound packet depth excludes non-packet completions at the owner drain", "[unit][network][io][metrics]") {
        auto prepared = PacketBufferPool::Create({2, 8, 8});
        REQUIRE(prepared.HasValue());
        auto pool = std::move(prepared).Value();
        NetworkMetrics metrics{80, true};
        auto fixture = MakeService(ScriptedPollSource::Script::Packet, {2, 2, 1}, &pool, &metrics);
        REQUIRE(fixture.service->PollBackend(2).HasValue());
        RecordingConsumer consumer;
        REQUIRE(fixture.service->DrainOwnerThread(consumer, 1).Value() == 1);
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.queueDepth[static_cast<std::size_t>(NetworkMetricQueue::Inbound)] == 1);
        REQUIRE(snapshot.queueDepth[static_cast<std::size_t>(NetworkMetricQueue::Completion)] == 1);
        fixture.service->Shutdown();
        REQUIRE(metrics.Close());
    }

    TEST_CASE("Disabled I/O observer and late producer never retain metric object", "[unit][network][io][metrics]") {
        NetworkMetrics disabled{78, false};
        auto unobserved = MakeService(ScriptedPollSource::Script::Overproduce, {1, 1, 1}, nullptr, &disabled);
        REQUIRE(unobserved.service->PollBackend(1).HasValue());
        RecordingConsumer consumer;
        REQUIRE(unobserved.service->DrainOwnerThread(consumer, 1).Value() == 1);
        REQUIRE(disabled.Publish());
        REQUIRE(disabled.Snapshot().drops[static_cast<std::size_t>(NetworkMetricDrop::Capacity)] == 0);
        unobserved.service->Shutdown();
        REQUIRE(disabled.Close());

        std::optional<NetworkIoCompletionProducer> late;
        {
            NetworkMetrics enabled{79, true};
            auto fixture = MakeService(ScriptedPollSource::Script::RetainProducer, {1, 1, 1}, nullptr, &enabled);
            REQUIRE(fixture.service->PollBackend(1).HasValue());
            late = *fixture.source->retainedProducer;
            REQUIRE(enabled.Close());
            fixture.service->Shutdown();
            fixture.service.reset();
        }
        auto candidate = NetworkIoCompletion::MakeOperation(NetworkIoCompletionKind::OperationSucceeded, Connection());
        RequireError(late->Publish(std::move(candidate).Value()), NetworkErrors::TransportShuttingDown);
    }

    TEST_CASE("Network I/O completion construction rejects malformed kind identity payload and provenance", "[unit][network][io]") {
        RequireError(NetworkIoCompletion::MakeOperation(NetworkIoCompletionKind::PacketReceived, Connection()),
                     NetworkErrors::NetworkIoCompletionInvalid);
        RequireError(NetworkIoCompletion::MakeOperation(NetworkIoCompletionKind::OperationSucceeded, {}),
                     NetworkErrors::NetworkIoCompletionInvalid);
        RequireError(NetworkIoCompletion::MakePacket(Connection(), ChannelId{}, {}), NetworkErrors::NetworkIoCompletionInvalid);

        const std::array context{NetworkFailureContextEntry{NetworkFailureContextKey::Connection, Connection(4).Diagnostic()}};
        const auto failure = MakeNetworkTerminalRecord(NetworkFailureLayer::Transport, NetworkFailureKind::TransportUnavailable, context);
        REQUIRE(failure.HasValue());
        RequireError(NetworkIoCompletion::MakeFailure(Connection(5), failure.Value()), NetworkErrors::NetworkIoCompletionInvalid);
        auto validFailure = NetworkIoCompletion::MakeFailure(Connection(4), failure.Value());
        REQUIRE(validFailure.HasValue());
        REQUIRE(validFailure.Value().Failure() != nullptr);
        REQUIRE(validFailure.Value().Failure()->Kind() == NetworkFailureKind::TransportUnavailable);
    }

    TEST_CASE("Network I/O service preserves backend failure and rejects cancelled polls before backend work", "[unit][network][io]") {
        auto failure = MakeService(ScriptedPollSource::Script::Fail);
        RequireError(failure.service->PollBackend(1), NetworkErrors::NameResolutionFailed);
        REQUIRE(failure.service->QueuedCompletions() == 0);

        auto cancelled = MakeService(ScriptedPollSource::Script::Success);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(cancelled.service->PollBackend(1, cancellation.Token()), NetworkErrors::TransportOperationCancelled);
        REQUIRE(cancelled.source->pollCalls == 0);
    }

    TEST_CASE("Network I/O owner affinity prevents background callbacks into consumer state", "[unit][network][io]") {
        auto fixture = MakeService(ScriptedPollSource::Script::Success);
        REQUIRE(fixture.service->PollBackend(1).HasValue());
        RecordingConsumer consumer;
        std::optional<Error> observedError;
        std::thread foreign{[&] {
            auto drained = fixture.service->DrainOwnerThread(consumer, 1);
            if (drained.HasError())
                observedError = drained.ErrorValue();
        }};
        foreign.join();
        REQUIRE(observedError.has_value());
        REQUIRE(observedError->code.Value() == NetworkErrors::NetworkIoWrongThread.code.Value());
        REQUIRE(consumer.records.empty());
        REQUIRE(fixture.service->DrainOwnerThread(consumer, 1).Value() == 1);
    }

    TEST_CASE("Network I/O stale producers and shutdown cannot republish or observe destroyed consumers", "[unit][network][io]") {
        auto fixture = MakeService(ScriptedPollSource::Script::RetainProducer);
        REQUIRE(fixture.service->PollBackend(1).HasValue());
        REQUIRE(fixture.source->retainedProducer.has_value());
        auto late = NetworkIoCompletion::MakeOperation(NetworkIoCompletionKind::OperationSucceeded, Connection());
        RequireError(fixture.source->retainedProducer->Publish(std::move(late).Value()), NetworkErrors::NetworkIoPollStale);

        fixture.service->Shutdown();
        fixture.service->Shutdown();
        REQUIRE(fixture.source->stopRequested);
        REQUIRE(fixture.source->shutdownCalled);
        REQUIRE(fixture.service->IsShuttingDown());
        RequireError(fixture.service->PollBackend(1), NetworkErrors::TransportShuttingDown);
        RecordingConsumer consumer;
        RequireError(fixture.service->DrainOwnerThread(consumer, 1), NetworkErrors::TransportShuttingDown);
        auto afterShutdown = NetworkIoCompletion::MakeOperation(NetworkIoCompletionKind::OperationSucceeded, Connection());
        RequireError(fixture.source->retainedProducer->Publish(std::move(afterShutdown).Value()), NetworkErrors::TransportShuttingDown);
        REQUIRE(consumer.records.empty());
    }

    TEST_CASE("Network I/O service validates construction hard bounds", "[unit][network][io]") {
        RequireError(NetworkIoService::Create({}, {1, 1, 1}), NetworkErrors::NetworkIoServiceInvalid);
        auto source = std::make_unique<ScriptedPollSource>(ScriptedPollSource::Script::Success);
        RequireError(NetworkIoService::Create(std::move(source), {}), NetworkErrors::NetworkIoServiceInvalid);
        auto oversized = std::make_unique<ScriptedPollSource>(ScriptedPollSource::Script::Success);
        RequireError(NetworkIoService::Create(std::move(oversized), {MaximumNetworkIoQueuedCompletions + 1, 1, 1}),
                     NetworkErrors::NetworkIoServiceInvalid);
    }
}  // namespace Horo::Network
