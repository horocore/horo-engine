#if defined(__GLIBC__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;  // NOSONAR(cpp:S5421) environ is provided by the POSIX runtime.

namespace Horo {
    namespace {
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

        [[nodiscard]] std::vector<std::string> BuildEnvironment(const ProcessEnvironment &overlay) {
            std::map<std::string, std::string, std::less<>> values;
            if (overlay.base == ProcessEnvironmentBase::InheritWithOverrides) {
                for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
                    const std::string_view text{*entry};
                    const std::size_t separator = text.find('=');
                    if (separator != std::string_view::npos)
                        values.try_emplace(std::string{text.substr(0, separator)}, text.substr(separator + 1));
                }
            }
            for (const std::string &name : overlay.unset)
                values.erase(name);
            for (const ProcessEnvironmentAssignment &assignment : overlay.set)
                values[assignment.name] = assignment.value;
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const auto &[name, value] : values)
                result.push_back(name + "=" + value);
            return result;
        }

        void Drain(const int descriptor, bool &open, LineDecoder &decoder) {
            std::array<char, 4096> buffer{};
            for (std::size_t reads = 0; reads < 16; ++reads) {
                const ssize_t count = read(descriptor, buffer.data(), buffer.size());
                if (count > 0) {
                    decoder.Append(std::span<const char>{buffer.data(), static_cast<std::size_t>(count)});
                    continue;
                }
                if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    open = false;
                    decoder.Finish();
                }
                return;
            }
        }

        Result<pid_t> SpawnProcess(const ExternalProcessRequest &request, const std::array<int, 2> &stdoutPipe,
                                   const std::array<int, 2> &stderrPipe) {
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
            posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
            posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
            posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);
#if defined(__APPLE__) || defined(__GLIBC__)
            if (!request.workingDirectory.empty())
                posix_spawn_file_actions_addchdir_np(&actions, request.workingDirectory.c_str());
#else
            if (!request.workingDirectory.empty()) {
                posix_spawn_file_actions_destroy(&actions);
                return Result<pid_t>::Failure(
                    MakeError(PlatformErrors::ProcessLaunchFailed, "Working directories are unavailable on this POSIX host."));
            }
