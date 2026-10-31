#pragma once
/** @file PrefabMetricsTestSupport.h @brief Real Telemetry collector shared by focused prefab and cook-host behavior tests. */
#include "Horo/Prefab/PrefabMetrics.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <thread>

namespace Horo::Prefab::MetricTest {
    /** @brief Test sink observes canonical descriptors and records on the real dispatcher thread. */
    class Sink final : public Telemetry::ISink {
    public:
        std::atomic<bool> blocked{};
        std::atomic<bool> entered{};
        std::mutex mutex;
        std::vector<Telemetry::Record> records;
        std::vector<Telemetry::InstrumentDescriptor> descriptors;

        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
            entered = true;
            while (blocked.load())
                std::this_thread::yield();
            std::lock_guard lock{mutex};
            records.push_back(record);
            descriptors.push_back(descriptor ? *descriptor : Telemetry::InstrumentDescriptor{});
        }

        void Flush() override {}

        double Attempts(const PrefabMetricPhase phase, const PrefabMetricOutcome outcome) {
            std::lock_guard lock{mutex};
            double total{};
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto *metric = std::get_if<Telemetry::MetricRecord>(&records[index].payload);
                if (metric && descriptors[index].name == "horo.prefab.operation" &&
                    metric->dimensionValueIds[0] == static_cast<unsigned>(phase) + 1 &&
                    metric->dimensionValueIds[1] == static_cast<unsigned>(outcome) + 1)
                    total += metric->value;
            }
            return total;
        }

        double Cache(const PrefabMetricCacheEvent event) {
            std::lock_guard lock{mutex};
            double total{};
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto *metric = std::get_if<Telemetry::MetricRecord>(&records[index].payload);
                if (metric && descriptors[index].name == "horo.prefab.cache" &&
                    metric->dimensionValueIds[0] == static_cast<unsigned>(event) + 1)
                    total += metric->value;
            }
            return total;
        }

        std::size_t Spans() {
            std::lock_guard lock{mutex};
            std::size_t count{};
            for (const auto &record : records)
                if (std::holds_alternative<Telemetry::SpanRecord>(record.payload))
                    ++count;
            return count;
        }
    };

    /** @brief Process-local real runtime owner; always releases a deliberately blocked sink before teardown. */
    struct Recorder final {
        std::shared_ptr<Sink> sink{std::make_shared<Sink>()};
        PrefabMetricHandles handles;

        explicit Recorder(Telemetry::MetricCollectionLevel level = Telemetry::MetricCollectionLevel::Detailed,
                          std::size_t capacity = 2048) {
            REQUIRE(Telemetry::Runtime::Shutdown());
            REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = capacity, .metricCollectionLevel = level}, sink));
            handles = RegisterPrefabMetricHandles(level, true);
            REQUIRE(handles.attempts[0][0]);
            ParkDispatcher();
        }

        ~Recorder() {
            sink->blocked = false;
            static_cast<void>(Telemetry::Runtime::Shutdown());
        }

        void Flush() {
            Resume();
            REQUIRE(Telemetry::Runtime::Flush());
            ParkDispatcher();
        }

        void Resume() {
            sink->blocked = false;
        }

    private:
        /** @brief Parks the real dispatcher outside its queue lock so exact-count tests cannot lose observations to contention. */
        void ParkDispatcher() {
            sink->entered = false;
            sink->blocked = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            bool admitted{};
            do {
                admitted = Telemetry::Runtime::EmitRecord({.subsystem = "prefab", .payload = Telemetry::LogRecord{}});
                if (!admitted)
                    std::this_thread::yield();
            } while (!admitted && std::chrono::steady_clock::now() < deadline);
            REQUIRE(admitted);
            while (!sink->entered && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            REQUIRE(sink->entered);
        }
    };
}  // namespace Horo::Prefab::MetricTest
