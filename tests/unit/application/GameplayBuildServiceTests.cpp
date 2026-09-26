#include "Horo/Application/CompilerDiagnosticParser.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Application;

    class TemporaryProject final {
    public:
        TemporaryProject() {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() / ("horo-gameplay-build-test-" + std::to_string(nonce));
            std::filesystem::create_directories(root / "source/gameplay");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void WriteValid() const {
            Write(root / "CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.25)
project(HoroGameplayBuildTest LANGUAGES CXX)
find_package(HoroEngineGameplay CONFIG REQUIRED)
horo_add_gameplay_module(HoroGameGameplay
    MODULE_ID tests.gameplay_build
    SOURCES source/gameplay/GameModule.cpp source/gameplay/Movement.cpp
)
)");
            Write(root / "source/gameplay/GameModule.cpp", R"(#include <Horo/Gameplay/GameModule.h>
namespace { class Module final : public Horo::Gameplay::IGameModule {
public:
    Horo::Result<void> Register(Horo::Gameplay::GameRegistrationContext&) override { return Horo::Result<void>::Success(); }
    Horo::Result<void> Start(Horo::Gameplay::GameRuntimeContext&) override { return Horo::Result<void>::Success(); }
    void Stop(Horo::Gameplay::GameRuntimeContext&) noexcept override {}
}; }
extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule* CreateGameModule() noexcept { return new Module{}; }
extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule* module) noexcept { delete module; }
)");
            Write(root / "source/gameplay/Movement.cpp", R"(#include <Horo/Gameplay/NativeBehavior.h>
class Movement final : public Horo::Gameplay::IBehaviorInstance {
public:
    static Horo::Gameplay::BehaviorDescriptor DescribeBehavior() {
        Horo::Gameplay::BehaviorDescriptor descriptor;
        descriptor.displayName = "Movement";
        return descriptor;
    }
};
HORO_BEHAVIOR(Movement, "game.tests.build_movement")
)");
        }

        void BreakSource() const {
            Write(root / "source/gameplay/Movement.cpp", "this is intentionally not valid C++\n");
            // Force the follow-up build through a clean configure so the test does
            // not depend on generator-specific timestamp/change detection.
            std::error_code error;
            std::filesystem::remove_all(root / ".horo/local/build/gameplay-debug", error);
        }

        void AddResolvedInput() const {
            Write(root / "project-settings.txt", "gameplay settings\n");
            std::ofstream manifest{root / ".horo/local/gameplay_build_inputs.txt", std::ios::app};
            manifest << "project-settings.txt\n";
            if (!manifest)
                throw std::runtime_error("Unable to update gameplay build input manifest.");
        }

        std::filesystem::path root;

    private:
        static void Write(const std::filesystem::path &path, const std::string_view bytes) {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream)
                throw std::runtime_error("Unable to prepare gameplay build test project.");
        }
    };

    GameplayBuildSnapshot AwaitTerminal(GameplayBuildService &service, const GameplayBuildSessionId id) {
        for (std::size_t attempt = 0; attempt < 3000; ++attempt) {
            const std::optional<GameplayBuildSnapshot> snapshot = service.Query(id);
            REQUIRE(snapshot.has_value());
            if (snapshot->state == GameplayBuildState::Succeeded || snapshot->state == GameplayBuildState::Failed ||
                snapshot->state == GameplayBuildState::Cancelled || snapshot->state == GameplayBuildState::TimedOut)
                return *snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        FAIL("Gameplay build session did not become terminal.");
    }

    std::optional<BuildOutputSnapshot> AwaitTerminalOutput(BuildOutputStore &output, const GameplayBuildSnapshot &terminal) {
        for (std::size_t attempt = 0; attempt < 3000; ++attempt) {
            if (const std::optional<BuildOutputSnapshot> snapshot = output.SnapshotIfChanged(0);
                snapshot.has_value() && std::ranges::count_if(snapshot->records, [&](const BuildOutputRecord &record) {
                return record.operationId == terminal.operationId && record.result != BuildOutputResult::None;
            }) == 1)
                return snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        FAIL("Gameplay build terminal output was not published.");
    }

    std::optional<BuildOutputSessionId> AssertSuccessfulBuildOutput(const BuildOutputSnapshot &output,
                                                                    const GameplayBuildSnapshot &success) {
        REQUIRE(success.state == GameplayBuildState::Succeeded);
        REQUIRE(success.operationId.has_value());
        REQUIRE_FALSE(output.records.empty());
        const auto session = output.records.front().sessionId;
        REQUIRE(session.has_value());
        REQUIRE(session->IsValid());
        for (const BuildOutputRecord &record : output.records) {
            REQUIRE(record.sessionId == session);
            REQUIRE(record.operationId == success.operationId);
        }
        REQUIRE((std::ranges::count_if(output.records, [](const BuildOutputRecord &record) {
            return record.result != BuildOutputResult::None;
        }) == 1));
        REQUIRE(output.records.back().result == BuildOutputResult::Succeeded);
        REQUIRE(output.records.back().code.Value() == "gameplay.build.succeeded");
        std::vector<std::string> stages;
        for (const BuildOutputRecord &record : output.records) {
            if (record.sessionId == session && record.result == BuildOutputResult::None &&
                (record.code.Value() == "gameplay.build.started" || record.code.Value() == "gameplay.build.configure_started" ||
                 record.code.Value() == "gameplay.build.build_started" || record.code.Value() == "gameplay.build.validation_started"))
                stages.push_back(record.stage);
        }
        REQUIRE(stages == std::vector<std::string>{"started", "configure", "build", "validate"});
        REQUIRE((std::ranges::none_of(output.records, [&](const BuildOutputRecord &record) {
            return record.sessionId == session && record.stage == "lock";
        })));
        return session;
    }

    void AssertCachedBuildOutput(const BuildOutputSnapshot &output) {
        REQUIRE_FALSE(output.records.empty());
        const auto session = output.records.back().sessionId;
        REQUIRE(session.has_value());
        REQUIRE((std::ranges::count_if(output.records, [&](const BuildOutputRecord &record) {
            return record.sessionId == session && record.result != BuildOutputResult::None;
        }) == 1));
        REQUIRE(output.records.back().result == BuildOutputResult::Cached);
        REQUIRE(output.records.back().code.Value() == "gameplay.build.cached");
        REQUIRE((std::ranges::any_of(output.records, [&](const BuildOutputRecord &record) {
            return record.sessionId == session && record.code.Value() == "gameplay.build.cache_hit";
        })));
        REQUIRE((std::ranges::none_of(output.records, [&](const BuildOutputRecord &record) {
            return record.sessionId == session && (record.stage == "configure" || record.stage == "build");
        })));
    }

    void AssertFailedBuildOutput(const BuildOutputSnapshot &output, const BuildOutputSessionId &successfulSession,
                                 const GameplayBuildSnapshot &failure) {
        REQUIRE(failure.state == GameplayBuildState::Failed);
        REQUIRE(failure.operationId.has_value());
        const auto failedSession = output.records.back().sessionId;
        REQUIRE(failedSession.has_value());
        REQUIRE(failedSession != successfulSession);
        REQUIRE((std::ranges::count_if(output.records, [&](const BuildOutputRecord &record) {
            return record.sessionId == failedSession && record.result != BuildOutputResult::None;
        }) == 1));
        REQUIRE(output.records.back().result == BuildOutputResult::Failed);
        REQUIRE(output.records.back().code.Value() == "gameplay.build.failed");
        REQUIRE(output.records.back().operationId == failure.operationId);
    }

    std::string Read(const std::filesystem::path &path) {
        std::ifstream stream{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    class CompilerDiagnosticProcessRunner final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &) override {
            const std::filesystem::path absoluteSource = request.workingDirectory / "source/gameplay/Movement.cpp";
            request.onOutput({ProcessOutputStream::StandardError,
                              "source/gameplay/Movement.cpp:12:7: warning: deprecated gameplay declaration [-Wdeprecated-declarations]"});
            request.onOutput({ProcessOutputStream::StandardError, absoluteSource.string() + ":14:3: error: invalid gameplay declaration"});
            request.onOutput(
                {ProcessOutputStream::StandardError, "source/gameplay/Movement.cpp:99999999999999999999:1: error: invalid coordinate"});
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 1});
        }
    };

    class CountingExternalProcessRunner final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &cancellation) override {
            calls.fetch_add(1, std::memory_order_relaxed);
            return native.Run(request, cancellation);
        }

        std::atomic<std::size_t> calls{};

    private:
        NativeExternalProcessRunner native;
    };

    class TerminalProcessRunner final : public IExternalProcessRunner {
    public:
        explicit TerminalProcessRunner(const ProcessTerminationReason reason) : reason_(reason) {}

        Result<ExternalProcessResult> Run(const ExternalProcessRequest &, const CancellationToken &) override {
            calls.fetch_add(1, std::memory_order_relaxed);
            callCondition_.notify_one();
            return Result<ExternalProcessResult>::Success({reason_, reason_ == ProcessTerminationReason::Exited ? 0 : 1});
        }

        [[nodiscard]] bool WaitForCall(const std::chrono::milliseconds timeout) {
            std::unique_lock lock{callMutex_};
            return callCondition_.wait_for(lock, timeout, [this] {
                return calls.load(std::memory_order_relaxed) != 0U;
            });
        }

        std::atomic<std::size_t> calls{};

    private:
        ProcessTerminationReason reason_;
        std::condition_variable callCondition_;
        std::mutex callMutex_;
    };

    class CancellationWaitingProcessRunner final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &, const CancellationToken &cancellation) override {
            {
                std::lock_guard lock(mutex_);
                running_ = true;
            }
            condition_.notify_one();
            while (!cancellation.IsCancellationRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Cancelled, 1});
        }

        [[nodiscard]] bool WaitUntilRunning() {
            std::unique_lock lock(mutex_);
            return condition_.wait_for(lock, std::chrono::seconds{30}, [this] {
                return running_;
            });
        }

    private:
        std::mutex mutex_;
        std::condition_variable condition_;
        bool running_{};
    };

    template <typename ProcessRunner> class GameplayBuildFixture final {
    public:
        explicit GameplayBuildFixture(ProcessRunner &runner, const std::size_t workerCount = 1U, const std::size_t queueCapacity = 4U,
                                      const std::size_t outputCapacity = 32U, const std::size_t operationCapacity = 4U,
                                      const std::size_t operationQueueCapacity = 4U, const bool withOperations = true)
            : processes(runner), project(), jobs{{workerCount, queueCapacity}}, output(outputCapacity),
              operations(operationCapacity, operationQueueCapacity),
              service(processes, jobs, files, &output, withOperations ? &operations : nullptr) {
            project.WriteValid();
        }

        GameplayBuildFixture(const GameplayBuildFixture &) = delete;
        GameplayBuildFixture &operator=(const GameplayBuildFixture &) = delete;

        ~GameplayBuildFixture() {
            service.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }

        [[nodiscard]] GameplayBuildRequest Request() const {
            return {.projectRoot = project.root, .environment = {.gameplaySdkPackage = HORO_GAMEPLAY_SDK_PACKAGE_DIR}};
        }

        [[nodiscard]] GameplayBuildRequest Request(const std::chrono::milliseconds configureTimeout) const {
            GameplayBuildRequest request = Request();
            request.timeouts.configure = configureTimeout;
            return request;
        }

        ProcessRunner &processes;
        TemporaryProject project;
        JobSystem jobs;
        NativeDurableFileSystem files;
        BuildOutputStore output;
        OperationStore operations;
        GameplayBuildService service;
    };

    void AssertTerminalOperation(OperationStore &operations, const GameplayBuildSnapshot &terminal, const OperationState expectedState) {
        REQUIRE(terminal.operationId.has_value());
        const auto snapshot = operations.SnapshotIfChanged(0);
        REQUIRE(snapshot.has_value());
        const auto operation = std::ranges::find_if(snapshot->operations, [&](const OperationRecord &record) {
            return record.id == *terminal.operationId;
        });
        REQUIRE((operation != snapshot->operations.end()));
        REQUIRE(operation->state == expectedState);
        REQUIRE(operation->finishedAt.has_value());
    }

    void AssertTerminalOutput(const BuildOutputSnapshot &snapshot, const GameplayBuildSnapshot &terminal,
                              const BuildOutputResult expectedResult, const std::string_view expectedCode) {
        REQUIRE((std::ranges::count_if(snapshot.records, [&](const BuildOutputRecord &record) {
            return record.operationId == terminal.operationId && record.result != BuildOutputResult::None;
        }) == 1));
        REQUIRE(snapshot.records.back().result == expectedResult);
        REQUIRE(snapshot.records.back().code.Value() == expectedCode);
    }
}  // namespace

