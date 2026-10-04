#include "../support/BenchmarkAllocationProbe.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <thread>
#include <vector>

using Horo::Tests::BenchmarkAllocationProbe::AllocationState;

namespace {
    class NullSink final : public Horo::Telemetry::ISink {
    public:
        void Export(const Horo::Telemetry::Record &, const Horo::Telemetry::InstrumentDescriptor *) override {
            // This fixture deliberately discards records; it measures producer behavior only.
        }

        void Flush() override {
            // This fixture retains no records to flush.
        }
    };
}  // namespace

int main() {
    constexpr std::size_t kIterations = 100'000;
    constexpr std::size_t kProducerCount = 4;
    static_cast<void>(Horo::Telemetry::Runtime::Shutdown());
    auto sink = std::make_shared<NullSink>();
    if (!Horo::Telemetry::Runtime::Initialize({.queueCapacity = 4096, .enabled = true}, sink))
        return 1;

    const Horo::Telemetry::Counter instrument = Horo::Telemetry::Runtime::RegisterCounter({
        .name = "benchmark.records",
        .subsystem = "Foundation.Telemetry",
        .unit = Horo::Telemetry::MetricUnit::Count,
        .dimensions = {{.key = "backend", .allowedValues = {"null"}}},
        .maxSeries = 1,
    });
    const Horo::Telemetry::Counter counter =
        instrument.WithDimensions(std::array{Horo::Telemetry::DimensionValue{.key = "backend", .value = "null"}});
    const Horo::Telemetry::Statistics metricBefore = Horo::Telemetry::Runtime::GetStatistics();

    AllocationState().trackedAllocations = 0;
    AllocationState().trackAllocations = true;
    const auto startedAt = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration)
        counter.Add();
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    AllocationState().trackAllocations = false;

    const Horo::Telemetry::Statistics metricAfter = Horo::Telemetry::Runtime::GetStatistics();
    const std::size_t metricAllocations = AllocationState().trackedAllocations;
    const auto metricNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    Horo::Log::Logger::SetLevel(Horo::Log::Level::Info);
    const Horo::Telemetry::Statistics loggingBefore = Horo::Telemetry::Runtime::GetStatistics();
    AllocationState().trackedAllocations = 0;
    AllocationState().trackAllocations = true;
    const auto loggingStartedAt = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration)
        Horo::Log::Logger::Write("bench.log", Horo::Log::Level::Info, "record");
    const auto loggingElapsed = std::chrono::steady_clock::now() - loggingStartedAt;
    AllocationState().trackAllocations = false;
    const Horo::Telemetry::Statistics loggingAfter = Horo::Telemetry::Runtime::GetStatistics();
    const std::size_t loggingAllocations = AllocationState().trackedAllocations;

    const Horo::Telemetry::Statistics concurrentBefore = Horo::Telemetry::Runtime::GetStatistics();
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    const auto concurrentStartedAt = std::chrono::steady_clock::now();
    for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([counter] {
            for (std::size_t iteration = 0; iteration < kIterations / kProducerCount; ++iteration)
                counter.Add();
        });
    }
    for (std::thread &producer : producers)
        producer.join();
    const auto concurrentElapsed = std::chrono::steady_clock::now() - concurrentStartedAt;
    const Horo::Telemetry::Statistics concurrentAfter = Horo::Telemetry::Runtime::GetStatistics();

    Horo::Log::Logger::SetLevel(Horo::Log::Level::Off);
    AllocationState().trackedAllocations = 0;
    AllocationState().trackAllocations = true;
    const auto disabledLoggingStartedAt = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration)
        HORO_LOG_DEBUG("bench.log", "disabled");
    const auto disabledLoggingElapsed = std::chrono::steady_clock::now() - disabledLoggingStartedAt;
    AllocationState().trackAllocations = false;
    const std::size_t disabledLoggingAllocations = AllocationState().trackedAllocations;

    const bool flushed = Horo::Telemetry::Runtime::Flush();
    const bool shutdown = Horo::Telemetry::Runtime::Shutdown();
    const double metricNanosecondsPerCall = static_cast<double>(metricNanoseconds) / static_cast<double>(kIterations);
    const double loggingNanosecondsPerCall =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(loggingElapsed).count()) /
        static_cast<double>(kIterations);
    const double concurrentNanosecondsPerCall =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(concurrentElapsed).count()) /
        static_cast<double>(kIterations);
    const double disabledLoggingNanosecondsPerCall =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(disabledLoggingElapsed).count()) /
        static_cast<double>(kIterations);

    std::cout << "telemetry_enabled=" << HORO_ENABLE_TELEMETRY << " iterations=" << kIterations
              << " metric_allocations=" << metricAllocations << " metric_ns_per_call=" << metricNanosecondsPerCall
              << " metric_accepted=" << (metricAfter.acceptedRecords - metricBefore.acceptedRecords)
              << " metric_dropped=" << (metricAfter.droppedRecords - metricBefore.droppedRecords)
              << " logging_allocations=" << loggingAllocations << " logging_ns_per_call=" << loggingNanosecondsPerCall
              << " logging_accepted=" << (loggingAfter.acceptedRecords - loggingBefore.acceptedRecords)
              << " logging_dropped=" << (loggingAfter.droppedRecords - loggingBefore.droppedRecords)
              << " concurrent_producers=" << kProducerCount << " concurrent_ns_per_call=" << concurrentNanosecondsPerCall
              << " concurrent_accepted=" << (concurrentAfter.acceptedRecords - concurrentBefore.acceptedRecords)
              << " concurrent_dropped=" << (concurrentAfter.droppedRecords - concurrentBefore.droppedRecords)
              << " disabled_logging_allocations=" << disabledLoggingAllocations
              << " disabled_logging_ns_per_call=" << disabledLoggingNanosecondsPerCall << '\n';

    // Allocation counts from an unoptimized runtime are implementation noise, in
    // particular with MSVC's Debug standard library. Keep reporting them in every
    // build, but enforce the performance contract only in optimized builds.
#if defined(NDEBUG)
    if (metricAllocations != 0 || loggingAllocations != 0 || disabledLoggingAllocations != 0)
        return 2;
#endif
#if HORO_ENABLE_TELEMETRY
    if (!static_cast<bool>(counter) || !flushed || !shutdown)
        return 3;
    if ((metricAfter.acceptedRecords - metricBefore.acceptedRecords) + (metricAfter.droppedRecords - metricBefore.droppedRecords) !=
        kIterations)
        return 4;
    if ((loggingAfter.acceptedRecords - loggingBefore.acceptedRecords) + (loggingAfter.droppedRecords - loggingBefore.droppedRecords) !=
        kIterations)
        return 7;
    if ((concurrentAfter.acceptedRecords - concurrentBefore.acceptedRecords) +
            (concurrentAfter.droppedRecords - concurrentBefore.droppedRecords) !=
        kIterations)
        return 8;
#else
    if (static_cast<bool>(counter) || !flushed || !shutdown)
        return 5;
    if (metricAfter.acceptedRecords != metricBefore.acceptedRecords || metricAfter.droppedRecords != metricBefore.droppedRecords)
        return 6;
#endif
    return 0;
}