#endif

            posix_spawnattr_t attributes;
            posix_spawnattr_init(&attributes);
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
            posix_spawnattr_setpgroup(&attributes, 0);

            std::vector<std::string> argumentStorage;
            argumentStorage.reserve(request.arguments.size() + 1U);
            argumentStorage.push_back(request.executable);
            argumentStorage.insert(argumentStorage.end(), request.arguments.begin(), request.arguments.end());
            std::vector<char *> arguments;
            for (std::string &argument : argumentStorage)
                arguments.push_back(argument.data());
            arguments.push_back(nullptr);

            std::vector<std::string> environmentStorage = BuildEnvironment(request.environment);
            std::vector<char *> environment;
            for (std::string &entry : environmentStorage)
                environment.push_back(entry.data());
            environment.push_back(nullptr);

            pid_t process{};
            const int spawned =
                posix_spawnp(&process, request.executable.c_str(), &actions, &attributes, arguments.data(), environment.data());
            posix_spawnattr_destroy(&attributes);
            posix_spawn_file_actions_destroy(&actions);
            if (spawned != 0)
                return Result<pid_t>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, std::strerror(spawned)));
            return Result<pid_t>::Success(process);
        }

        [[nodiscard]] Result<pid_t> SpawnCapturedProcess(const ExternalProcessRequest &request, std::array<int, 2> &stdoutPipe,
                                                         std::array<int, 2> &stderrPipe) {
            if (pipe(stdoutPipe.data()) != 0 || pipe(stderrPipe.data()) != 0) {
                const int failure = errno;
                for (const int descriptor : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]})
                    if (descriptor >= 0)
                        close(descriptor);
                return Result<pid_t>::Failure(MakeError(PlatformErrors::ProcessIoFailed, std::strerror(failure)));
            }

            auto spawned = SpawnProcess(request, stdoutPipe, stderrPipe);
            close(stdoutPipe[1]);
            close(stderrPipe[1]);
            if (spawned.HasError()) {
                close(stdoutPipe[0]);
                close(stderrPipe[0]);
                return spawned;
            }

            static_cast<void>(fcntl(stdoutPipe[0], F_SETFL, fcntl(stdoutPipe[0], F_GETFL) | O_NONBLOCK));
            static_cast<void>(fcntl(stderrPipe[0], F_SETFL, fcntl(stderrPipe[0], F_GETFL) | O_NONBLOCK));
            return spawned;
        }

        struct ProcessMonitorState {
            std::chrono::steady_clock::time_point started;
            std::chrono::steady_clock::time_point terminationStarted;
            bool terminationRequested = false;
            ProcessStopCause cause{ProcessStopCause::None};
            bool forced = false;
            std::chrono::steady_clock::time_point forcedAt;
        };

        void UpdateProcessTermination(const pid_t process, const ExternalProcessRequest &request, const CancellationToken &cancellation,
                                      ProcessMonitorState &monitor, const bool childExited) {
            const auto now = std::chrono::steady_clock::now();
            if (request.forceCancellation.IsCancellationRequested() && !monitor.forced) {
                monitor.cause = monitor.cause == ProcessStopCause::None ? ProcessStopCause::Cancellation : monitor.cause;
                monitor.terminationRequested = true;
                static_cast<void>(kill(-process, SIGKILL));
                monitor.forced = true;
                monitor.forcedAt = now;
                return;
            }
            if (const bool cancelled = cancellation.IsCancellationRequested();
                !monitor.terminationRequested && (cancelled || now - monitor.started >= request.timeout || childExited)) {
                monitor.terminationRequested = true;
                monitor.cause = cancelled ? ProcessStopCause::Cancellation
                                          : (childExited ? ProcessStopCause::DescendantCleanup : ProcessStopCause::Timeout);
                monitor.terminationStarted = now;
                static_cast<void>(kill(-process, SIGTERM));
            } else if (monitor.terminationRequested && !monitor.forced && now - monitor.terminationStarted >= request.gracefulTermination) {
                static_cast<void>(kill(-process, SIGKILL));
                monitor.forced = true;
                monitor.forcedAt = now;
            }
        }

        [[nodiscard]] ExternalProcessResult BuildExternalProcessResult(const int status, const ProcessMonitorState &monitor) {
            using enum ProcessTerminationReason;
            ExternalProcessResult result;
            if (monitor.forced)
                result.reason = Forced;
            else if (monitor.cause == ProcessStopCause::Timeout)
                result.reason = TimedOut;
            else if (monitor.cause == ProcessStopCause::Cancellation)
                result.reason = Cancelled;
            else if (WIFSIGNALED(status))
                result.reason = Signalled;

            result.stopCause = monitor.cause;

            if (WIFEXITED(status))
                result.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result.exitCode = 128 + WTERMSIG(status);
            else
                result.exitCode = -1;
            return result;
        }
    }  // namespace

    /** @copydoc NativeExternalProcessRunner::Run */
    Result<ExternalProcessResult> NativeExternalProcessRunner::Run(const ExternalProcessRequest &request,
                                                                   const CancellationToken &cancellation) {
        if (request.executable.empty())
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, "Executable is empty."));

        std::array<int, 2> stdoutPipe{-1, -1};
        std::array<int, 2> stderrPipe{-1, -1};
        auto spawnedResult = SpawnCapturedProcess(request, stdoutPipe, stderrPipe);
        if (spawnedResult.HasError()) {
            return Result<ExternalProcessResult>::Failure(spawnedResult.ErrorValue());
        }
        const pid_t process = spawnedResult.Value();

        LineDecoder standardOutput{ProcessOutputStream::StandardOutput, request.maximumLineBytes, request.maximumOutputBytes,
                                   request.onOutput};
        LineDecoder standardError{ProcessOutputStream::StandardError, request.maximumLineBytes, request.maximumOutputBytes,
                                  request.onOutput};
        bool stdoutOpen = true;
        bool stderrOpen = true;
        bool childExited = false;
        int status{};
        const auto started = std::chrono::steady_clock::now();
        ProcessMonitorState monitor{
            .started = started,
            .terminationStarted = started,
        };

        while (!childExited || stdoutOpen || stderrOpen) {
            UpdateProcessTermination(process, request, cancellation, monitor, childExited);

            std::array<pollfd, 2> descriptors{{{stdoutPipe[0], static_cast<short>(stdoutOpen ? POLLIN : 0), 0},
                                               {stderrPipe[0], static_cast<short>(stderrOpen ? POLLIN : 0), 0}}};
            static_cast<void>(poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 25));
            if (stdoutOpen && (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                Drain(stdoutPipe[0], stdoutOpen, standardOutput);
            if (stderrOpen && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                Drain(stderrPipe[0], stderrOpen, standardError);
            if (!childExited) {
                siginfo_t information{};
                childExited = waitid(P_PID, static_cast<id_t>(process), &information, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                              information.si_pid == process;
            }
            if (monitor.forced && std::chrono::steady_clock::now() - monitor.forcedAt >= request.maximumDrainDuration)
                break;
        }
        // Keep the unreaped leader PID reserved until the whole group has received its final kill.
        static_cast<void>(kill(-process, SIGKILL));
        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        standardOutput.Finish();
        standardError.Finish();
        static_cast<void>(waitpid(process, &status, 0));

        return Result<ExternalProcessResult>::Success(BuildExternalProcessResult(status, monitor));
    }

}  // namespace Horo