TEST_CASE("Gameplay build service consumes exported SDK and preserves last success on failure", "[integration][gameplay][build]") {
    CountingExternalProcessRunner processes;
    GameplayBuildFixture fixture{processes, 2U, 16U, 1024U, 16U, 64U};
    auto &service = fixture.service;
    auto &project = fixture.project;
    auto &output = fixture.output;
    GameplayBuildRequest request = fixture.Request();
    request.environment.cxxCompiler = std::filesystem::path{HORO_GAMEPLAY_CXX_COMPILER};
    request.environment.generator = HORO_TEST_CMAKE_GENERATOR;
    request.timeouts = {.configure = std::chrono::minutes{1}, .build = std::chrono::minutes{2}};

    const auto started = service.Start(request);
    REQUIRE(started.HasValue());
    const GameplayBuildSnapshot success = AwaitTerminal(service, started.Value());
    const std::optional<BuildOutputSnapshot> buildOutput = output.SnapshotIfChanged(0);
    REQUIRE(buildOutput.has_value());
    const std::optional<BuildOutputSessionId> successfulOutputSession = AssertSuccessfulBuildOutput(*buildOutput, success);
    REQUIRE(service.IsUpToDate(request));
    REQUIRE(std::filesystem::is_regular_file(project.root / ".horo/local/gameplay_module.json"));

    const std::size_t callsAfterBuild = processes.calls.load(std::memory_order_relaxed);
    const auto cached = service.Start(request);
    REQUIRE(cached.HasValue());
    const GameplayBuildSnapshot cachedSession = AwaitTerminal(service, cached.Value());
    REQUIRE(cachedSession.state == GameplayBuildState::Succeeded);
    REQUIRE((processes.calls.load(std::memory_order_relaxed) == callsAfterBuild));
    const std::optional<BuildOutputSnapshot> cachedOutput = output.SnapshotIfChanged(buildOutput->revision);
    REQUIRE(cachedOutput.has_value());
    AssertCachedBuildOutput(*cachedOutput);
    project.AddResolvedInput();
    REQUIRE_FALSE(service.IsUpToDate(request));
    const std::filesystem::path successfulState = project.root / ".horo/local/gameplay_build_state.json";
    const std::string beforeFailure = Read(successfulState);
    REQUIRE_FALSE(beforeFailure.empty());

    project.BreakSource();
    const auto broken = service.Start(request);
    REQUIRE(broken.HasValue());
    const GameplayBuildSnapshot failure = AwaitTerminal(service, broken.Value());
    const std::optional<BuildOutputSnapshot> failedOutput = output.SnapshotIfChanged(buildOutput->revision);
    REQUIRE(failedOutput.has_value());
    REQUIRE(successfulOutputSession.has_value());
    AssertFailedBuildOutput(*failedOutput, *successfulOutputSession, failure);
    REQUIRE(Read(successfulState) == beforeFailure);
    REQUIRE(std::filesystem::is_regular_file(project.root / ".horo/local/gameplay_module.json"));
    REQUIRE_FALSE(service.IsUpToDate(request));
}

