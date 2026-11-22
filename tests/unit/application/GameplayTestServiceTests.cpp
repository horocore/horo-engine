#include "../../support/OwnedTestDirectory.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/GameplayTestService.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace std::chrono_literals;

    struct Fixture final {
        Horo::Tests::OwnedTestDirectory directory{"horo-gameplay-tests"};
        NativeDurableFileSystem files;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4, .ioWorkerCount = 1}};
        OperationStore operations{4, 8};
        BuildOutputStore output{64};
        GameplayTestService tests;
        GameplayTestEnvironment environment{HORO_GAMEPLAY_TEST_CTEST};

        explicit Fixture(IExternalProcessRunner &process) : tests(process, jobs, files, operations, output) {
            std::filesystem::create_directories(directory.Path() / ".horo/local/locks");
            std::filesystem::create_directories(GameplayLocalBuildDirectory(directory.Path()));
        }

        void WriteInventory(std::string_view content) {
            std::ofstream(GameplayLocalBuildDirectory(directory.Path()) / "CTestTestfile.cmake", std::ios::binary) << content;
        }

        OperationId StartConfiguredTests() {
            const auto started = tests.Start(directory.Path(), environment);
            INFO((started.HasError() ? started.ErrorValue().message : ""));
            REQUIRE(started.HasValue());
            return started.Value();
        }

        GameplayTestSnapshot Await(const OperationId id) {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (std::chrono::steady_clock::now() < deadline) {
                tests.Poll();
                const auto status = tests.Query(id);
                REQUIRE(status);
                if (status->state == OperationState::Succeeded || status->state == OperationState::Failed ||
                    status->state == OperationState::Cancelled)
                    return *status;
                std::this_thread::sleep_for(2ms);
            }
            FAIL("Test operation did not reach a terminal state");
            return {};
        }

        void CheckOutput(const OperationId id, const BuildOutputResult result, const OperationState state) {
            const auto outputSnapshot = output.SnapshotIfChanged(0);
            REQUIRE(outputSnapshot);
            CHECK(std::ranges::any_of(outputSnapshot->records, [&](const auto &record) {
                return record.operationId == id && record.result == result && record.timestampUtc.time_since_epoch().count() > 0;
            }));
            const auto native = operations.SnapshotIfChanged(0);
            REQUIRE(native);
            CHECK(std::ranges::any_of(native->operations, [&](const auto &operation) {
                return operation.id == id && operation.state == state && operation.kind == OperationKind::Validation;
            }));
        }
    };

    class BlockingTestRunner final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &, const CancellationToken &stop) override {
            entered.store(true, std::memory_order_release);
            while (!stop.IsCancellationRequested())
                std::this_thread::sleep_for(2ms);
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Cancelled, 1, ProcessStopCause::Cancellation});
        }

        std::atomic_bool entered{};
    };

    TEST_CASE("Gameplay tests execute the configured inventory and correlate output with native status", "[gameplay][test-service]") {
        NativeExternalProcessRunner processes;
        Fixture fixture{processes};
        const auto cmake = std::filesystem::path{HORO_GAMEPLAY_TEST_CMAKE}.generic_string();
        fixture.WriteInventory("add_test(sample \"" + cmake + "\" -E echo horo-test-result)\n");
        const auto id = fixture.StartConfiguredTests();
        CHECK(fixture.Await(id).state == OperationState::Succeeded);
        fixture.CheckOutput(id, BuildOutputResult::Succeeded, OperationState::Succeeded);

        fixture.WriteInventory("");
        const auto empty = fixture.StartConfiguredTests();
        CHECK(fixture.Await(empty).state == OperationState::Failed);
        fixture.CheckOutput(empty, BuildOutputResult::Failed, OperationState::Failed);
    }

    TEST_CASE("Gameplay tests leave a build reservation untouched and do not start after denied admission", "[gameplay][test-service]") {
        BlockingTestRunner process;
        Fixture fixture{process};
        fixture.WriteInventory("");
        const auto held = fixture.files.TryAcquireExclusive(fixture.directory.Path() / ".horo/local/locks/gameplay-build.lock", "build");
        REQUIRE(held.HasValue());
        CHECK(fixture.tests
                  .Start(fixture.directory.Path(), fixture.environment, [] {
            return true;
        }).HasError());
        const auto untouched = fixture.operations.SnapshotIfChanged(0);
        CHECK((!untouched || untouched->operations.empty()));
        CHECK(fixture.Await(fixture.StartConfiguredTests()).state == OperationState::Failed);
        CHECK_FALSE(process.entered.load(std::memory_order_acquire));
    }

    TEST_CASE("Native operation cancellation stops the exact gameplay test child", "[gameplay][test-service]") {
        BlockingTestRunner process;
        Fixture fixture{process};
        fixture.WriteInventory("");
        const auto id = fixture.StartConfiguredTests();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!process.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        REQUIRE(process.entered.load(std::memory_order_acquire));
        CHECK_FALSE(fixture.tests.RequestCancel(id + 1));
        CHECK(fixture.tests.Start(fixture.directory.Path(), fixture.environment).HasError());
        REQUIRE(fixture.operations.RequestCancel(id));
        CHECK(fixture.Await(id).state == OperationState::Cancelled);
        fixture.CheckOutput(id, BuildOutputResult::Cancelled, OperationState::Cancelled);
        CHECK_FALSE(fixture.tests.RequestCancel(id));
    }
}  // namespace
