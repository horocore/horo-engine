#include "EditorMicrophoneMonitor.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Editor {
    namespace {
        using namespace Audio;
        using Phase = EditorMicrophoneMonitorPhase;
        using Clock = std::chrono::steady_clock;

        /** @brief Drives the ordinary control loop while the SDL dummy driver owns the callback. */
        void AdvanceUntil(EditorMicrophoneMonitor &monitor, const Phase phase, const bool authorized) {
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (monitor.Phase() != phase && Clock::now() < deadline) {
                const auto result = monitor.Pump(authorized);
                INFO((result.HasError() ? result.ErrorValue().message : ""));
                REQUIRE(result.HasValue());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(monitor.Phase() == phase);
        }

        TEST_CASE("The editor opens one native monitor route and withdraws it on lost consent", "[audio][editor][monitor][sdl3]") {
            const auto owner = AudioRuntimeId::Create(67).Value();
            auto discovered = EditorMicrophoneMonitor::Discover(owner);
            REQUIRE(discovered.HasValue());
            auto monitor = std::move(discovered).Value();
            const auto inputs = monitor->Inputs();
            const auto outputs = monitor->Outputs();
            const auto input = std::ranges::find_if(inputs, [](const auto &device) { return device.preferredFormat.has_value(); });
            const auto pinned = std::ranges::find(outputs, AudioDeviceClass::Physical, &AudioDiscoveredDevice::deviceClass);
            const auto migrating = std::ranges::find(outputs, AudioDeviceClass::Virtual, &AudioDiscoveredDevice::deviceClass);
            REQUIRE(input != inputs.end());
            REQUIRE(pinned != outputs.end());
            REQUIRE(migrating != outputs.end());
            auto jobs = std::make_shared<JobSystem>(JobSystemConfig{.workerCount = 1});
            const auto unconfirmed = monitor->Start(input->id, pinned->id, false, jobs);
            REQUIRE(unconfirmed.HasError());
            CHECK(unconfirmed.ErrorValue().code.Value() == AudioErrors::CapturePermissionDenied.code.Value());
            CHECK(monitor->Start(input->id, migrating->id, true, jobs).HasError());
            REQUIRE(monitor->Phase() == Phase::Ready);
            const auto started = monitor->Start(input->id, pinned->id, true, jobs);
            INFO((started.HasError() ? started.ErrorValue().message : ""));
            REQUIRE(started.HasValue());
            REQUIRE(monitor->Phase() == Phase::Opening);
            SECTION("revocation during output startup prevents capture admission") {}
            SECTION("revocation after activation retires live capture") {
                AdvanceUntil(*monitor, Phase::Listening, true);
                REQUIRE(monitor->Pump(true).HasValue());
            }
            const auto revoked = monitor->Pump(false);
            REQUIRE(revoked.HasError());
            CHECK(revoked.ErrorValue().code.Value() == AudioErrors::CapturePermissionDenied.code.Value());
            REQUIRE(monitor->Phase() == Phase::Closing);
            AdvanceUntil(*monitor, Phase::Closed, false);
        }
    }  // namespace
}  // namespace Horo::Editor