TEST_CASE("Gameplay build output classifies bounded GCC and Clang diagnostics", "[unit][gameplay][build]") {
    CompilerDiagnosticProcessRunner processes;
    GameplayBuildFixture fixture{processes, 1U, 4U, 16U, 4U, 4U, false};
    auto &service = fixture.service;
    auto &project = fixture.project;
    auto &output = fixture.output;
    const GameplayBuildRequest request = fixture.Request();

    const auto started = service.Start(request);
    REQUIRE(started.HasValue());
    REQUIRE((AwaitTerminal(service, started.Value()).state == GameplayBuildState::Failed));
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());

    const auto findCode = [&](const std::string_view code) {
        return std::ranges::find_if(snapshot->records, [&](const BuildOutputRecord &record) {
            return record.code.Value() == code;
        });
    };
    const auto warning = findCode("gameplay.build.compiler_warning");
    REQUIRE((warning != snapshot->records.end()));
    REQUIRE((warning->severity == DiagnosticSeverity::Warning));
    REQUIRE(warning->source.has_value());
    REQUIRE((std::filesystem::path{warning->source->absolutePath} == (project.root / "source/gameplay/Movement.cpp").lexically_normal()));
    REQUIRE((warning->source->line == 12U));
    REQUIRE((warning->source->column == 7U));
    REQUIRE((warning->toolCode == "-Wdeprecated-declarations"));

    const auto error = findCode("gameplay.build.compiler_error");
    REQUIRE((error != snapshot->records.end()));
    REQUIRE((error->severity == DiagnosticSeverity::Error));
    REQUIRE(error->source.has_value());
    REQUIRE(std::filesystem::path{error->source->absolutePath}.is_absolute());
    REQUIRE((error->source->line == 14U));
    REQUIRE((error->source->column == 3U));

    const auto malformed = std::ranges::find_if(snapshot->records, [](const BuildOutputRecord &record) {
        return record.message.find("invalid coordinate") != std::string::npos;
    });
    REQUIRE((malformed != snapshot->records.end()));
    REQUIRE_FALSE(malformed->source.has_value());
    REQUIRE((malformed->severity == DiagnosticSeverity::Note));
    REQUIRE((malformed->code.Value() == "gameplay.build.output"));
}

