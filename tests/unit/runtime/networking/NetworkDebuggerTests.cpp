#include "AllocationProbe.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkTickAlignment.h"
#include "Horo/Network/PeerSessionLifecycle.h"
#include "NetworkDebuggerLoopback.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>
using namespace Horo;
using namespace Horo::Network;

namespace {
    constexpr NetworkDiagnosticSource Source{1, 2, 3, 4};

    ConnectionHandle Handle() {
        return ConnectionHandle::Create(0, 1).Value();
    }
}  // namespace

TEST_CASE("Network diagnostics retain bounded evidence and immutable revisions without allocation", "[network][debugger]") {
    NetworkDebugger debugger;
    REQUIRE(debugger.Begin(Source, true));
    const auto first = debugger.Snapshot();
    const auto connection = Handle();
    bool accepted = true;
    Horo::Tests::AllocationProbe::Measurement allocation;
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measured;
        for (int i = 0; i < 10000; ++i)
            accepted =
                debugger.Observe(Source, NetworkConnectionRecord{connection, NetworkTransportEventKind::PacketReceived, 7}) && accepted;
        accepted = debugger.Publish(Source, 100) && accepted;
        allocation = measured.Snapshot();
    }
    REQUIRE(accepted);
    REQUIRE(allocation.requests == 0);
    const auto snapshot = debugger.Snapshot();
    REQUIRE(snapshot.connections.size == 32);
    REQUIRE(snapshot.connections.dropped == 9968);
    REQUIRE(snapshot.capture.size == 256);
    REQUIRE(snapshot.capture.dropped == 9744);
    REQUIRE(snapshot.capture.records[255].sequence == 256);
    REQUIRE(first.capture.size == 0);
    REQUIRE(snapshot.revision > first.revision);
    REQUIRE(snapshot.source == Source);
}

TEST_CASE("Network diagnostics fence source replacement and bounded capture commands", "[network][debugger][lifecycle]") {
    NetworkDebugger debugger;
    REQUIRE(debugger.Begin(Source, true));
    REQUIRE(debugger.Publish(Source, 100));
    auto snapshot = debugger.Snapshot();
    REQUIRE_FALSE(debugger.Request({1, 2, 3, 5}, snapshot.revision, NetworkCaptureAction::Clear));
    REQUIRE_FALSE(debugger.Request(Source, snapshot.revision - 1, NetworkCaptureAction::Clear));
    REQUIRE(debugger.Request(Source, snapshot.revision, NetworkCaptureAction::Pause));
    REQUIRE_FALSE(debugger.Snapshot().capturePaused);
    REQUIRE(debugger.Publish(Source, 101));
    REQUIRE(debugger.Snapshot().capturePaused);
    REQUIRE(debugger.Observe(Source, NetworkConnectionRecord{Handle(), NetworkTransportEventKind::Connected}));
    REQUIRE(debugger.Publish(Source, 102));
    REQUIRE(debugger.Snapshot().capture.size == 0);
    snapshot = debugger.Snapshot();
    for (int i = 0; i < 8; ++i)
        REQUIRE(debugger.Request(Source, snapshot.revision, NetworkCaptureAction::Resume));
    REQUIRE_FALSE(debugger.Request(Source, snapshot.revision, NetworkCaptureAction::Clear));
    REQUIRE(debugger.Publish(Source, 103));
    REQUIRE(debugger.Observe(Source, NetworkConnectionRecord{Handle(), NetworkTransportEventKind::Connected}));
    REQUIRE(debugger.Publish(Source, 104));
    snapshot = debugger.Snapshot();
    REQUIRE(snapshot.capture.size == 1);
    REQUIRE(debugger.Request(Source, snapshot.revision, NetworkCaptureAction::Clear));
    debugger.Detach();
    REQUIRE_FALSE(debugger.Observe(Source, NetworkConnectionRecord{Handle(), NetworkTransportEventKind::Connected}));
    REQUIRE_FALSE(debugger.Request(Source, snapshot.revision, NetworkCaptureAction::Resume));
    REQUIRE(debugger.Begin({1, 3, 3, 4}, true));
    REQUIRE_FALSE(debugger.Publish(Source, 105));
    REQUIRE_FALSE(debugger.Begin(Source, true));
    REQUIRE_FALSE(debugger.Observe(Source, NetworkConnectionRecord{Handle(), NetworkTransportEventKind::Connected}));
    REQUIRE(debugger.Publish({1, 3, 3, 4}, 106));
    REQUIRE(debugger.Snapshot().capture.size == 0);
    REQUIRE(debugger.Snapshot().revision > snapshot.revision);
}

