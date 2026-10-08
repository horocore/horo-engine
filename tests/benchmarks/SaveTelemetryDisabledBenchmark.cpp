#include "../support/BenchmarkAllocationProbe.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"

#include <chrono>
#include <iostream>
#include <memory>

using Horo::Tests::BenchmarkAllocationProbe::AllocationState;

namespace {
    struct Measurement {
        std::size_t completed{};
        std::size_t allocations{};
        std::int64_t nanoseconds{};
    };

    /** @brief Numeric lineage failures and calling-thread allocations observed during identity capture. */
    struct IdentityMeasurement {
        std::size_t failures{};
        std::size_t allocations{};
    };

    /** @brief Measures one explicit allocation before producer work; reports no completion on allocation failure. */
    Measurement CalibrateAllocationProbe() {
        Measurement result;
        AllocationState().trackedAllocations = 0;
        AllocationState().trackAllocations = true;
        const auto release = [](void *memory) noexcept {
            ::operator delete(memory);
        };
        try {
            // An explicit allocation-function call cannot be elided like a new-expression.
            const std::unique_ptr<void, decltype(release)> allocation{::operator new(64), release};
            AllocationState().trackAllocations = false;
            result.completed = 1;
        } catch (const std::bad_alloc &) {
            AllocationState().trackAllocations = false;
            // Failed calibration cannot establish that the probe observes successful allocations.
        }
        result.allocations = AllocationState().trackedAllocations;
        return result;
    }

    /** @brief Measures numeric identity capture without including setup or positive-control allocations. */
    IdentityMeasurement MeasureIdentities(const std::size_t iterations) {
        IdentityMeasurement result;
        AllocationState().trackedAllocations = 0;
        AllocationState().trackAllocations = true;
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            const auto identity = Horo::Telemetry::CaptureOperationIdentity();
            result.failures +=
                identity.operationId != 41 || identity.parentOperationId != 40 || !identity.diagnosticContext.Fields().empty() ? 1U : 0U;
        }
        AllocationState().trackAllocations = false;
        result.allocations = AllocationState().trackedAllocations;
        return result;
    }

    class PolicySink final : public Horo::Telemetry::ISink {
        void Export(const Horo::Telemetry::Record &, const Horo::Telemetry::InstrumentDescriptor *) override {
            // This fixture deliberately discards records; it measures producer behavior only.
        }

        void Flush() override {
            // This fixture retains no records to flush.
        }
    };

    Measurement MeasureStages(const std::size_t iterations) {
        Measurement result;
        AllocationState().trackedAllocations = 0;
        AllocationState().trackAllocations = true;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            const auto work = Horo::Runtime::ObserveSaveStage(Horo::Runtime::SaveTelemetryStage::Capture, 1, [] {
                return Horo::Result<void>::Success();
            });
            result.completed += work.HasValue() ? 1U : 0U;
        }
        result.nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        AllocationState().trackAllocations = false;
        result.allocations = AllocationState().trackedAllocations;
        return result;
    }
}  // namespace

int main() {
    const auto calibration = CalibrateAllocationProbe();
    if (calibration.completed != 1 || calibration.allocations != 1)
        return 5;
    Horo::Log::Logger::Shutdown();
    auto registrationResult = Horo::Runtime::SaveTelemetryRegistration::Create();
    if (registrationResult.HasError())
        return 1;
    auto registration = std::move(registrationResult).Value();
    constexpr std::size_t iterations = 100'000;
    Horo::Telemetry::ScopedOperationContext operation{Horo::Telemetry::OperationContext{.operationId = 41, .parentOperationId = 40}};
    Horo::Log::LogContext ambient{"account.id", std::string(4096, 'a'), "payload.özel", std::string(4096, 'b')};
    const auto identities = MeasureIdentities(iterations);
    const auto disabled = MeasureStages(iterations);
    registration.reset();
    if (!Horo::Telemetry::Runtime::Initialize({.subsystemPrefixes = {"other"},
                                               .metricCollectionLevel = Horo::Telemetry::MetricCollectionLevel::Off},
                                              std::make_shared<PolicySink>()))
        return 3;
    registrationResult = Horo::Runtime::SaveTelemetryRegistration::Create();
    if (registrationResult.HasError())
        return 4;
    registration = std::move(registrationResult).Value();
    const auto policy = MeasureStages(iterations);
    registration.reset();
    const bool shutdown = Horo::Telemetry::Runtime::Shutdown();
    std::cout << "compiled_telemetry=" << HORO_ENABLE_TELEMETRY << " iterations=" << iterations
              << " identity_allocations=" << identities.allocations << " identity_failures=" << identities.failures
              << " disabled_save_allocations=" << disabled.allocations << " disabled_ns_per_stage=" << disabled.nanoseconds / iterations
              << " policy_disabled_allocations=" << policy.allocations
              << " policy_disabled_ns_per_stage=" << policy.nanoseconds / iterations << '\n';
    return shutdown && disabled.completed == iterations && disabled.allocations == 0 && policy.completed == iterations &&
                   policy.allocations == 0 && identities.allocations == 0 && identities.failures == 0
               ? 0
               : 2;
}