TEST_CASE("Gameplay build service maps cancellation to one correlated terminal record", "[unit][gameplay][build][cancellation]") {
    CancellationWaitingProcessRunner processes;
    GameplayBuildFixture fixture{processes};

    const auto started = fixture.service.Start(fixture.Request(std::chrono::seconds{1}));
    REQUIRE(started.HasValue());
    REQUIRE(processes.WaitUntilRunning());
    REQUIRE(fixture.service.RequestCancel(started.Value()));
    const GameplayBuildSnapshot terminal = AwaitTerminal(fixture.service, started.Value());
    REQUIRE(terminal.state == GameplayBuildState::Cancelled);
    REQUIRE(terminal.operationId.has_value());
    const auto snapshot = AwaitTerminalOutput(fixture.output, terminal);
    REQUIRE(snapshot.has_value());
    AssertTerminalOutput(*snapshot, terminal, BuildOutputResult::Cancelled, "gameplay.build.cancelled");
    AssertTerminalOperation(fixture.operations, terminal, OperationState::Cancelled);
    REQUIRE_FALSE(fixture.service.RequestCancel(started.Value()));
}

TEST_CASE("Gameplay build shutdown joins active work and publishes terminal output before returning", "[unit][gameplay][build][shutdown]") {
    CancellationWaitingProcessRunner processes;
    GameplayBuildFixture fixture{processes};
    const auto started = fixture.service.Start(fixture.Request());
    REQUIRE(started.HasValue());
    REQUIRE(processes.WaitUntilRunning());

    fixture.service.Shutdown();

    const std::optional<GameplayBuildSnapshot> terminal = fixture.service.Query(started.Value());
    REQUIRE(terminal.has_value());
    REQUIRE(terminal->state == GameplayBuildState::Cancelled);
    const std::optional<BuildOutputSnapshot> output = fixture.output.SnapshotIfChanged(0);
    REQUIRE(output.has_value());
    AssertTerminalOutput(*output, *terminal, BuildOutputResult::Cancelled, "gameplay.build.cancelled");
    AssertTerminalOperation(fixture.operations, *terminal, OperationState::Cancelled);
    REQUIRE_FALSE(fixture.service.RequestCancel(started.Value()));
}

