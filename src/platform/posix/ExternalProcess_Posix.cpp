#if defined(__GLIBC__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "../ExternalProcessBytePump.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <optional>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;  // NOSONAR(cpp:S5421) environ is provided by the POSIX runtime.

namespace Horo {
    namespace {
        [[nodiscard]] std::vector<std::string> BuildEnvironment(const ProcessEnvironment &overlay, const bool probeLease) {
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
            values.erase("HORO_PRODUCT_PROBE_LEASE");
            if (probeLease)
                values["HORO_PRODUCT_PROBE_LEASE"] = "3";
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const auto &[name, value] : values)
                result.push_back(name + "=" + value);
            return result;
        }

        /** @brief Reads exactly one explicitly sized destination span from a pipe. */
        [[nodiscard]] ssize_t ReadPipeChunk(const int descriptor, const std::span<char> destination) {
            iovec buffer{destination.data(), destination.size()};
            return readv(descriptor, &buffer, 1);
        }

        enum class PipeDrainState {
            Closed,
            WriterPending,
            Retry,
            Rejected
        };

        /** @brief Distinguishes an open writer from a finite unread tail or interrupted read. */
        PipeDrainState Drain(const int descriptor, bool &open, PlatformInternal::ProcessOutputCapture &decoder) {
            using enum PipeDrainState;
            std::array<char, 4096> buffer{};
            for (std::size_t reads = 0; reads < 16; ++reads) {
                const ssize_t count = ReadPipeChunk(descriptor, buffer);
                if (count > 0) {
                    const auto bytes = std::span<const char>{buffer.data(), static_cast<std::size_t>(count)};
                    if (!decoder.Append(bytes))
                        return Rejected;
                    continue;
                }
                if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    open = false;
                    decoder.Finish();
                    return Closed;
                }
                return errno == EINTR ? Retry : WriterPending;
            }
            return Retry;
        }

        /** @brief Observes terminal leader status without reaping, keeping its PID reserved for group cleanup. */
        [[nodiscard]] bool LeaderExited(const pid_t process) {
            siginfo_t information{};
            return waitid(P_PID, static_cast<id_t>(process), &information, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                   information.si_pid == process;
        }

        /** @brief Adds the sole maintenance descriptor to the child file actions. */
        void AddProbeFileAction(posix_spawn_file_actions_t &actions, const int probeSource) {
            if (probeSource < 0)
                return;
            posix_spawn_file_actions_adddup2(&actions, probeSource, 3);
            posix_spawn_file_actions_addclose(&actions, probeSource);
        }

        /** @brief Owns raw stdin endpoints and prevents SIGPIPE on the worker without changing process signal policy. */
        class ProcessInputSocket final {
        public:
            ProcessInputSocket() = default;
            ProcessInputSocket(const ProcessInputSocket &) = delete;
            ProcessInputSocket &operator=(const ProcessInputSocket &) = delete;

            ~ProcessInputSocket() {
                for (const int endpoint : endpoints_)
                    if (endpoint >= 0)
                        close(endpoint);
            }

            /** @brief Creates a close-on-exec stream with a blocking child and nonblocking parent endpoint. */
            [[nodiscard]] bool Open() noexcept {
#ifdef SOCK_CLOEXEC
                constexpr int socketType = SOCK_STREAM | SOCK_CLOEXEC;
#else
                constexpr int socketType = SOCK_STREAM;
#endif
                if (socketpair(AF_UNIX, socketType, 0, endpoints_.data()) != 0)
                    return false;
                for (int &endpoint : endpoints_) {
                    const int reserved = fcntl(endpoint, F_DUPFD_CLOEXEC, 4);
                    if (reserved < 0)
                        return false;
                    close(endpoint);
                    endpoint = reserved;
                }
#ifdef SO_NOSIGPIPE
                const int suppress = 1;
                if (setsockopt(endpoints_[0], SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress)) != 0)
                    return false;
#endif
                return fcntl(endpoints_[0], F_SETFL, O_NONBLOCK) == 0;
            }

            /** @brief Redirects only explicit child stdin and leaves other process modes unchanged. */
            [[nodiscard]] int Stage(posix_spawn_file_actions_t &actions) const noexcept {
                if (endpoints_[1] < 0)
                    return 0;
                return posix_spawn_file_actions_adddup2(&actions, endpoints_[1], STDIN_FILENO);
            }

            /** @brief Drops the parent's extra reference to the child's endpoint after spawn. */
            void ReleaseChild() noexcept {
                if (endpoints_[1] >= 0)
                    close(std::exchange(endpoints_[1], -1));
            }

            /** @brief Writes a bounded chunk and retains unwritten bytes for the next pump step. */
            [[nodiscard]] bool Write(PlatformInternal::ProcessInputPump &input) const noexcept {
                if (!input.Refill())
                    return false;
                const auto bytes = input.Pending();
                if (bytes.empty())
                    return true;
#ifdef MSG_NOSIGNAL
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                const auto sent = send(endpoints_[0], bytes.data(), bytes.size(), flags);
                if (sent < 0)
                    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
                input.Written(static_cast<std::size_t>(sent));
                return true;
            }

        private:
            std::array<int, 2> endpoints_{-1, -1};
        };

        /** @brief Keeps argv pointers backed by owned storage until posix_spawnp returns. */
        void BuildSpawnArguments(const ExternalProcessRequest &request, std::vector<std::string> &storage, std::vector<char *> &arguments) {
            storage.reserve(request.arguments.size() + 1U);
            storage.push_back(request.executable);
            storage.insert(storage.end(), request.arguments.begin(), request.arguments.end());
            for (std::string &argument : storage)
                arguments.push_back(argument.data());
            arguments.push_back(nullptr);
        }

        /** @brief Owns partially initialized spawn attributes through argument preparation and launch failures. */
        class SpawnAttributes final {
        public:
            SpawnAttributes() = default;

            ~SpawnAttributes() {
                if (initialization_ == 0)
                    posix_spawnattr_destroy(&attributes_);
            }

            SpawnAttributes(const SpawnAttributes &) = delete;
            SpawnAttributes &operator=(const SpawnAttributes &) = delete;

            /** @brief Establishes child-owned control signals independently of the host's presentation policy.
             * @return Zero or the native setup error; a failure must prevent spawning.
             */
            [[nodiscard]] int Configure() noexcept {
                if (initialization_ != 0)
                    return initialization_;
                sigset_t defaults{};
                sigset_t unblocked{};
                if (sigemptyset(&defaults) != 0 || sigemptyset(&unblocked) != 0)
                    return errno;
                for (const int controlSignal : {SIGINT, SIGTERM, SIGPIPE})
                    if (sigaddset(&defaults, controlSignal) != 0)
                        return errno;
                int status = posix_spawnattr_setflags(&attributes_, static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                                                                       POSIX_SPAWN_SETSIGDEF));
                if (status == 0)
                    status = posix_spawnattr_setpgroup(&attributes_, 0);
                if (status == 0)
                    status = posix_spawnattr_setsigmask(&attributes_, &unblocked);
                if (status == 0)
                    status = posix_spawnattr_setsigdefault(&attributes_, &defaults);
                return status;
            }

            /** @brief Borrows the initialized native attributes for the synchronous spawn call. */
            [[nodiscard]] const posix_spawnattr_t *Get() const noexcept {
                return &attributes_;
            }

        private:
            posix_spawnattr_t attributes_{};
            const int initialization_{posix_spawnattr_init(&attributes_)};
        };

        Result<pid_t> SpawnProcess(const ExternalProcessRequest &request, const std::array<int, 2> &stdoutPipe,
                                   const std::array<int, 2> &stderrPipe, const ProcessInputSocket &input,
                                   const std::optional<int> maintenanceDescriptor) {
            // Finish allocating borrowed argv/environment storage before acquiring native staging resources.
            std::vector<std::string> argumentStorage;
            std::vector<char *> arguments;
            BuildSpawnArguments(request, argumentStorage, arguments);
            std::vector<std::string> environmentStorage = BuildEnvironment(request.environment, maintenanceDescriptor.has_value());
            std::vector<char *> environment;
            for (std::string &entry : environmentStorage)
                environment.push_back(entry.data());
            environment.push_back(nullptr);

            const int probeSource = maintenanceDescriptor.has_value() ? fcntl(*maintenanceDescriptor, F_DUPFD_CLOEXEC, 64) : -1;
            if (maintenanceDescriptor.has_value() && probeSource < 0)
                return Result<pid_t>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            posix_spawn_file_actions_t actions;
            if (const int initialized = posix_spawn_file_actions_init(&actions); initialized != 0) {
                if (probeSource >= 0)
                    close(probeSource);
                return Result<pid_t>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, std::strerror(initialized)));
            }
            posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
            posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
            posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
            posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);
            AddProbeFileAction(actions, probeSource);
