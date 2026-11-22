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

    /** @brief Runs one configured test while optionally simulating a concurrent source change. */
    class ValidationTestRunner final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &stop) override {
            ++calls;
            if (!change.empty())
                std::ofstream{change, std::ios::binary} << "// changed during tests\n";
            return native_.Run(request, stop);
        }

        std::filesystem::path change;
        std::atomic_uint calls{};

    private:
        NativeExternalProcessRunner native_;
    };

    /** @brief Uses the build service's real input fingerprint and supplies the isolated test's successful publication fixture. */
    GameplayTestBuildInput PublishedBuild(Fixture &fixture) {
        BlockingTestRunner process;
        GameplayBuildService builds{process, fixture.jobs, fixture.files};
        GameplayBuildRequest request{.projectRoot = fixture.directory.Path()};
        const auto started = builds.StartIsolated(request, {});
        REQUIRE(started.HasValue());
        const auto snapshot = builds.Query(started.Value());
        REQUIRE(snapshot);
        REQUIRE_FALSE(snapshot->activeInputHash.empty());
        builds.Shutdown();
        std::ofstream state{fixture.directory.Path() / ".horo/local/gameplay_build_state.json", std::ios::binary};
        state << R"({"schemaVersion":1,"successfulInputHash":")" << snapshot->activeInputHash << R"("})";
        state.close();
        REQUIRE(state.good());
        return {std::move(request), snapshot->activeInputHash};
    }

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

    TEST_CASE("Approved gameplay tests reject stale native build evidence before and during execution", "[gameplay][test-service]") {
        ValidationTestRunner process;
        Fixture fixture{process};
        const auto sources = fixture.directory.Path() / "source";
        std::filesystem::create_directory(sources);
        const auto source = sources / "game.cpp";
        std::ofstream{source, std::ios::binary} << "int revision = 1;\n";
        const auto cmake = std::filesystem::path{HORO_GAMEPLAY_TEST_CMAKE}.generic_string();
        fixture.WriteInventory("add_test(sample \"" + cmake + "\" -E echo validated-build)\n");
        auto approved = PublishedBuild(fixture);
        bool staleBefore = false;
        bool staleDuring = false;
        SECTION("Current publication") {
            // The matching input remains current across the child run.
        }
        SECTION("Sources change before CTest starts") {
            staleBefore = true;
            std::ofstream{source, std::ios::binary} << "int revision = 2;\n";
        }
        SECTION("Sources change while CTest runs") {
            staleDuring = true;
            process.change = source;
        }
        SECTION("A different build publication replaced the approved receipt") {
            staleBefore = true;
            approved.successfulInputHash.assign(64, '0');
        }
        SECTION("The editor revision changes while the native inputs stay the same") {
            staleDuring = true;
            approved.request.validateSources = [checks = 0U](const CancellationToken &) mutable {
                if (++checks == 1)
                    return Result<void>::Success();
                static const ErrorCodeDescriptor Stale{ErrorDomainId{"gameplay.test"}, ErrorCode{"build_inputs_stale"},
                                                       ErrorSeverity::Warning, "The editor revision changed."};
                return Result<void>::Failure(MakeError(Stale));
            };
        }

        const auto operation = fixture.tests.StartForBuild(std::move(approved), fixture.environment);
        REQUIRE(operation.HasValue());
        const auto terminal = fixture.Await(operation.Value());
        CHECK(process.calls == (staleBefore ? 0U : 1U));
        if (staleBefore || staleDuring) {
            REQUIRE(terminal.state == OperationState::Failed);
            REQUIRE(terminal.error);
            CHECK(terminal.error->code.Value() == "build_inputs_stale");
        } else {
            CHECK(terminal.state == OperationState::Succeeded);
        }
    }
}  // namespace
