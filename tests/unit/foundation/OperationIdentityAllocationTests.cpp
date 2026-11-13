#include "../../support/AllocationProbe.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Telemetry/Operation.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <thread>
#include <utility>

TEST_CASE("Operation identity capture does not allocate on a fresh thread", "[foundation][jobs][telemetry][identity-allocation]") {
    std::size_t requests{};
    bool emptyLineage{};
    bool emptyFields{};
    std::thread worker([&requests, &emptyLineage, &emptyFields] {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        const auto identity = Horo::Telemetry::CaptureOperationIdentity();
        emptyLineage = identity.operationId == 0 && identity.parentOperationId == 0;
        emptyFields = identity.diagnosticContext.Fields().empty();
        requests = measurement.Snapshot().requests;
    });
    worker.join();
    CHECK(emptyLineage);
    CHECK(emptyFields);
    CHECK(requests == 0);
}

TEST_CASE("Empty log snapshot construction copy and move do not allocate", "[foundation][jobs][telemetry][identity-allocation]") {
    std::size_t requests{};
    bool empty{};
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        const Horo::Log::LogContextSnapshot source;
        auto copy = source;
        const auto moved = std::move(copy);
        empty = source.Fields().empty() && moved.Fields().empty() && !source.IsIsolationBoundary();
        requests = measurement.Snapshot().requests;
    }
    CHECK(empty);
    CHECK(requests == 0);
}

TEST_CASE("Retained log snapshots preserve fields and isolation across derivation", "[foundation][jobs][telemetry][snapshot-ownership]") {
    Horo::Log::LogContextSnapshot retained;
    {
        const auto source = Horo::Log::LogContextSnapshot::Isolated({{"operation", "first"}});
        retained = source;
    }
    const auto derived = retained.With("operation", "second");
    REQUIRE(retained.Fields().size() == 1);
    REQUIRE(derived.Fields().size() == 1);
    CHECK(retained.Fields()[0] == Horo::Log::MdcField{"operation", "first"});
    CHECK(derived.Fields()[0] == Horo::Log::MdcField{"operation", "second"});
    CHECK(retained.IsIsolationBoundary());
    CHECK(derived.IsIsolationBoundary());
}
