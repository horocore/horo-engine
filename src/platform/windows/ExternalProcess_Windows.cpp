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
    }  // namespace

    /** @copydoc NativeExternalProcessRunner::Run */
    Result<ExternalProcessResult> NativeExternalProcessRunner::Run(const ExternalProcessRequest &request,
                                                                   const CancellationToken &cancellation) {
        if (request.executable.empty())
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, "Executable is empty."));

        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle stdoutRead;
        Handle stdoutWrite;
        Handle stderrRead;
        Handle stderrWrite;
        if (!CreatePipe(&stdoutRead.value, &stdoutWrite.value, &security, 0) ||
            !CreatePipe(&stderrRead.value, &stderrWrite.value, &security, 0) ||
            !SetHandleInformation(stdoutRead.value, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stderrRead.value, HANDLE_FLAG_INHERIT, 0))
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));

        Result<std::wstring> executable = ToWide(request.executable);
        if (executable.HasError())
            return Result<ExternalProcessResult>::Failure(executable.ErrorValue());
        std::wstring commandLine = QuoteArgument(executable.Value());
        for (const std::string &argument : request.arguments) {
            Result<std::wstring> wide = ToWide(argument);
            if (wide.HasError())
                return Result<ExternalProcessResult>::Failure(wide.ErrorValue());
            commandLine.push_back(L' ');
            commandLine.append(QuoteArgument(wide.Value()));
        }
        Result<std::vector<wchar_t>> environment = BuildEnvironment(request.environment);
        if (environment.HasError())
            return Result<ExternalProcessResult>::Failure(environment.ErrorValue());

        std::vector<wchar_t> environmentBlock = std::move(environment).Value();
        const std::wstring workingDirectory = request.workingDirectory.native();

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = stdoutWrite.value;
        startup.hStdError = stderrWrite.value;
        PROCESS_INFORMATION process{};
        if (const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
            !CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, flags, environmentBlock.data(),
                            workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process))
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
        Handle processHandle{process.hProcess};
        Handle threadHandle{process.hThread};
        stdoutWrite = {};
        stderrWrite = {};

        Handle job{CreateJobObjectW(nullptr, nullptr)};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job.value == nullptr || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job.value, processHandle.value)) {
            TerminateProcess(processHandle.value, 1);
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
        }
        ResumeThread(threadHandle.value);

        LineDecoder standardOutput{ProcessOutputStream::StandardOutput, request.maximumLineBytes, request.maximumOutputBytes,
                                   request.onOutput};
        LineDecoder standardError{ProcessOutputStream::StandardError, request.maximumLineBytes, request.maximumOutputBytes,
                                  request.onOutput};
        bool stdoutOpen = true;
        bool stderrOpen = true;
        bool terminationRequested = false;
        bool forceTerminated = false;
        ProcessStopCause stopCause{ProcessStopCause::None};
        const auto started = std::chrono::steady_clock::now();
        auto terminationStarted = started;
        auto forcedAt = started;
        for (;;) {
            DrainAvailable(stdoutRead.value, stdoutOpen, standardOutput);
            DrainAvailable(stderrRead.value, stderrOpen, standardError);
            const DWORD wait = WaitForSingleObject(processHandle.value, 10);
            const auto now = std::chrono::steady_clock::now();
            const bool jobActive = JobHasActiveProcesses(job.value);
            if (request.forceCancellation.IsCancellationRequested() && !forceTerminated) {
                stopCause = stopCause == ProcessStopCause::None ? ProcessStopCause::Cancellation : stopCause;
                terminationRequested = true;
                TerminateJobObject(job.value, 1);
                forceTerminated = true;
                forcedAt = now;
            } else if (const bool cancelled = cancellation.IsCancellationRequested();
                       !terminationRequested && (cancelled || now - started >= request.timeout || (wait == WAIT_OBJECT_0 && jobActive))) {
                terminationRequested = true;
                stopCause = cancelled ? ProcessStopCause::Cancellation
                                      : (wait == WAIT_OBJECT_0 ? ProcessStopCause::DescendantCleanup : ProcessStopCause::Timeout);
                terminationStarted = now;
                static_cast<void>(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId));
            } else if (terminationRequested && !forceTerminated && now - terminationStarted >= request.gracefulTermination) {
                TerminateJobObject(job.value, 1);
                forceTerminated = true;
                forcedAt = now;
            }
            if (wait == WAIT_OBJECT_0) {
                DrainAvailable(stdoutRead.value, stdoutOpen, standardOutput);
                DrainAvailable(stderrRead.value, stderrOpen, standardError);
                DWORD stdoutAvailable = 0;
                DWORD stderrAvailable = 0;
                const bool stdoutEmpty =
                    !PeekNamedPipe(stdoutRead.value, nullptr, 0, nullptr, &stdoutAvailable, nullptr) || stdoutAvailable == 0;
                const bool stderrEmpty =
                    !PeekNamedPipe(stderrRead.value, nullptr, 0, nullptr, &stderrAvailable, nullptr) || stderrAvailable == 0;
                if (!JobHasActiveProcesses(job.value) && ((!stdoutOpen && !stderrOpen) || (stdoutEmpty && stderrEmpty))) {
                    if (stdoutEmpty && stderrEmpty) {
                        standardOutput.Finish();
                        standardError.Finish();
                    }
                    break;
                }
            }
            if (forceTerminated && now - forcedAt >= request.maximumDrainDuration)
                break;
        }
        standardOutput.Finish();
        standardError.Finish();
        if (JobHasActiveProcesses(job.value)) {
            if (!TerminateJobObject(job.value, 1))
                return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
            const auto stopWaiting = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            while (JobHasActiveProcesses(job.value) && std::chrono::steady_clock::now() < stopWaiting)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            if (JobHasActiveProcesses(job.value))
                return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
        }
        if (WaitForSingleObject(processHandle.value, 0) != WAIT_OBJECT_0)
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processHandle.value, &exitCode))
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
        ExternalProcessResult result;
        if (forceTerminated) {
            result.reason = ProcessTerminationReason::Forced;
        } else if (stopCause == ProcessStopCause::Timeout) {
            result.reason = ProcessTerminationReason::TimedOut;
        } else if (terminationRequested) {
            result.reason = ProcessTerminationReason::Cancelled;
        } else {
            result.reason = ProcessTerminationReason::Exited;
        }
        result.exitCode = static_cast<int>(exitCode);
        result.stopCause = stopCause;
        return Result<ExternalProcessResult>::Success(result);
    }
}  // namespace Horo
