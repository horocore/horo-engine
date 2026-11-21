#pragma once

/** @file OpenXRFrameMetricsTestSupport.h @brief Shares deterministic nonblocking XR telemetry qualification. */
#include "Horo/XR/XRFeatureNegotiation.h"
#include "Horo/XR/XRInteraction.h"
#include "OpenXRFrameTelemetry.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

namespace Horo::Tests::OpenXR {
#if HORO_ENABLE_TELEMETRY
    /** @brief Parks the dispatcher once, then retains immutable records for reads after joined shutdown. */
    class FrameMetricSink final : public Telemetry::ISink {
    public:
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        std::vector<Telemetry::MetricRecord> records;
        std::vector<Telemetry::InstrumentDescriptor> descriptors;

        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
            if (first_) {
                first_ = false;
                entered.release();
                resume.acquire();
            }
            if (const auto *metric = std::get_if<Telemetry::MetricRecord>(&record.payload); metric && descriptor) {
                records.push_back(*metric);
                descriptors.push_back(*descriptor);
            }
        }

        void Flush() override {}

        double Total(XR::OpenXRInternal::NativeFrameMetricOutcome outcome) const {
            return Total("horo.xr.native_frame", static_cast<unsigned>(outcome));
        }

        double Total(XR::OpenXRInternal::NativeSwapchainMetricOutcome outcome) const {
            return Total("horo.xr.native_swapchain", static_cast<unsigned>(outcome));
        }

        double Total(XR::XRFeatureDecisionState state) const {
            return Total("horo.xr.fixed_foveation_plan", static_cast<unsigned>(state));
        }

        double Total(XR::XRStageBoundaryProximity proximity) const {
            return Total("horo.xr.stage_boundary", static_cast<unsigned>(proximity));
        }

        double Total(std::string_view name, unsigned outcome) const {
            double total{};
            for (std::size_t index = 0; index < records.size(); ++index)
                if (descriptors[index].name == name && records[index].dimensionValueIds[0] == outcome + 1)
                    total += records[index].value;
            return total;
        }

    private:
        bool first_{true};
    };

    /** @brief Always releases the test's dispatcher barrier before retiring the process-local Telemetry runtime. */
    struct FrameMetricRecorder final {
        std::shared_ptr<FrameMetricSink> sink{std::make_shared<FrameMetricSink>()};
        XR::OpenXRInternal::NativeFrameMetricHandles handles;
        bool released{};

        void Start(std::size_t queueCapacity = 128) {
            REQUIRE(Telemetry::Runtime::Shutdown());
            REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = queueCapacity}, sink));
            handles = XR::OpenXRInternal::RegisterNativeFrameMetricHandles();
            REQUIRE(handles.outcomes[0]);
            REQUIRE(handles.swapchain.outcomes[0]);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            bool accepted{};
            do {
                accepted = Telemetry::Runtime::EmitRecord({.subsystem = "XR.Test", .payload = Telemetry::LogRecord{}});
                if (!accepted)
                    std::this_thread::yield();
            } while (!accepted && std::chrono::steady_clock::now() < deadline);
            REQUIRE(accepted);
            REQUIRE(sink->entered.try_acquire_for(std::chrono::seconds{1}));
        }

        ~FrameMetricRecorder() {
            if (!released)
                sink->resume.release();
            static_cast<void>(Telemetry::Runtime::Shutdown());
        }

        void Finish() {
            released = true;
            sink->resume.release();
            REQUIRE(Telemetry::Runtime::Shutdown());
        }
    };
#endif
}  // namespace Horo::Tests::OpenXR
