#pragma once

/**
 * @file ExternalProcess.h
 * @brief Shell-free bounded external-process execution contract.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Horo {
    class ProductLaunchLease;
    /** @brief Selects whether a child begins with the parent environment or an empty environment. */
    enum class ProcessEnvironmentBase : std::uint8_t {
        InheritWithOverrides,
        Replace
    };

    /** @brief One explicit child-process environment assignment. */
    struct ProcessEnvironmentAssignment {
        std::string name;
        std::string value;
    };

    /** @brief Typed child environment construction policy. */
    struct ProcessEnvironment {
        ProcessEnvironmentBase base{ProcessEnvironmentBase::InheritWithOverrides};
        std::vector<ProcessEnvironmentAssignment> set;
        std::vector<std::string> unset;
    };

    /** @brief Identifies the source pipe of one streamed line. */
    enum class ProcessOutputStream : std::uint8_t {
        StandardOutput,
        StandardError
    };

    /** @brief One bounded normalized line emitted while the child runs. */
    struct ProcessOutputLine {
        ProcessOutputStream stream{ProcessOutputStream::StandardOutput};
        std::string text;
        bool truncated{false};
    };

    /** @brief Reason a child reached its terminal state. */
    enum class ProcessTerminationReason : std::uint8_t {
        Exited,
        Signalled,
        Cancelled,
        TimedOut,
        Forced
    };

    /** @brief Trigger that initiated termination before any forced escalation. */
    enum class ProcessStopCause : std::uint8_t {
        None,
        Cancellation,
        Timeout,
        DescendantCleanup
    };

    /** @brief Terminal native-process result after every pipe has been drained. */
    struct ExternalProcessResult {
        ProcessTerminationReason reason{ProcessTerminationReason::Exited};
        int exitCode{};
        ProcessStopCause stopCause{ProcessStopCause::None};
    };

    /** @brief Optional full-duplex byte exchange, called only on the process runner's worker.
     * @details Callbacks must be nonblocking. The owner supplies bounded queues and keeps the
     * capability alive until Run returns. Standard error retains the normal bounded line capture.
     */
    class IProcessByteChannel {
    public:
        virtual ~IProcessByteChannel() = default;

        /** @brief Copies available input into the runner's fixed buffer and retires those bytes.
         * @param destination Bounded writable span. @return Byte count, with zero meaning temporarily empty.
         */
        virtual std::size_t TakeInput(std::span<char> destination) noexcept = 0;

        /** @brief Accepts exact stdout bytes, including nulls and line terminators.
         * @param bytes Borrowed only until return. @return False to terminate when the receiver cannot accept them.
         */
        virtual bool AcceptOutput(std::span<const char> bytes) noexcept = 0;
    };

    /** @brief Complete shell-free request for one child process. */
    struct ExternalProcessRequest {
        std::string executable;
        std::vector<std::string> arguments;
        std::filesystem::path workingDirectory;
        ProcessEnvironment environment;
        std::chrono::milliseconds timeout{std::chrono::minutes{15}};
        std::chrono::milliseconds gracefulTermination{std::chrono::seconds{2}};
        std::size_t maximumLineBytes{16U * 1024U};
        std::function<void(ProcessOutputLine)> onOutput;
        std::size_t maximumOutputBytes{4U * 1024U * 1024U}; /**< Per-stream callback byte budget; excess is drained and discarded. */
        std::chrono::milliseconds maximumDrainDuration{std::chrono::seconds{1}}; /**< Final drain bound after forced termination. */
        CancellationToken forceCancellation;          /**< Repeated-interrupt policy; bypasses the graceful period. */
        const ProductLaunchLease *maintenanceLease{}; /**< Borrowed exclusive lease transferred only for this bounded child. */
        IProcessByteChannel *byteChannel{};           /**< Optional raw stdin/stdout transport, borrowed for the duration of Run. */
    };

    /** @brief Blocking portable execution capability intended to run on an owned worker. */
    class IExternalProcessRunner {
    public:
        virtual ~IExternalProcessRunner() = default;

        /**
         * @brief Runs one process without a shell and drains both output pipes until terminal.
         * @param request Valid process request whose callbacks execute on the calling worker.
         * @param cancellation Cooperative cancellation observed for the complete process tree.
         * @return Terminal process result or a typed launch/I/O failure.
         */
        [[nodiscard]] virtual Result<ExternalProcessResult> Run(const ExternalProcessRequest &request,
                                                                const CancellationToken &cancellation) = 0;
    };

    /** @brief Native POSIX/Windows implementation with process-tree cancellation.
     * @details POSIX children start in their own process group with an unblocked signal mask and default
     * SIGINT, SIGTERM and SIGPIPE dispositions. Host presentation/cancellation signal suppression stays in the host.
     */
    class NativeExternalProcessRunner final : public IExternalProcessRunner {
    public:
        /** @copydoc IExternalProcessRunner::Run */
        [[nodiscard]] Result<ExternalProcessResult> Run(const ExternalProcessRequest &request,
                                                        const CancellationToken &cancellation) override;
    };
}  // namespace Horo
