#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/PlatformErrors.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cwchar>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <windows.h>

namespace Horo {
    namespace {
        struct Handle final {
            HANDLE value{INVALID_HANDLE_VALUE};
            Handle() = default;

            explicit Handle(HANDLE handle) : value(handle) {}

            ~Handle() {
                if (value != nullptr && value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }

            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;

            Handle(Handle &&other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}

            Handle &operator=(Handle &&other) noexcept {
                if (this != &other) {
                    if (value != nullptr && value != INVALID_HANDLE_VALUE)
                        CloseHandle(value);
                    value = std::exchange(other.value, INVALID_HANDLE_VALUE);
                }
                return *this;
            }
        };

        class LineDecoder final {
        public:
            LineDecoder(const ProcessOutputStream stream, const std::size_t maximum, const std::size_t outputBudget,
                        const std::function<void(ProcessOutputLine)> &callback)
                : stream_(stream), maximum_(std::max<std::size_t>(maximum, 1U)), outputBudget_(outputBudget), callback_(&callback) {}

            void Append(const std::span<const char> bytes) {
                for (const char value : bytes) {
                    if (received_ >= outputBudget_) {
                        truncated_ = true;
                        continue;
                    }
                    ++received_;
                    if (value == '\n')
                        Emit();
                    else if (value != '\r') {
                        if (pending_.size() < maximum_)
                            pending_.push_back(value);
                        else
                            truncated_ = true;
                    }
                }
            }

            void Finish() {
                if (!pending_.empty() || truncated_)
                    Emit();
            }

        private:
            void Emit() {
                if (*callback_)
                    (*callback_)(ProcessOutputLine{stream_, std::move(pending_), truncated_});
                pending_.clear();
                truncated_ = false;
            }

            ProcessOutputStream stream_;
            std::size_t maximum_;
            std::size_t outputBudget_;
            std::size_t received_{};
            const std::function<void(ProcessOutputLine)> *callback_;
            std::string pending_;
            bool truncated_{false};
        };

        [[nodiscard]] Result<std::wstring> ToWide(const std::string_view text) {
            if (text.empty())
                return Result<std::wstring>::Success({});
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (count <= 0)
                return Result<std::wstring>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, "Process text is not UTF-8."));
            std::wstring result(static_cast<std::size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
            return Result<std::wstring>::Success(std::move(result));
        }

        [[nodiscard]] std::wstring QuoteArgument(const std::wstring_view argument) {
            if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
                return std::wstring{argument};
            std::wstring quoted{L"\""};
            std::size_t slashes = 0;
            for (const wchar_t value : argument) {
                if (value == L'\\') {
                    ++slashes;
                    continue;
                }
                if (value == L'\"')
                    quoted.append(slashes * 2U + 1U, L'\\');
                else
                    quoted.append(slashes, L'\\');
                slashes = 0;
                quoted.push_back(value);
            }
            quoted.append(slashes * 2U, L'\\');
            quoted.push_back(L'\"');
            return quoted;
        }

        struct CaseInsensitiveWideLess {
            using is_transparent = void;

            bool operator()(const std::wstring_view left, const std::wstring_view right) const noexcept {
                if (const int cmp = _wcsnicmp(left.data(), right.data(), std::min(left.size(), right.size())); cmp != 0)
                    return cmp < 0;
                return left.size() < right.size();
            }
        };