TEST_CASE("Disabled detached stale and wrong-thread network evidence cannot become live", "[network][debugger][lifecycle]") {
    using Application::NetworkDebuggerService;
    using Application::NetworkDebuggerState;
    NetworkDebugger debugger;
    REQUIRE_FALSE(debugger.Begin({}, true));
    REQUIRE(debugger.Begin(Source, false));
    REQUIRE_FALSE(debugger.Observe(Source, NetworkConnectionRecord{Handle(), NetworkTransportEventKind::Connected}));
    REQUIRE(debugger.Publish(Source, 100));
    auto disabled = debugger.Snapshot();
    REQUIRE(NetworkDebuggerService::Assess(disabled, 100, 5) == NetworkDebuggerState::Disabled);
    REQUIRE_FALSE(debugger.Request(Source, disabled.revision, NetworkCaptureAction::Pause));
    debugger.Detach();
    REQUIRE(NetworkDebuggerService::Assess(debugger.Snapshot(), 100, 5) == NetworkDebuggerState::Detached);
    REQUIRE(debugger.Begin({1, 3, 3, 5}, true));
    const auto source = debugger.Source();
    REQUIRE(NetworkDebuggerService::Assess(debugger.Snapshot(), 100, 5) == NetworkDebuggerState::Stale);
    REQUIRE(debugger.Publish(source, 100));
    REQUIRE(NetworkDebuggerService::Assess(debugger.Snapshot(), 105, 5) == NetworkDebuggerState::Live);
    REQUIRE(NetworkDebuggerService::Assess(debugger.Snapshot(), 106, 5) == NetworkDebuggerState::Stale);
    REQUIRE(NetworkDebuggerService::Assess(debugger.Snapshot(), 99, 5) == NetworkDebuggerState::Stale);
    bool recorded = true, published = true;
    std::thread other{[&] {
        recorded = debugger.Observe(source, NetworkConnectionRecord{Handle(), NetworkTransportEventKind::Connected});
        published = debugger.Publish(source, 102);
    }};
    other.join();
    REQUIRE_FALSE(recorded);
    REQUIRE_FALSE(published);
    REQUIRE(debugger.Snapshot().publishedNanoseconds == 100);
    REQUIRE_FALSE(debugger.Publish(source, 99));
    NetworkMetricSnapshot foreign;
    foreign.ownerGeneration = 99;
    REQUIRE_FALSE(debugger.Publish(source, 101, &foreign));
}