#if defined(__APPLE__) || defined(__GLIBC__)
            if (!request.workingDirectory.empty())
                posix_spawn_file_actions_addchdir_np(&actions, request.workingDirectory.c_str());
#else
            if (!request.workingDirectory.empty()) {
                posix_spawn_file_actions_destroy(&actions);
                if (probeSource >= 0)
                    close(probeSource);
                return Result<pid_t>::Failure(
                    MakeError(PlatformErrors::ProcessLaunchFailed, "Working directories are unavailable on this POSIX host."));
            }
#endif

            SpawnAttributes attributes;
            const int inputConfigured = input.Stage(actions);
            const int configured = inputConfigured == 0 ? attributes.Configure() : inputConfigured;

            pid_t process{};
            const int spawned = configured == 0 ? posix_spawnp(&process, request.executable.c_str(), &actions, attributes.Get(),
                                                               arguments.data(), environment.data())
                                                : configured;
            posix_spawn_file_actions_destroy(&actions);
            if (probeSource >= 0)
                close(probeSource);
            if (spawned != 0)
                return Result<pid_t>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed, std::strerror(spawned)));
            return Result<pid_t>::Success(process);
        }

        [[nodiscard]] Result<pid_t> SpawnCapturedProcess(const ExternalProcessRequest &request, std::array<int, 2> &stdoutPipe,
                                                         std::array<int, 2> &stderrPipe, ProcessInputSocket &input,
                                                         const std::optional<int> maintenanceDescriptor) {
            if (pipe(stdoutPipe.data()) != 0 || pipe(stderrPipe.data()) != 0) {
                const int failure = errno;
                for (const int descriptor : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]})
                    if (descriptor >= 0)
                        close(descriptor);
                return Result<pid_t>::Failure(MakeError(PlatformErrors::ProcessIoFailed, std::strerror(failure)));
            }

            auto spawned = SpawnProcess(request, stdoutPipe, stderrPipe, input, maintenanceDescriptor);
            input.ReleaseChild();
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
                                      ProcessMonitorState &monitor, const bool descendantWriter) {
            using enum ProcessStopCause;
            const auto now = std::chrono::steady_clock::now();
            if (request.forceCancellation.IsCancellationRequested() && !monitor.forced) {
                monitor.cause = monitor.cause == None ? Cancellation : monitor.cause;
                monitor.terminationRequested = true;
                static_cast<void>(kill(-process, SIGKILL));
                monitor.forced = true;
                monitor.forcedAt = now;
                return;
            }
            if (const bool cancelled = cancellation.IsCancellationRequested();
                !monitor.terminationRequested && (cancelled || now - monitor.started >= request.timeout || descendantWriter)) {
                monitor.terminationRequested = true;
                if (cancelled)
                    monitor.cause = Cancellation;
                else if (descendantWriter)
                    monitor.cause = DescendantCleanup;
                else
                    monitor.cause = Timeout;
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

        std::optional<int> maintenanceDescriptor;
        if (request.maintenanceLease != nullptr) {
            if (!request.maintenanceLease->IsMaintenance())
                return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessLaunchFailed));
            maintenanceDescriptor = static_cast<int>(request.maintenanceLease->NativeHandle());
        }

        std::array<int, 2> stdoutPipe{-1, -1};
        std::array<int, 2> stderrPipe{-1, -1};
        ProcessInputSocket inputSocket;
        if (request.byteChannel && !inputSocket.Open())
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed, std::strerror(errno)));
        auto spawnedResult = SpawnCapturedProcess(request, stdoutPipe, stderrPipe, inputSocket, maintenanceDescriptor);
        if (spawnedResult.HasError()) {
            return Result<ExternalProcessResult>::Failure(spawnedResult.ErrorValue());
        }
        const pid_t process = spawnedResult.Value();

        PlatformInternal::ProcessOutputCapture standardOutput{ProcessOutputStream::StandardOutput, request};
        PlatformInternal::ProcessOutputCapture standardError{ProcessOutputStream::StandardError, request};
        bool stdoutOpen = true;
        bool stderrOpen = true;
        bool childExited = false;
        bool ioFailed = false;
        PlatformInternal::ProcessInputPump input{request.byteChannel};
        int status{};
        const auto started = std::chrono::steady_clock::now();
        ProcessMonitorState monitor{
            .started = started,
            .terminationStarted = started,
        };

        while (!childExited || stdoutOpen || stderrOpen) {
            using enum PipeDrainState;
            std::array<pollfd, 2> descriptors{{{stdoutPipe[0], static_cast<short>(stdoutOpen ? POLLIN : 0), 0},
                                               {stderrPipe[0], static_cast<short>(stderrOpen ? POLLIN : 0), 0}}};
            static_cast<void>(poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 25));
            childExited = childExited || LeaderExited(process);
            // Observe exit before reading: EAGAIN then proves a surviving pipe writer,
            // whereas pending bytes, EOF and the reserved zombie leader do not.
            const auto stdoutState = stdoutOpen ? Drain(stdoutPipe[0], stdoutOpen, standardOutput) : Closed;
            const auto stderrState = stderrOpen ? Drain(stderrPipe[0], stderrOpen, standardError) : Closed;
            const bool writing = !childExited && !monitor.terminationRequested && !cancellation.IsCancellationRequested();
            if (stdoutState == Rejected || (writing && !inputSocket.Write(input))) {
                ioFailed = true;
                break;
            }
            const bool complete = childExited && !stdoutOpen && !stderrOpen;
            const bool descendantWriter = childExited && (stdoutState == WriterPending || stderrState == WriterPending);
            if (!complete)
                UpdateProcessTermination(process, request, cancellation, monitor, descendantWriter);
            if (complete || (monitor.forced && std::chrono::steady_clock::now() - monitor.forcedAt >= request.maximumDrainDuration))
                break;
        }
        // Keep the unreaped leader PID reserved until the whole group has received its final kill.
        static_cast<void>(kill(-process, SIGKILL));
        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        standardOutput.Finish();
        standardError.Finish();
        while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
        }

        if (ioFailed)
            return Result<ExternalProcessResult>::Failure(MakeError(PlatformErrors::ProcessIoFailed));
        return Result<ExternalProcessResult>::Success(BuildExternalProcessResult(status, monitor));
    }

}  // namespace Horo
