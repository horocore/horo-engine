#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/PlatformErrors.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cwchar>
#include <functional>
#include <map>
#include <optional>
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

        [[nodiscard]] Result<std::vector<wchar_t>> BuildEnvironment(const ProcessEnvironment &overlay,
                                                                    const std::optional<std::uintptr_t> probeHandle) {
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
            values.erase(L"HORO_PRODUCT_PROBE_LEASE");
            if (probeHandle)
                values[L"HORO_PRODUCT_PROBE_LEASE"] = std::to_wstring(*probeHandle);
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

        /** @brief Distinguishes a surviving descendant from a lagging accounting reference to the exited direct child. */
        [[nodiscard]] bool JobHasActiveDescendants(const HANDLE job, const DWORD directProcessId) noexcept {
            struct ProcessIds final {
                DWORD assigned{};
                DWORD count{};
                std::array<ULONG_PTR, 16> ids{};
            } processIds;

            if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &processIds, sizeof(processIds), nullptr) ||
                processIds.assigned > processIds.ids.size() || processIds.count > processIds.ids.size())
                return true;
            return std::ranges::any_of(std::span{processIds.ids}.first(processIds.count), [directProcessId](const ULONG_PTR id) {
                if (id == directProcessId)
                    return false;
                const Handle descendant{OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(id))};
                if (descendant.value == nullptr)
                    return GetLastError() != ERROR_INVALID_PARAMETER;
                return WaitForSingleObject(descendant.value, 0) != WAIT_OBJECT_0;
            });
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

        /** @brief Limits inherited handles to the standard streams and one explicit probe lease. */
        class ChildHandleAllowlist final {
        public:
            explicit ChildHandleAllowlist(std::vector<HANDLE> handles) : handles_(std::move(handles)) {
                SIZE_T bytes = 0;
                static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes));
                storage_.resize(bytes);
                list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
                if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes)) {
                    list_ = nullptr;
                    return;
                }
                valid_ = UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
                                                   handles_.size() * sizeof(HANDLE), nullptr, nullptr) != 0;
            }

            ChildHandleAllowlist(const ChildHandleAllowlist &) = delete;
            ChildHandleAllowlist &operator=(const ChildHandleAllowlist &) = delete;

            ~ChildHandleAllowlist() {
                if (list_ != nullptr)
                    DeleteProcThreadAttributeList(list_);
            }

            [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept {
                return valid_ ? list_ : nullptr;
            }

        private:
            std::vector<HANDLE> handles_;
            std::vector<std::byte> storage_;
            LPPROC_THREAD_ATTRIBUTE_LIST list_{};
            bool valid_{};
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

        /** @brief Admits the suspended child before any product code can run. */
        [[nodiscard]] Result<void> AdmitCapturedProcess(CapturedProcess &captured) {
            captured.job = Handle{CreateJobObjectW(nullptr, nullptr)};
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (captured.job.value == nullptr ||
                !SetInformationJobObject(captured.job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(captured.job.value, captured.process.value)) {
                TerminateProcess(captured.process.value, 1);
                return Result<void>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            }
            ResumeThread(captured.thread.value);
            return Result<void>::Success();
        }

        /** @brief Connects the child's three standard handles through the explicit allowlist. */
        [[nodiscard]] STARTUPINFOEXW ChildStartup(const ChildHandleAllowlist &allowlist, const HANDLE stdinRead, const HANDLE stdoutWrite,
                                                  const HANDLE stderrWrite) {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = stdinRead;
            startup.StartupInfo.hStdOutput = stdoutWrite;
            startup.StartupInfo.hStdError = stderrWrite;
            startup.lpAttributeList = allowlist.Get();
            return startup;
        }

        /** @brief Exposes only the duplicated probe handle through the child's fresh environment. */
        [[nodiscard]] Result<std::vector<wchar_t>> ChildEnvironment(const ExternalProcessRequest &request,
                                                                    const std::optional<std::uintptr_t> maintenanceHandle,
                                                                    const Handle &probeTransfer) {
            std::optional<std::uintptr_t> childProbeHandle;
            if (maintenanceHandle)
                childProbeHandle = reinterpret_cast<std::uintptr_t>(probeTransfer.value);
            return BuildEnvironment(request.environment, childProbeHandle);
        }

        /** @brief Lists exactly the handles admitted to the child. */
        [[nodiscard]] std::vector<HANDLE> ChildHandles(const Handle &stdinRead, const Handle &stdoutWrite, const Handle &stderrWrite,
                                                       const Handle &probeTransfer, const bool includeProbe) {
            std::vector<HANDLE> inherited{stdinRead.value, stdoutWrite.value, stderrWrite.value};
            if (includeProbe)
                inherited.push_back(probeTransfer.value);
            return inherited;
        }

        /** @brief Creates a suspended child and admits it to a kill-on-close Job before resuming. */
        [[nodiscard]] Result<CapturedProcess> LaunchCapturedProcess(const ExternalProcessRequest &request,
                                                                    const std::optional<std::uintptr_t> maintenanceHandle) {
            CapturedProcess captured;
            Handle stdoutWrite;
            Handle stderrWrite;
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            Handle stdinRead{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (stdinRead.value == INVALID_HANDLE_VALUE)
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
            if (!CreatePipe(&captured.stdoutRead.value, &stdoutWrite.value, &security, 0) ||
                !CreatePipe(&captured.stderrRead.value, &stderrWrite.value, &security, 0) ||
                !SetHandleInformation(captured.stdoutRead.value, HANDLE_FLAG_INHERIT, 0) ||
                !SetHandleInformation(captured.stderrRead.value, HANDLE_FLAG_INHERIT, 0))
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessIoFailed));

            Handle probeTransfer;
            if (maintenanceHandle && !DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(*maintenanceHandle),
                                                      GetCurrentProcess(), &probeTransfer.value, 0, TRUE, DUPLICATE_SAME_ACCESS))
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessIoFailed));

            auto commandLine = BuildCommandLine(request);
            if (commandLine.HasError())
                return Result<CapturedProcess>::Failure(commandLine.ErrorValue());
            std::wstring commandLineBuffer = std::move(commandLine).Value();
            auto environment = ChildEnvironment(request, maintenanceHandle, probeTransfer);
            if (environment.HasError())
                return Result<CapturedProcess>::Failure(environment.ErrorValue());
            std::vector<wchar_t> environmentBlock = std::move(environment).Value();
            const std::wstring workingDirectory = request.workingDirectory.native();
            ChildHandleAllowlist allowlist{ChildHandles(stdinRead, stdoutWrite, stderrWrite, probeTransfer, maintenanceHandle.has_value())};
            if (allowlist.Get() == nullptr)
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
            STARTUPINFOEXW startup = ChildStartup(allowlist, stdinRead.value, stdoutWrite.value, stderrWrite.value);
            PROCESS_INFORMATION process{};
            const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
            if (!CreateProcessW(nullptr, commandLineBuffer.data(), nullptr, nullptr, TRUE, flags, environmentBlock.data(),
                                workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup.StartupInfo, &process))
                return Result<CapturedProcess>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            captured.process = Handle{process.hProcess};
            captured.thread = Handle{process.hThread};
            captured.processId = process.dwProcessId;
            stdoutWrite = {};
            stderrWrite = {};

            auto admitted = AdmitCapturedProcess(captured);
            if (admitted.HasError())
                return Result<CapturedProcess>::Failure(admitted.ErrorValue());
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
                               const CapturedProcess &captured, const DWORD wait, const bool descendantsActive,
                               const std::chrono::steady_clock::time_point now, ProcessMonitorState &state) {
            if (request.forceCancellation.IsCancellationRequested() && !state.forceTerminated) {
                state.stopCause = state.stopCause == ProcessStopCause::None ? ProcessStopCause::Cancellation : state.stopCause;
                state.terminationRequested = true;
                TerminateJobObject(captured.job.value, 1);
                state.forceTerminated = true;
                state.forcedAt = now;
            } else if (const bool cancelled = cancellation.IsCancellationRequested();
                       !state.terminationRequested &&
                       (cancelled || now - state.started >= request.timeout || (wait == WAIT_OBJECT_0 && descendantsActive))) {
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
            if (!JobHasActiveDescendants(captured.job.value, captured.processId) &&
                ((!stdoutOpen && !stderrOpen) || (stdoutEmpty && stderrEmpty))) {
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
                const bool descendantsActive = wait == WAIT_OBJECT_0 && JobHasActiveDescendants(captured.job.value, captured.processId);
                UpdateTermination(request, cancellation, captured, wait, descendantsActive, now, state);
                if (wait == WAIT_OBJECT_0 && CompletedAndDrained(captured, stdoutOpen, stderrOpen, standardOutput, standardError))
                    break;
                if (state.forceTerminated && now - state.forcedAt >= request.maximumDrainDuration)
                    break;
            }
            standardOutput.Finish();
            standardError.Finish();
            return state;
        }

        /** @brief Verifies that no owned descendant survives before reporting a typed terminal process outcome. */
        [[nodiscard]] Result<ExternalProcessResult> FinalizeProcess(const CapturedProcess &captured, const ProcessMonitorState &state) {
            if (JobHasActiveDescendants(captured.job.value, captured.processId)) {
                if (!TerminateJobObject(captured.job.value, 1))
                    return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
                const auto stopWaiting = std::chrono::steady_clock::now() + std::chrono::seconds{1};
                while (JobHasActiveDescendants(captured.job.value, captured.processId) && std::chrono::steady_clock::now() < stopWaiting)
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                if (JobHasActiveDescendants(captured.job.value, captured.processId))
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
        std::optional<std::uintptr_t> maintenanceHandle;
        if (request.maintenanceLease != nullptr) {
            if (!request.maintenanceLease->IsMaintenance())
                return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            maintenanceHandle = request.maintenanceLease->NativeHandle();
        }
        auto captured = LaunchCapturedProcess(request, maintenanceHandle);
        if (captured.HasError())
            return Result<ExternalProcessResult>::Failure(captured.ErrorValue());
        const ProcessMonitorState state = MonitorProcess(request, cancellation, captured.Value());
        return FinalizeProcess(captured.Value(), state);
    }
}  // namespace Horo