TEST_CASE("Real session and tick-alignment producers publish generation-fenced diagnostics", "[network][debugger][producer]") {
    NetworkDebugger debugger;
    REQUIRE(debugger.Begin(Source, true));
    auto session =
        PeerSessionLifecycle::Create(Handle(), NetworkOperationGeneration::Create(7).Value(), {10, 20, 30, 100, 20}, nullptr, &debugger);
    REQUIRE(session.HasValue());
    REQUIRE(session.Value().BeginNegotiation(Handle(), NetworkOperationGeneration::Create(7).Value(), 1).HasValue());
    REQUIRE(debugger.Publish(Source, 100));
    REQUIRE(debugger.Snapshot().connections.records[0].peerSessionGeneration == 7);
    REQUIRE_FALSE(debugger.Snapshot().connections.records[0].gameplayAdmitted);
    auto mapper = NetworkTickAlignment::Create(Handle(), NetworkOperationGeneration::Create(7).Value(), {8, 2, 3}, &debugger);
    REQUIRE(mapper.HasValue());
    REQUIRE(mapper.Value().Observe({Handle(), NetworkOperationGeneration::Create(7).Value(), 1, 1, 0, 100, 2}).HasValue());
    REQUIRE(mapper.Value().Advance(1).HasValue());
    REQUIRE(debugger.Publish(Source, 101));
    REQUIRE(debugger.Snapshot().prediction.size == 1);
    REQUIRE(debugger.Snapshot().prediction.records[0].localTick == 1);
    REQUIRE(debugger.Snapshot().prediction.records[0].hasMapping);
    debugger.Detach();
    REQUIRE(debugger.Begin({1, 3, 3, 5}, true));
    REQUIRE(mapper.Value().Advance(2).HasValue());
    REQUIRE(debugger.Publish(debugger.Source(), 102));
    REQUIRE(debugger.Snapshot().prediction.size == 0);
}

TEST_CASE("Real loopback measurements reach the application projection without synthetic preview values", "[network][debugger][producer]") {
    Application::NetworkDebuggerService service;
    REQUIRE(service.Begin(9, 4, true, NetworkDiagnosticProvider::Deterministic));
    const auto source = service.Producer().Source();
    NetworkMetrics metrics{source.session, true, &service.Producer()};
    auto created = DeterministicTransport::Create(TestSupport::DebuggerLoopbackDescriptor(), &metrics, &service.Producer());
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport.Open(Handle()).HasValue());
    std::array<DeterministicTransportEvent, 2> events;
    REQUIRE(transport.Advance(1, events).HasValue());
    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(transport.Send(Handle(), ChannelId::Create(0, 2).Value(), TransportTrafficClass::Reliable, 0, payload).HasValue());
    REQUIRE(transport.Advance(2, events).Value() == 1);
    REQUIRE(metrics.Publish());
    const auto metricSnapshot = metrics.Snapshot();
    REQUIRE(service.Publish(source, &metricSnapshot));
    auto live = service.Query();
    REQUIRE(live.state == Application::NetworkDebuggerState::Live);
    REQUIRE(live.snapshot.metrics.bytes[0][0] == 3);
    REQUIRE(live.snapshot.metrics.bytes[1][0] == 3);
    REQUIRE(live.snapshot.connections.size == 2);
    REQUIRE(live.snapshot.connections.records[1].event == NetworkTransportEventKind::PacketReceived);
    REQUIRE(live.snapshot.connections.records[1].bytes == 3);
    REQUIRE(live.snapshot.provider == NetworkDiagnosticProvider::Deterministic);
    REQUIRE(service.Request(source, live.snapshot.revision, NetworkCaptureAction::Clear));
    REQUIRE(service.Publish(source));
    REQUIRE(service.Query().snapshot.capture.size == 0);
    service.Producer().Detach();
    REQUIRE(service.Begin(9, 5, true, NetworkDiagnosticProvider::Deterministic));
    REQUIRE(transport.Send(Handle(), ChannelId::Create(0, 2).Value(), TransportTrafficClass::Reliable, 0, payload).HasValue());
    REQUIRE(transport.Advance(3, events).Value() == 1);
    REQUIRE(service.Publish(service.Producer().Source()));
    REQUIRE(service.Query().snapshot.connections.size == 0);
    REQUIRE_FALSE(service.Request(source, live.snapshot.revision, NetworkCaptureAction::Pause));
    service.Producer().Detach();
    REQUIRE(service.Query().state == Application::NetworkDebuggerState::Detached);
    (void)transport.Shutdown();
    REQUIRE(metrics.Close());
}