TEST_CASE("Gameplay build cancellation and shutdown publish one terminal outcome", "[unit][gameplay][build][shutdown][race]") {
    CancellationWaitingProcessRunner processes;
    GameplayBuildFixture fixture{processes};
    const auto started = fixture.service.Start(fixture.Request());
    REQUIRE(started.HasValue());
    REQUIRE(processes.WaitUntilRunning());

    std::thread cancellation{[&fixture, id = started.Value()] {
        static_cast<void>(fixture.service.RequestCancel(id));
    }};
    fixture.service.Shutdown();
    cancellation.join();

    const std::optional<GameplayBuildSnapshot> terminal = fixture.service.Query(started.Value());
    REQUIRE(terminal.has_value());
    REQUIRE(terminal->state == GameplayBuildState::Cancelled);
    const std::optional<BuildOutputSnapshot> output = fixture.output.SnapshotIfChanged(0);
    REQUIRE(output.has_value());
    AssertTerminalOutput(*output, *terminal, BuildOutputResult::Cancelled, "gameplay.build.cancelled");
    AssertTerminalOperation(fixture.operations, *terminal, OperationState::Cancelled);
}

TEST_CASE("Active gameplay build projection survives readers and clears at cancellation", "[unit][gameplay][build][cancellation]") {
    CancellationWaitingProcessRunner processes;
    GameplayBuildFixture fixture{processes};
    const auto started = fixture.service.Start(fixture.Request());
    REQUIRE(started.HasValue());
    REQUIRE(processes.WaitUntilRunning());

    const auto active = fixture.service.QueryActiveProject(fixture.project.root);
    REQUIRE(active.has_value());
    REQUIRE(active->id == started.Value());
    REQUIRE(active->startedAt <= std::chrono::steady_clock::now());
    REQUIRE(active->progress.has_value());
    REQUIRE(*active->progress > 0.0F);
    REQUIRE_FALSE(active->cancellationRequested);
    REQUIRE_FALSE(fixture.service.QueryActiveProject(fixture.project.root / "other-project").has_value());

    std::atomic<bool> reading{true};
    std::atomic<bool> invalidSnapshot{false};
    std::thread reader{[&] {
        while (reading.load(std::memory_order_relaxed)) {
            const auto snapshot = fixture.service.QueryActiveProject(fixture.project.root);
            if (snapshot.has_value() && (snapshot->id != started.Value() || snapshot->state == GameplayBuildState::Cancelled))
                invalidSnapshot.store(true, std::memory_order_relaxed);
        }
    }};
    const bool requested = fixture.service.RequestCancel(active->id);
    const auto cancelling = fixture.service.QueryActiveProject(fixture.project.root);
    const bool duplicateRequest = fixture.service.RequestCancel(active->id);
    reading.store(false, std::memory_order_relaxed);
    reader.join();
    REQUIRE(requested);
    if (cancelling.has_value())
        REQUIRE(cancelling->cancellationRequested);
    REQUIRE_FALSE(duplicateRequest);
    REQUIRE_FALSE(invalidSnapshot.load(std::memory_order_relaxed));
    REQUIRE(AwaitTerminal(fixture.service, active->id).state == GameplayBuildState::Cancelled);
    REQUIRE_FALSE(fixture.service.QueryActiveProject(fixture.project.root).has_value());
}

