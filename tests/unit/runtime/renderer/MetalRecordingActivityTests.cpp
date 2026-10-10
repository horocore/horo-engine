#include "Horo/Foundation/JobSystem.h"
#include "runtime/renderer/modules/metal/MetalRecordingActivity.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>

namespace Horo::Render::Detail {
    static_assert(!std::is_copy_constructible_v<MetalRecordingActivity::RecordScope>);
    static_assert(!std::is_move_constructible_v<MetalRecordingActivity::RecordScope>);

    TEST_CASE("Native closure cannot authorize owner pin recycling before the actual active record guard drains",
              "[unit][renderer][parallel][metal][lifetime]") {
        JobSystem jobs{{.workerCount = 1}};
        auto activity = std::make_shared<MetalRecordingActivity>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto release = std::make_shared<std::atomic<bool>>(false);
        const auto record = jobs.SubmitResult({}, [activity, entered, release](const CancellationToken &token) {
            const MetalRecordingActivity::RecordScope scope{*activity};
            if (!scope.CanRecord())
                return JobCancelled();
            entered->store(true);
            while (!release->load() && !token.IsCancellationRequested())
                std::this_thread::yield();
            return Result<void>::Success();
        });
        REQUIRE(record.HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!entered->load()) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        activity->Close();
        CHECK(activity->Closed());
        CHECK_FALSE(activity->Idle());
        // MetalRuntime uses this exact Idle fence for cancelled resource lease retirement.
        release->store(true);
        while (!record.Value().Snapshot()->terminalResult) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        REQUIRE(activity->Idle());
        const MetalRecordingActivity::RecordScope delayed{*activity};
        CHECK_FALSE(delayed.CanRecord());
        CHECK_FALSE(activity->Idle());
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}  // namespace Horo::Render::Detail
