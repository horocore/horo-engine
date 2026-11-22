#include "Horo/Application/GameplayTestService.h"

#include "GameplayBuildInvocation.h"
#include "Horo/Foundation/Platform.h"

#include <algorithm>

namespace Horo::Application {
    struct GameplayTestService::Attempt final {
        OperationId operation{};
        BuildOutputSessionId output;
        CancellationSource cancellation;
    };

    namespace {
        const ErrorCodeDescriptor Unavailable{ErrorDomainId{"gameplay.test"}, ErrorCode{"unavailable"}, ErrorSeverity::Error,
                                              "A configured project test inventory and native test runner are required."};
        const ErrorCodeDescriptor Busy{ErrorDomainId{"gameplay.test"}, ErrorCode{"busy"}, ErrorSeverity::Warning,
                                       "A gameplay test already owns the test service, or the operation store is full."};
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"gameplay.test"}, ErrorCode{"cancelled"}, ErrorSeverity::Info,
                                            "Gameplay tests were cancelled."};
        const ErrorCodeDescriptor TimedOut{ErrorDomainId{"gameplay.test"}, ErrorCode{"timed_out"}, ErrorSeverity::Error,
                                           "Gameplay tests timed out."};
        const ErrorCodeDescriptor Failed{ErrorDomainId{"gameplay.test"}, ErrorCode{"failed"}, ErrorSeverity::Error,
                                         "Gameplay tests failed."};

        /** @brief Hosts choose a single CMake configuration token without argument syntax. */
        bool ValidConfiguration(const std::string_view value) {
            return !value.empty() && value.size() <= 64 && value.front() != '-' && std::ranges::all_of(value, [](const char letter) {
                return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') ||
                       letter == '-' || letter == '_';
            });
        }

        /** @brief Validates the host-selected executable and project before allocating an operation. */
        Result<ExternalProcessRequest> Prepare(const std::filesystem::path &root, const GameplayTestEnvironment &environment) {
            using Output = Result<ExternalProcessRequest>;
            if (root.empty() || !environment.ctest.is_absolute() || !ValidConfiguration(environment.configuration))
                return Output::Failure(MakeError(Unavailable));
            std::error_code error;
            const auto project = std::filesystem::canonical(root, error);
            if (error || !std::filesystem::is_directory(project, error))
                return Output::Failure(MakeError(Unavailable));
            const auto executable = std::filesystem::canonical(environment.ctest, error);
            if (error || !std::filesystem::is_regular_file(executable, error))
                return Output::Failure(MakeError(Unavailable));
            const auto spelling = executable.u8string();
            return Output::Success({.executable = {spelling.begin(), spelling.end()},
                                    .arguments = {"-C", environment.configuration, "--output-on-failure", "--no-tests=error"},
                                    .workingDirectory = project,
                                    .timeout = std::chrono::minutes{5},
                                    .maximumLineBytes = 2048,
                                    .maximumOutputBytes = 128U * 1024U});
        }

        /** @brief Accepts no redirect between the canonical project root and the configured test inventory. */
        bool ExactNativePath(const std::filesystem::path &expected) {
            std::error_code error;
            const auto actual = std::filesystem::weakly_canonical(expected, error);
            return !error && actual == expected;
        }

        /** @brief Maps the drained child result to actionable, transport-neutral application status. */
        Result<void> TestOutcome(const ExternalProcessResult &result) {
            if (result.reason == ProcessTerminationReason::Cancelled || result.stopCause == ProcessStopCause::Cancellation)
                return JobCancelled(MakeError(Cancelled));
            if (result.reason == ProcessTerminationReason::TimedOut || result.stopCause == ProcessStopCause::Timeout)
                return Result<void>::Failure(MakeError(TimedOut));
            return result.reason == ProcessTerminationReason::Exited && result.exitCode == 0 ? Result<void>::Success()
                                                                                             : Result<void>::Failure(MakeError(Failed));
        }

        /** @brief Holds the existing build lock while resolving and executing CTest. */
        Result<void> RunTests(IExternalProcessRunner &processes, DurableFileSystem &files, ExternalProcessRequest request,
                              const CancellationToken &stop) {
            if (stop.IsCancellationRequested())
                return JobCancelled(MakeError(Cancelled));
            const auto lockPath = Detail::GameplayBuildLockPath(request.workingDirectory);
            if (!ExactNativePath(lockPath))
                return Result<void>::Failure(MakeError(Unavailable));
            const auto lock = files.TryAcquireExclusive(lockPath, "gameplay tests");
            if (lock.HasError())
                return Result<void>::Failure(lock.ErrorValue());
            const auto build = GameplayLocalBuildDirectory(request.workingDirectory);
            const auto inventory = build / "CTestTestfile.cmake";
            std::error_code error;
            if (!ExactNativePath(inventory) || !std::filesystem::is_regular_file(inventory, error))
                return Result<void>::Failure(MakeError(Unavailable));
            request.workingDirectory = build;
            auto result = processes.Run(request, stop);
            return result.HasValue() ? TestOutcome(result.Value()) : Result<void>::Failure(std::move(result).ErrorValue());
        }

        /** @brief Preserves timeout as a distinct terminal Build Output result. */
        BuildOutputResult OutputResult(const OperationState state, const std::optional<Error> &error) {
            if (state == OperationState::Succeeded)
                return BuildOutputResult::Succeeded;
            if (state == OperationState::Cancelled)
                return BuildOutputResult::Cancelled;
            if (error && error->domain.Value() == TimedOut.domain.Value() && error->code.Value() == TimedOut.code.Value())
                return BuildOutputResult::TimedOut;
            return BuildOutputResult::Failed;
        }
    }  // namespace

    /** @copydoc GameplayTestService::GameplayTestService */
    GameplayTestService::GameplayTestService(IExternalProcessRunner &processes, JobSystem &jobs, DurableFileSystem &files,
                                             OperationStore &operations, BuildOutputStore &output)
        : processes_(processes), jobs_(jobs), files_(files), operations_(operations), output_(output) {}

    GameplayTestService::~GameplayTestService() {
        Close();
    }

    /** @copydoc GameplayTestService::Start */
    Result<OperationId> GameplayTestService::Start(const std::filesystem::path &projectRoot, const GameplayTestEnvironment &environment,
                                                   const std::function<bool()> &stop) {
        using Output = Result<OperationId>;
        if (closed_ || owner_ != std::this_thread::get_id())
            return Output::Failure(MakeError(Unavailable));
        Poll();
        if (active_)
            return Output::Failure(MakeError(Busy));
        auto request = Prepare(projectRoot, environment);
        if (request.HasError())
            return Output::Failure(std::move(request).ErrorValue());
        if (stop && stop())
            return Output::Failure(MakeError(Cancelled));
        return Submit(std::move(request).Value());
    }

    /** @copydoc GameplayTestService::Submit */
    Result<OperationId> GameplayTestService::Submit(ExternalProcessRequest request) {
        using Output = Result<OperationId>;
        const auto output = output_.BeginSession();
        if (!output)
            return Output::Failure(MakeError(Busy));
        auto attempt = std::make_shared<Attempt>();
        attempt->output = *output;
        const auto id = operations_.Begin({.kind = OperationKind::Validation,
                                           .title = "Gameplay Tests",
                                           .phase = "queued",
                                           .cancellable = true,
                                           .requestCancel = [weak = std::weak_ptr{attempt}] {
            if (const auto active = weak.lock())
                active->cancellation.RequestCancellation();
        }});
        if (!id)
            return Output::Failure(MakeError(Busy));
        attempt->operation = *id;
        active_ = attempt;
        latest_ = GameplayTestSnapshot{.operation = *id};
        request.onOutput = [store = &output_, id = *id, output = *output](ProcessOutputLine line) {
            store->Append({.timestampUtc = std::chrono::system_clock::now(),
                           .sessionId = output,
                           .operationId = id,
                           .severity = DiagnosticSeverity::Note,
                           .stage = "test",
                           .code = DiagnosticCode{"gameplay.test.output"},
                           .message = std::move(line.text)});
        };
        auto submitted =
            jobs_.SubmitResult({.parentCancellation = attempt->cancellation.Token(), .operationId = *id, .resource = JobResource::Io},
                               [processes = &processes_, files = &files_,
                                run = std::move(request)](const CancellationToken &cancellation) mutable {
            return RunTests(*processes, *files, std::move(run), cancellation);
        });
        if (submitted.HasError()) {
            auto error = std::move(submitted).ErrorValue();
            Finish(OperationState::Failed, error);
            return Output::Failure(std::move(error));
        }
        job_ = std::move(submitted).Value();
        return Output::Success(*id);
    }

    /** @copydoc GameplayTestService::Query */
    std::optional<GameplayTestSnapshot> GameplayTestService::Query(const OperationId id) const {
        return owner_ == std::this_thread::get_id() && latest_ && latest_->operation == id ? latest_ : std::nullopt;
    }

    /** @copydoc GameplayTestService::RequestCancel */
    bool GameplayTestService::RequestCancel(const OperationId id) {
        if (owner_ != std::this_thread::get_id() || !active_ || active_->operation != id || latest_->cancellationRequested)
            return false;
        active_->cancellation.RequestCancellation();
        latest_->cancellationRequested = true;
        return true;
    }

    /** @copydoc GameplayTestService::Poll */
    void GameplayTestService::Poll() {
        if (owner_ != std::this_thread::get_id() || !job_)
            return;
        const auto job = job_->Snapshot();
        if (!job)
            return;
        latest_->cancellationRequested = active_->cancellation.Token().IsCancellationRequested();
        if (job->terminalResult) {
            const auto terminal = job->terminalResult->state == JobState::Succeeded   ? OperationState::Succeeded
                                  : job->terminalResult->state == JobState::Cancelled ? OperationState::Cancelled
                                                                                      : OperationState::Failed;
            Finish(terminal, job->terminalResult->error);
            return;
        }
        const auto state = latest_->cancellationRequested   ? OperationState::Cancelling
                           : job->state == JobState::Queued ? OperationState::Queued
                                                            : OperationState::Running;
        if (state != latest_->state) {
            latest_->state = state;
            static_cast<void>(operations_.Update(active_->operation, {.state = state, .phase = "test"}));
        }
    }

    /** @copydoc GameplayTestService::Finish */
    void GameplayTestService::Finish(const OperationState state, std::optional<Error> error) {
        const std::string message = error ? error->message : "Gameplay tests passed.";
        const auto result = OutputResult(state, error);
        output_.Append({.timestampUtc = std::chrono::system_clock::now(),
                        .sessionId = active_->output,
                        .operationId = active_->operation,
                        .severity = result == BuildOutputResult::Failed || result == BuildOutputResult::TimedOut ? DiagnosticSeverity::Error
                                                                                                                 : DiagnosticSeverity::Note,
                        .result = result,
                        .stage = "terminal",
                        .code = DiagnosticCode{"gameplay.test.terminal"},
                        .message = message});
        static_cast<void>(
            operations_.Update(active_->operation, {.state = state, .phase = "terminal", .message = message, .error = error}));
        latest_->state = state;
        latest_->error = std::move(error);
        job_.reset();
        active_.reset();
    }

    /** @copydoc GameplayTestService::Close */
    void GameplayTestService::Close() {
        if (closed_ || owner_ != std::this_thread::get_id())
            return;
        closed_ = true;
        if (!job_)
            return;
        static_cast<void>(RequestCancel(active_->operation));
        static_cast<void>(job_->Wait());
        Poll();
    }
}  // namespace Horo::Application