        [[nodiscard]] Result<std::vector<wchar_t>> BuildEnvironment(const ProcessEnvironment &overlay) {
            std::map<std::wstring, std::wstring, CaseInsensitiveWideLess> values;
            if (overlay.base == ProcessEnvironmentBase::InheritWithOverrides) {
                wchar_t *block = GetEnvironmentStringsW();
                if (block == nullptr)
                    return Result<std::vector<wchar_t>>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
                for (const wchar_t *entry = block; *entry != L'\0'; entry += std::wstring_view{entry}.size() + 1U) {
                    const std::wstring_view text{entry};
                    const std::size_t separator = text.find(L'=', text.starts_with(L'=') ? 1U : 0U);
                    if (separator != std::wstring_view::npos)
                        values.try_emplace(std::wstring{text.substr(0, separator)}, std::wstring{text.substr(separator + 1U)});
                }
                FreeEnvironmentStringsW(block);
            }
            for (const std::string &name : overlay.unset) {
                Result<std::wstring> wide = ToWide(name);
                if (wide.HasError())
                    return Result<std::vector<wchar_t>>::Failure(wide.ErrorValue());
                values.erase(wide.Value());
            }
            for (const ProcessEnvironmentAssignment &assignment : overlay.set) {
                Result<std::wstring> name = ToWide(assignment.name);
                Result<std::wstring> value = ToWide(assignment.value);
                if (name.HasError() || value.HasError())
                    return Result<std::vector<wchar_t>>::Failure(name.HasError() ? name.ErrorValue() : value.ErrorValue());
                values[std::move(name).Value()] = std::move(value).Value();
            }
            std::vector<wchar_t> block;
            for (const auto &[name, value] : values) {
                block.insert(block.end(), name.begin(), name.end());
                block.push_back(L'=');
                block.insert(block.end(), value.begin(), value.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return Result<std::vector<wchar_t>>::Success(std::move(block));
        }

        void DrainAvailable(const HANDLE pipe, bool &open, LineDecoder &decoder) {
            std::array<char, 4096> buffer{};
            for (std::size_t reads = 0; reads < 16; ++reads) {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                    open = false;
                    decoder.Finish();
                    return;
                }
                if (available == 0)
                    return;
                DWORD read = 0;
                if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)), &read, nullptr) ||
                    read == 0) {
                    open = false;
                    decoder.Finish();
                    return;
                }
                decoder.Append(std::span{buffer.data(), static_cast<std::size_t>(read)});
            }
        }

        [[nodiscard]] bool JobHasActiveProcesses(const HANDLE job) noexcept {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            return !QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) ||
                   accounting.ActiveProcesses != 0;
        }

        /** @brief Owns capture, child, and Job handles across launch, monitoring, and terminal mapping. */
        struct CapturedProcess {
            Handle stdoutRead;
            Handle stderrRead;
            Handle process;
            Handle thread;
            Handle job;
            DWORD processId{};
        };

        /** @brief Preserves Windows quoting and UTF-8 validation before any child is created. */
        [[nodiscard]] Result<std::wstring> BuildCommandLine(const ExternalProcessRequest &request) {
            Result<std::wstring> executable = ToWide(request.executable);
            if (executable.HasError())
                return Result<std::wstring>::Failure(executable.ErrorValue());
            std::wstring commandLine = QuoteArgument(executable.Value());
            for (const std::string &argument : request.arguments) {
                Result<std::wstring> wide = ToWide(argument);
                if (wide.HasError())
                    return Result<std::wstring>::Failure(wide.ErrorValue());
                commandLine.push_back(L' ');
                commandLine.append(QuoteArgument(wide.Value()));
            }
            return Result<std::wstring>::Success(std::move(commandLine));
        }

        /** @brief Creates a suspended child and admits it to a kill-on-close Job before resuming. */
        [[nodiscard]] Result<CapturedProcess> LaunchCapturedProcess(const ExternalProcessRequest &request) {
            CapturedProcess captured;
            Handle stdoutWrite;
            Handle stderrWrite;
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            if (!CreatePipe(&captured.stdoutRead.value, &stdoutWrite.value, &security, 0) ||
                !CreatePipe(&captured.stderrRead.value, &stderrWrite.value, &security, 0) ||
                !SetHandleInformation(captured.stdoutRead.value, HANDLE_FLAG_INHERIT, 0) ||
                !SetHandleInformation(captured.stderrRead.value, HANDLE_FLAG_INHERIT, 0))
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessIoFailed));

            auto commandLine = BuildCommandLine(request);
            if (commandLine.HasError())
                return Result<CapturedProcess>::Failure(commandLine.ErrorValue());
            std::wstring commandLineBuffer = std::move(commandLine).Value();
            auto environment = BuildEnvironment(request.environment);
            if (environment.HasError())
                return Result<CapturedProcess>::Failure(environment.ErrorValue());
            std::vector<wchar_t> environmentBlock = std::move(environment).Value();
            const std::wstring workingDirectory = request.workingDirectory.native();
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = stdoutWrite.value;
            startup.hStdError = stderrWrite.value;
            PROCESS_INFORMATION process{};
            const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
            if (!CreateProcessW(nullptr, commandLineBuffer.data(), nullptr, nullptr, TRUE, flags, environmentBlock.data(),
                                workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process))
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            captured.process = Handle{process.hProcess};
            captured.thread = Handle{process.hThread};
            captured.processId = process.dwProcessId;
            stdoutWrite = {};
            stderrWrite = {};

            captured.job = Handle{CreateJobObjectW(nullptr, nullptr)};
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (captured.job.value == nullptr ||
                !SetInformationJobObject(captured.job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(captured.job.value, captured.process.value)) {
                TerminateProcess(captured.process.value, 1);
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            }
            ResumeThread(captured.thread.value);
            return Result<CapturedProcess>::Success(std::move(captured));
        }

        /** @brief Records the terminal stop priority and escalation timestamps for one Job. */
        struct ProcessMonitorState {
            bool terminationRequested{};
            bool forceTerminated{};
            ProcessStopCause stopCause{ProcessStopCause::None};
            std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
            std::chrono::steady_clock::time_point terminationStarted{started};
            std::chrono::steady_clock::time_point forcedAt{started};
        };

        /** @brief Applies the same cancellation, deadline, graceful, and forced Job escalation in priority order. */
        void UpdateTermination(const ExternalProcessRequest &request, const CancellationToken &cancellation,
                               const CapturedProcess &captured, const DWORD wait, const bool jobActive,
                               const std::chrono::steady_clock::time_point now, ProcessMonitorState &state) {
            if (request.forceCancellation.IsCancellationRequested() && !state.forceTerminated) {
                state.stopCause = state.stopCause == ProcessStopCause::None ? ProcessStopCause::Cancellation : state.stopCause;
                state.terminationRequested = true;
                TerminateJobObject(captured.job.value, 1);
                state.forceTerminated = true;
                state.forcedAt = now;
            } else if (const bool cancelled = cancellation.IsCancellationRequested();
                       !state.terminationRequested &&
                       (cancelled || now - state.started >= request.timeout || (wait == WAIT_OBJECT_0 && jobActive))) {
                state.terminationRequested = true;
                state.stopCause = cancelled ? ProcessStopCause::Cancellation
                                            : (wait == WAIT_OBJECT_0 ? ProcessStopCause::DescendantCleanup : ProcessStopCause::Timeout);
                state.terminationStarted = now;
                static_cast<void>(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, captured.processId));
            } else if (state.terminationRequested && !state.forceTerminated &&
                       now - state.terminationStarted >= request.gracefulTermination) {
                TerminateJobObject(captured.job.value, 1);
                state.forceTerminated = true;
                state.forcedAt = now;
            }
        }

        /** @brief Drains final pipe bytes only after the direct child exits, then checks the owned Job and pipes. */
        [[nodiscard]] bool CompletedAndDrained(const CapturedProcess &captured, bool &stdoutOpen, bool &stderrOpen,
                                               LineDecoder &standardOutput, LineDecoder &standardError) {
            DrainAvailable(captured.stdoutRead.value, stdoutOpen, standardOutput);
            DrainAvailable(captured.stderrRead.value, stderrOpen, standardError);
            DWORD stdoutAvailable = 0;
            DWORD stderrAvailable = 0;
            const bool stdoutEmpty =
                !PeekNamedPipe(captured.stdoutRead.value, nullptr, 0, nullptr, &stdoutAvailable, nullptr) || stdoutAvailable == 0;
            const bool stderrEmpty =
                !PeekNamedPipe(captured.stderrRead.value, nullptr, 0, nullptr, &stderrAvailable, nullptr) || stderrAvailable == 0;
            if (!JobHasActiveProcesses(captured.job.value) && ((!stdoutOpen && !stderrOpen) || (stdoutEmpty && stderrEmpty))) {
                if (stdoutEmpty && stderrEmpty) {
                    standardOutput.Finish();
                    standardError.Finish();
                }
                return true;
            }
            return false;
        }

        /** @brief Drives both output decoders and process-tree escalation without transferring handle ownership. */
        [[nodiscard]] ProcessMonitorState MonitorProcess(const ExternalProcessRequest &request, const CancellationToken &cancellation,
                                                         const CapturedProcess &captured) {
            LineDecoder standardOutput{ProcessOutputStream::StandardOutput, request.maximumLineBytes, request.maximumOutputBytes,
                                       request.onOutput};
            LineDecoder standardError{ProcessOutputStream::StandardError, request.maximumLineBytes, request.maximumOutputBytes,
                                      request.onOutput};
            bool stdoutOpen = true;
            bool stderrOpen = true;
            ProcessMonitorState state;
            for (;;) {
                DrainAvailable(captured.stdoutRead.value, stdoutOpen, standardOutput);
                DrainAvailable(captured.stderrRead.value, stderrOpen, standardError);
                const DWORD wait = WaitForSingleObject(captured.process.value, 10);
                const auto now = std::chrono::steady_clock::now();
                const bool jobActive = JobHasActiveProcesses(captured.job.value);
                UpdateTermination(request, cancellation, captured, wait, jobActive, now, state);
                if (wait == WAIT_OBJECT_0 && CompletedAndDrained(captured, stdoutOpen, stderrOpen, standardOutput, standardError))
                    break;
                if (state.forceTerminated && now - state.forcedAt >= request.maximumDrainDuration)
                    break;
            }
            standardOutput.Finish();
            standardError.Finish();
            return state;
        }

        /** @brief Verifies that the Job is empty before reporting one typed terminal process outcome. */
        [[nodiscard]] Result<ExternalProcessResult> FinalizeProcess(const CapturedProcess &captured, const ProcessMonitorState &state) {
            if (JobHasActiveProcesses(captured.job.value)) {
                if (!TerminateJobObject(captured.job.value, 1))
                    return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
                const auto stopWaiting = std::chrono::steady_clock::now() + std::chrono::seconds{1};
                while (JobHasActiveProcesses(captured.job.value) && std::chrono::steady_clock::now() < stopWaiting)
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                if (JobHasActiveProcesses(captured.job.value))
                    return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
            }
            if (WaitForSingleObject(captured.process.value, 0) != WAIT_OBJECT_0)
                return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(captured.process.value, &exitCode))
                return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
            ExternalProcessResult result;
            if (state.forceTerminated)
                result.reason = ProcessTerminationReason::Forced;
            else if (state.stopCause == ProcessStopCause::Timeout)
                result.reason = ProcessTerminationReason::TimedOut;
            else if (state.terminationRequested)
                result.reason = ProcessTerminationReason::Cancelled;
            else
                result.reason = ProcessTerminationReason::Exited;
            result.exitCode = static_cast<int>(exitCode);
            result.stopCause = state.stopCause;
            return Result<ExternalProcessResult>::Success(result);
        }
    }  // namespace

    /** @copydoc NativeExternalProcessRunner::Run */
    Result<ExternalProcessResult> NativeExternalProcessRunner::Run(const ExternalProcessRequest &request,
                                                                   const CancellationToken &cancellation) {
        if (request.executable.empty())
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, "Executable is empty."));
        auto captured = LaunchCapturedProcess(request);
        if (captured.HasError())
            return Result<ExternalProcessResult>::Failure(captured.ErrorValue());
        const ProcessMonitorState state = MonitorProcess(request, cancellation, captured.Value());
        return FinalizeProcess(captured.Value(), state);
    }
}  // namespace Horo