TEST_CASE("Gameplay build service maps process timeout to one correlated terminal record", "[unit][gameplay][build][timeout]") {
    TerminalProcessRunner processes{ProcessTerminationReason::TimedOut};
    GameplayBuildFixture fixture{processes};

    const auto started = fixture.service.Start(fixture.Request(std::chrono::milliseconds{20}));
    REQUIRE(started.HasValue());
    const GameplayBuildSnapshot terminal = AwaitTerminal(fixture.service, started.Value());
    REQUIRE(terminal.state == GameplayBuildState::TimedOut);
    REQUIRE(terminal.operationId.has_value());
    const auto snapshot = fixture.output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    AssertTerminalOutput(*snapshot, terminal, BuildOutputResult::TimedOut, "gameplay.build.timed_out");
    AssertTerminalOperation(fixture.operations, terminal, OperationState::Failed);
}

TEST_CASE("Gameplay build service exposes external lock waiting and bounded timeout", "[unit][gameplay][build][lock]") {
    TerminalProcessRunner processes{ProcessTerminationReason::Exited};
    GameplayBuildFixture fixture{processes};
    auto &service = fixture.service;
    auto &project = fixture.project;
    auto &files = fixture.files;
    auto &output = fixture.output;
    auto &operations = fixture.operations;
    const auto held = files.TryAcquireExclusive(project.root / ".horo/local/locks/gameplay-build.lock", std::string(4096U, 'x'));
    REQUIRE(held.HasValue());
    GameplayBuildRequest request = fixture.Request();
    request.timeouts.externalWait = std::chrono::milliseconds{80};

    const auto started = service.Start(request);
    REQUIRE(started.HasValue());
    bool observedWaiting = false;
    for (std::size_t attempt = 0; attempt < 100 && !observedWaiting; ++attempt) {
        if (const auto snapshot = operations.SnapshotIfChanged(0); snapshot.has_value())
            observedWaiting = std::ranges::any_of(snapshot->operations, [](const OperationRecord &record) {
                return record.state == OperationState::Waiting && record.phase == "waiting_external_build";
            });
        if (!observedWaiting)
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(observedWaiting);
    const GameplayBuildSnapshot terminal = AwaitTerminal(service, started.Value());
    REQUIRE(terminal.state == GameplayBuildState::TimedOut);
    REQUIRE(terminal.operationId.has_value());
    REQUIRE(terminal.externalLockOwner.size() <= 512U);
    REQUIRE((processes.calls.load(std::memory_order_relaxed) == 0U));

    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE((std::ranges::any_of(snapshot->records, [](const BuildOutputRecord &record) {
        return record.code.Value() == "gameplay.build.waiting_for_external_lock";
    })));
    REQUIRE((snapshot->records.back().result == BuildOutputResult::TimedOut));
}

TEST_CASE("Gameplay build service rejects operation-store admission without uncorrelated output", "[unit][gameplay][build][admission]") {
    TerminalProcessRunner processes{ProcessTerminationReason::Exited};
    GameplayBuildFixture fixture{processes, 1U, 4U, 32U, 1U, 4U};
    auto &service = fixture.service;
    auto &output = fixture.output;
    auto &operations = fixture.operations;
    const auto occupied = operations.Begin({.kind = OperationKind::Build, .title = "Existing build"});
    REQUIRE(occupied.has_value());
    const GameplayBuildRequest request = fixture.Request();

    const auto rejected = service.Start(request);
    REQUIRE(rejected.HasError());
    REQUIRE((rejected.ErrorValue().code.Value() == "operation_admission_failed"));
    REQUIRE_FALSE(output.SnapshotIfChanged(0).has_value());
    REQUIRE((processes.calls.load(std::memory_order_relaxed) == 0U));
}
