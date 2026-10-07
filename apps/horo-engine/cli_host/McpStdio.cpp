#include "CliHostCommands.h"
#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Mcp/McpErrors.h"
#include "McpServe.h"

#include <array>
#include <chrono>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <atomic>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace Horo::Application::Internal {
    namespace {
#if defined(_WIN32)
        std::atomic<bool> Interrupted{};

        BOOL WINAPI InterruptHandler(const DWORD event) {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
                return FALSE;
            Interrupted.store(true);
            return TRUE;
        }
#else
        volatile std::sig_atomic_t Interrupted{};

        /** @brief Signal-safe flag only; ordinary owner-thread code performs cancellation and teardown. */
        void InterruptHandler(int) {
            Interrupted = 1;
        }
#endif
        /** @brief Owns native stdio/signal changes only for this synchronous process invocation. */
        class Stdio final {
        public:
            Result<void> Start() {
                Interrupted = false;
#if defined(_WIN32)
                input_ = GetStdHandle(STD_INPUT_HANDLE);
                output_ = GetStdHandle(STD_OUTPUT_HANDLE);
                if (!SetConsoleCtrlHandler(InterruptHandler, TRUE))
                    return Failure();
                signals_ = true;
                // A protocol service accepts redirected input, never an implicit interactive console.
                if (GetFileType(input_) != FILE_TYPE_PIPE && GetFileType(input_) != FILE_TYPE_DISK)
                    return Failure();
#else
                struct sigaction action{};
                action.sa_handler = InterruptHandler;
                sigemptyset(&action.sa_mask);
                if (sigaction(SIGINT, &action, &interrupt_) != 0)
                    return Failure();
                interruptInstalled_ = true;
                if (sigaction(SIGTERM, &action, &terminate_) != 0)
                    return Failure();
                terminateInstalled_ = true;
                action.sa_handler = SIG_IGN;
                if (sigaction(SIGPIPE, &action, &pipe_) != 0)
                    return Failure();
                pipeInstalled_ = true;
                inputFlags_ = fcntl(STDIN_FILENO, F_GETFL);
                outputFlags_ = fcntl(STDOUT_FILENO, F_GETFL);
                if (inputFlags_ < 0 || outputFlags_ < 0 || fcntl(STDIN_FILENO, F_SETFL, inputFlags_ | O_NONBLOCK) < 0 ||
                    fcntl(STDOUT_FILENO, F_SETFL, outputFlags_ | O_NONBLOCK) < 0)
                    return Failure();
#endif
                return Result<void>::Success();
            }

            ~Stdio() {
#if defined(_WIN32)
                if (signals_)
                    SetConsoleCtrlHandler(InterruptHandler, FALSE);
#else
                if (inputFlags_ >= 0)
                    static_cast<void>(fcntl(STDIN_FILENO, F_SETFL, inputFlags_));
                if (outputFlags_ >= 0)
                    static_cast<void>(fcntl(STDOUT_FILENO, F_SETFL, outputFlags_));
                if (pipeInstalled_)
                    static_cast<void>(sigaction(SIGPIPE, &pipe_, nullptr));
                if (terminateInstalled_)
                    static_cast<void>(sigaction(SIGTERM, &terminate_, nullptr));
                if (interruptInstalled_)
                    static_cast<void>(sigaction(SIGINT, &interrupt_, nullptr));
#endif
            }

            bool Stopped() const {
                return Interrupted != 0;
            }

            Result<McpChannelRead> Read() const {
                std::array<char, 64> bytes{};
#if defined(_WIN32)
                if (GetFileType(input_) == FILE_TYPE_PIPE) {
                    DWORD available{};
                    if (!PeekNamedPipe(input_, nullptr, 0, nullptr, &available, nullptr)) {
                        if (GetLastError() == ERROR_BROKEN_PIPE)
                            return Result<McpChannelRead>::Success({.disconnected = true});
                        return Result<McpChannelRead>::Failure(MakeError(Cli::CliErrors::HostFailure));
                    }
                    if (available == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{20});
                        return Result<McpChannelRead>::Success({});
                    }
                }
                DWORD count{};
                if (!ReadFile(input_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF)
                        return Result<McpChannelRead>::Success({.disconnected = true});
                    return Result<McpChannelRead>::Failure(MakeError(Cli::CliErrors::HostFailure));
                }
#else
                pollfd descriptor{STDIN_FILENO, POLLIN, 0};
                const int ready = poll(&descriptor, 1, 20);
                if (ready < 0 && errno != EINTR)
                    return Result<McpChannelRead>::Failure(MakeError(Cli::CliErrors::HostFailure));
                if (ready <= 0)
                    return Result<McpChannelRead>::Success({});
                const auto count = read(STDIN_FILENO, bytes.data(), bytes.size());
                if (count < 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        return Result<McpChannelRead>::Success({});
                    return Result<McpChannelRead>::Failure(MakeError(Cli::CliErrors::HostFailure));
                }
#endif
                return Result<McpChannelRead>::Success(
                    {.bytes = std::string{bytes.data(), static_cast<std::size_t>(count)}, .disconnected = count == 0});
            }

            Result<void> Write(const std::string_view bytes) const {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                std::size_t offset{};
                while (offset < bytes.size()) {
                    if (Stopped())
                        return Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                    if (std::chrono::steady_clock::now() >= deadline)
                        return Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionTimedOut));
#if defined(_WIN32)
                    // The inherited pipe is synchronous. Own and join its writer, cancelling blocked I/O on stop/deadline.
                    WriteOperation operation{output_, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset)};
                    const HANDLE writer = CreateThread(nullptr, 0, WriteOperation::Run, &operation, 0, nullptr);
                    if (writer == nullptr)
                        return Failure();
                    while (WaitForSingleObject(writer, 10) == WAIT_TIMEOUT) {
                        if (Stopped() || std::chrono::steady_clock::now() >= deadline)
                            CancelSynchronousIo(writer);
                    }
                    CloseHandle(writer);
                    const DWORD count = operation.count;
                    if (!operation.written || count == 0)
                        return Failure();
#else
                    pollfd descriptor{STDOUT_FILENO, POLLOUT, 0};
                    const int ready = poll(&descriptor, 1, 20);
                    if (ready < 0 && errno != EINTR)
                        return Failure();
                    if (ready <= 0)
                        continue;
                    const auto count = write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
                    if (count < 0 && (errno == EINTR || errno == EAGAIN))
                        continue;
                    if (count <= 0)
                        return Failure();
#endif
                    offset += static_cast<std::size_t>(count);
                }
                return Result<void>::Success();
            }

        private:
            static Result<void> Failure() {
                return Result<void>::Failure(MakeError(Cli::CliErrors::HostFailure));
            }
#if defined(_WIN32)
            /** @brief Stack storage stays alive until the owned synchronous writer has joined. */
            struct WriteOperation final {
                HANDLE output;
                const char *bytes;
                DWORD size;
                DWORD count{};
                BOOL written{};

                static DWORD WINAPI Run(void *value) {
                    auto &operation = *static_cast<WriteOperation *>(value);
                    operation.written = WriteFile(operation.output, operation.bytes, operation.size, &operation.count, nullptr);
                    return 0;
                }
            };

            HANDLE input_{};
            HANDLE output_{};
            bool signals_{};
#else
            struct sigaction interrupt_{}, terminate_{}, pipe_{};
            bool interruptInstalled_{}, terminateInstalled_{}, pipeInstalled_{};
            int inputFlags_{-1}, outputFlags_{-1};
#endif
        };

        /** @brief MCP adapter retains its application observability owner through controller drainage. */
        class ObservabilitySmokeTool final : public Mcp::IMcpToolAdapter {
        public:
            explicit ObservabilitySmokeTool(std::shared_ptr<HostObservabilitySession> session) : session_(std::move(session)) {}

            Result<nlohmann::json> Invoke(const nlohmann::json &, const Mcp::McpRequestContext &context) override {
                if (context.IsStopRequested())
                    return Result<nlohmann::json>::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                const auto emitted = EmitHostObservabilitySmoke();
                if (emitted.HasError())
                    return Result<nlohmann::json>::Failure(emitted.ErrorValue());
                return Result<nlohmann::json>::Success({{"completed", true}});
            }

        private:
            std::shared_ptr<HostObservabilitySession> session_;
        };
    }  // namespace

    /** @copydoc ServeNativeMcp */
    Result<void> ServeNativeMcp(const Cli::CliExecutionContext &context, std::shared_ptr<HostObservabilitySession> session) {
        Stdio stdio;
        const auto started = stdio.Start();
        if (started.HasError())
            return started;
        if (!session || !context.HasCapability(Cli::CliCapabilityId{"horo.observability.smoke"}))
            return Result<void>::Failure(MakeError(Cli::CliErrors::CommandUnavailable));
        const std::vector<std::string> capabilities{"horo.observability.smoke"};
        auto registry = std::make_shared<Mcp::McpToolRegistry>();
        const auto published =
            registry->Publish({{.descriptor = {.id = {"observability.smoke"},
                                               .description = "Run the application's headless observability smoke operation.",
                                               .inputSchema = {{"type", "object"}, {"additionalProperties", false}},
                                               .outputSchema = {{"type", "object"},
                                                                {"properties", {{"completed", {{"type", "boolean"}}}}},
                                                                {"required", {"completed"}},
                                                                {"additionalProperties", false}},
                                               .effect = Mcp::McpToolEffect::Mutation,
                                               .requiredCapabilities = capabilities},
                                .adapter = std::make_shared<ObservabilitySmokeTool>(std::move(session)),
                                .owner = Mcp::McpOwnerContext::Build}},
                              capabilities);
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        return ServeMcp(std::move(registry),
                        {.read =
                             [&] {
            return stdio.Read();
        },
                         .write =
                             [&](const std::string_view bytes) {
            return stdio.Write(bytes);
        },
                         .stopped =
                             [&] {
            return stdio.Stopped() || context.IsStopRequested();
        }},
                        capabilities);
    }
}  // namespace Horo::Application::Internal
