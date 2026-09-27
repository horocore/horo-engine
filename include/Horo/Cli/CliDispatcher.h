#pragma once

/**
 * @file CliDispatcher.h
 * @brief Capability-scoped CLI application dispatch and invocation lifetime contract.
 */

#include "Horo/Cli/CliCommandRegistry.h"
#include "Horo/Cli/CliOptionParser.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Configuration.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace Horo {
    class IOperationQuery;
    class JobSystem;
}  // namespace Horo

namespace Horo::Cli {
    /** @brief Non-zero host-issued identity for one CLI invocation. */
    struct CliInvocationId final {
        std::uint64_t value{}; /**< Process-local invocation value; zero is invalid. */

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] auto operator<=>(const CliInvocationId &) const = default;
    };

    /** @brief Non-zero application operation identity correlated with one invocation. */
    struct CliOperationId final {
        std::uint64_t value{}; /**< Application-issued operation value; zero is invalid. */

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] auto operator<=>(const CliOperationId &) const = default;
    };

    /** @brief Non-zero background-job identity correlated with one invocation. */
    struct CliJobId final {
        std::uint64_t value{}; /**< Application-issued job value; zero is invalid. */

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] auto operator<=>(const CliJobId &) const = default;
    };

    /** @brief Project identity already classified by the application as safe for diagnostics. */
    struct CliSafeProjectContext final {
        std::string identity; /**< Stable non-secret project identity, never a path or display name. */
    };

    /** @brief Correlation copied into every terminal dispatcher outcome. */
    struct CliExecutionCorrelation final {
        CliInvocationId invocation;                   /**< Required invocation identity. */
        std::optional<CliOperationId> operation;      /**< Application operation when one was accepted. */
        std::optional<CliJobId> job;                  /**< Background job when one was accepted. */
        std::optional<CliSafeProjectContext> project; /**< Safe project context, when available. */
    };

    /** @brief Bounded presentation-independent progress update from an application use case. */
    struct CliProgressEvent final {
        std::string phase;   /**< Stable non-secret phase identity. */
        float completion{};  /**< Finite normalized completion in the closed range `[0, 1]`. */
        std::string message; /**< Optional safe presentation message. */
    };

    /** @brief Polls authoritative bounded operation/job snapshots for one correlated invocation. */
    class CliProgressProjection final {
    public:
        /** @param operations Optional application operation store. @param jobs Optional scheduler job store.
         * @param correlation Immutable invocation identities selected by the application. */
        CliProgressProjection(const IOperationQuery *operations, const JobSystem *jobs, CliExecutionCorrelation correlation);
        /** @brief Returns a changed bounded event, preferring the operation record over its child job. */
        [[nodiscard]] std::optional<CliProgressEvent> Poll();

    private:
        const IOperationQuery *operations_{};
        const JobSystem *jobs_{};
        CliExecutionCorrelation correlation_;
        std::uint64_t operationRevision_{};
        std::uint64_t jobRevision_{};
        bool operationSeen_{};
    };

    /** @brief Invocation-scoped progress destination owned by the host presentation layer. */
    class ICliProgressSink {
    public:
        virtual ~ICliProgressSink() = default;

        /**
         * @brief Accepts one already-bounded progress event synchronously.
         * @param event Event valid only for the duration of this call.
         */
        virtual void Report(const CliProgressEvent &event) = 0;
    };

    /** @brief Host-level stop trigger; it does not redefine application failure semantics. */
    enum class CliStopReason : std::uint8_t {
        None,
        Interrupted,
        TimedOut,
        Shutdown,
        ParentCancelled
    };

    /** @brief Invocation-owned cooperative/forced tokens; platform signal bridges call Interrupt, never signal handlers here. */
    class CliInvocationStopController final {
    public:
        /** @brief Starts one finite deadline and optional parent cancellation chain. @param timeout Zero disables the deadline.
         * @param parent Optional host/task-group cancellation ancestry. */
        explicit CliInvocationStopController(std::chrono::milliseconds timeout, CancellationToken parent = {});
        ~CliInvocationStopController() = default;
        CliInvocationStopController(const CliInvocationStopController &) = delete;
        CliInvocationStopController &operator=(const CliInvocationStopController &) = delete;
        /** @brief First interrupt cancels cooperatively; a repeated interrupt requests forced process termination. */
        void Interrupt() noexcept;
        /** @brief Host shutdown requests cooperative cancellation without a native signal. */
        void Shutdown() noexcept;
        /** @brief Returns the invocation/task-group/process cancellation ancestry. */
        [[nodiscard]] CancellationToken Token() const noexcept;
        /** @brief Returns the repeated-interrupt escalation token for owned process requests. */
        [[nodiscard]] CancellationToken ForceToken() const noexcept;
        /** @brief Returns the first terminal stop trigger, including parent cancellation. */
        [[nodiscard]] CliStopReason Reason() const noexcept;

    private:
        void Request(CliStopReason reason) noexcept;
        CancellationToken parent_;
        CancellationSource cancellation_;
        CancellationSource force_;
        std::atomic<CliStopReason> reason_{CliStopReason::None};
        std::atomic<unsigned> interrupts_{};
        std::condition_variable_any deadlineWake_;
        std::mutex deadlineMutex_;
        std::jthread deadlineWorker_;
    };

    /** @brief One-slot coalescing progress destination; slow consumers never hold an operation worker. */
    class CliProgressMailbox final : public ICliProgressSink {
    public:
        /** @copydoc ICliProgressSink::Report */
        void Report(const CliProgressEvent &event) override;
        /** @brief Takes the most recent update, discarding superseded intermediate values. */
        [[nodiscard]] std::optional<CliProgressEvent> Take();

    private:
        std::mutex mutex_;
        std::optional<CliProgressEvent> latest_;
    };

    /** @brief Presentation-independent cadence for TTY or rate-limited non-TTY human progress. */
    class CliProgressCadence final {
    public:
        explicit CliProgressCadence(bool tty, std::chrono::milliseconds minimumInterval = std::chrono::seconds{1});
        /** @brief Accepts changed TTY progress immediately; non-TTY accepts phase changes or elapsed cadence. */
        [[nodiscard]] bool ShouldPresent(const CliProgressEvent &event, std::chrono::steady_clock::time_point now);

    private:
        bool tty_{};
        std::chrono::milliseconds minimumInterval_{};
        std::optional<std::chrono::steady_clock::time_point> last_;
        std::string phase_;
        std::string message_;
        float completion_{};
    };

    /** @brief Selects where progress records may be presented. */
    enum class CliProgressOutputMode : std::uint8_t {
        Human,
        Json,
        JsonLines
    };

    /** @brief Presentation-thread writer for coalesced progress; never called by an operation worker. */
    class CliProgressPresenter final {
    public:
        /** @param mailbox Invocation-owned coalescing source. @param output Structured stdout.
         * @param diagnostics Human stderr. @param mode Requested output mode. @param tty Whether diagnostics is a terminal. */
        CliProgressPresenter(CliProgressMailbox &mailbox, std::ostream &output, std::ostream &diagnostics, CliProgressOutputMode mode,
                             bool tty);
        /** @brief Consumes at most one pending update and writes it only when the mode and cadence permit. */
        void Pump(std::chrono::steady_clock::time_point now);

    private:
        CliProgressMailbox *mailbox_{};
        std::ostream *output_{};
        std::ostream *diagnostics_{};
        CliProgressOutputMode mode_{};
        bool tty_{};
        CliProgressCadence cadence_;
    };

    /** @brief Typed scalar emitted by a command adapter for later host presentation. */
    using CliResultValue = std::variant<bool, std::int64_t, double, std::string>;

    /** @brief One stable named field in a command result. */
    struct CliResultField final {
        std::string name;     /**< Canonical field identity from the command output schema. */
        CliResultValue value; /**< Typed result value. */
    };

    /** @brief Presentation-independent result returned by an application command adapter. */
    struct CliCommandResult final {
        std::vector<CliResultField> fields; /**< Bounded output fields in deterministic schema order. */
    };

    /** @brief Hard bounds retained by one dispatcher and enforced around every adapter call. */
    struct CliExecutionLimits final {
        std::size_t maximumAdapters{512};              /**< Maximum active command adapters. */
        std::size_t maximumProgressEvents{4096};       /**< Maximum events forwarded by one invocation. */
        std::size_t maximumProgressTextBytes{4096};    /**< Maximum phase or message bytes. */
        std::size_t maximumResultFields{1024};         /**< Maximum fields in one terminal result. */
        std::size_t maximumResultTextBytes{64 * 1024}; /**< Maximum field-name or string-value bytes. */
        std::size_t maximumProjectIdentityBytes{256};  /**< Maximum safe project identity bytes. */
    };

    /** @brief Immutable host authority used to activate a dispatcher. */
    struct CliDispatchPolicy final {
        CliHostKind activeHost{};                         /**< Exact executable composition. */
        CliSideEffectPolicy maximumSideEffects{};         /**< Highest admitted mutation category. */
        std::vector<CliCapabilityId> grantedCapabilities; /**< Complete host capability grant set. */
        CliExecutionLimits limits{};                      /**< Per-dispatcher and per-invocation bounds. */
    };

    /** @brief Explicit host-owned values used to construct one invocation scope. */
    struct CliInvocationContext final {
        CliInvocationId invocation;                       /**< Required non-zero invocation identity. */
        const ConfigurationSnapshot *configuration{};     /**< Required immutable invocation snapshot. */
        CancellationToken cancellation;                   /**< Cooperative host/signal cancellation chain. */
        CancellationToken forceCancellation;              /**< Optional repeated-interrupt process escalation. */
        const CliInvocationStopController *stopControl{}; /**< Optional owning host stop authority for exact timeout classification. */
        CliProgressMailbox *progress{};                   /**< Optional bounded coalescing progress destination. */
        std::optional<CliSafeProjectContext> project;     /**< Optional application-approved safe identity. */
        std::uint64_t timeoutMilliseconds{};              /**< Zero selects the descriptor default. */
    };

    /**
     * @brief Narrow invocation scope passed to an adapter after all authority checks succeed.
     *
     * Application use-case capabilities are constructor-injected into the concrete adapter. This
     * context deliberately exposes no service locator, mutable engine object, job system, or output writer.
     */
    class CliExecutionContext final {
    public:
        /** @brief Returns the immutable invocation configuration. @return Host-resolved snapshot. */
        [[nodiscard]] const ConfigurationSnapshot &Configuration() const noexcept;
        /** @brief Returns the immutable invocation cancellation token. @return Cooperative token. */
        [[nodiscard]] const CancellationToken &Cancellation() const noexcept;
        /** @brief Returns the owned-subprocess escalation token, without exposing native signal APIs. */
        [[nodiscard]] const CancellationToken &ForceCancellation() const noexcept;
        /** @brief Returns the time left before the descriptor deadline, rounded up to one millisecond.
         * @return No value when unlimited; zero when expired. Adapters cap injected process requests to this bound. */
        [[nodiscard]] std::optional<std::chrono::milliseconds> RemainingTimeout() const noexcept;
        /** @brief Returns exact descriptor-declared grants only. @return Invocation-scoped capability identities. */
        [[nodiscard]] std::span<const CliCapabilityId> GrantedCapabilities() const noexcept;
        /** @brief Tests one exact capability identity. @param capability Candidate identity. @return Whether it was declared and granted.
         */
        [[nodiscard]] bool HasCapability(const CliCapabilityId &capability) const noexcept;
        /** @brief Reports whether cancellation or the finite deadline has been reached. @return Whether work should stop cooperatively. */
        [[nodiscard]] bool IsStopRequested() const noexcept;
        /** @brief Reports one bounded event synchronously. @param event Safe progress event. @return Typed validation/capacity result. */
        [[nodiscard]] Result<void> ReportProgress(const CliProgressEvent &event);
        /** @brief Correlates an accepted application operation once. @param operation Non-zero operation identity. @return Success or
         * context error. */
        [[nodiscard]] Result<void> BindOperation(CliOperationId operation);
        /** @brief Correlates an accepted application job once. @param job Non-zero job identity. @return Success or context error. */
        [[nodiscard]] Result<void> BindJob(CliJobId job);
        /** @brief Returns the current immutable correlation snapshot. @return Invocation and accepted child identities. */
        [[nodiscard]] const CliExecutionCorrelation &Correlation() const noexcept;

    private:
        friend class CliDispatcher;
        /** @brief Binds a validated host invocation to an exact adapter capability view. */
        CliExecutionContext(const CliInvocationContext &invocation, std::span<const CliCapabilityId> capabilities,
                            const CliExecutionLimits &limits, std::optional<std::chrono::steady_clock::time_point> deadline) noexcept;

        const ConfigurationSnapshot *configuration_{};
        CancellationToken cancellation_;
        CancellationToken forceCancellation_;
        CliProgressMailbox *progress_{};
        std::span<const CliCapabilityId> capabilities_;
        const CliExecutionLimits *limits_{};
        std::optional<std::chrono::steady_clock::time_point> deadline_;
        CliExecutionCorrelation correlation_;
        std::size_t progressCount_{};
        bool progressRejected_{};
        bool correlationRejected_{};
    };

    /** @brief Domain-owned thin adapter over a constructor-injected application use case. */
    class ICliCommandAdapter {
    public:
        virtual ~ICliCommandAdapter() = default;
        /** @brief Returns the inert descriptor authored by this adapter's owner. @return Stable adapter-lifetime descriptor. */
        [[nodiscard]] virtual const CliCommandDescriptor &GetDescriptor() const noexcept = 0;
        /** @brief Dispatches a validated request to the shared application use case. @param request Typed request. @param context Narrow
         * invocation scope. @return Typed application result. */
        [[nodiscard]] virtual Result<CliCommandResult> Execute(const CliCommandRequest &request, CliExecutionContext &context) = 0;
    };

    /** @brief Owned adapter registration prepared by the explicit application composition root. */
    struct CliCommandAdapterRegistration final {
        std::unique_ptr<ICliCommandAdapter> adapter; /**< Sole adapter ownership transferred to the dispatcher. */
        std::vector<CliCapabilityId> capabilities;   /**< Exact narrow dependencies bound to this adapter. */
    };

    /** @brief Terminal result whose correlation survives success, cancellation, and application failure. */
    class CliTerminalResult final {
    public:
        /** @brief Creates a successful terminal envelope. @param correlation Final correlation. @param result Adapter result. @return Owned
         * envelope. */
        [[nodiscard]] static CliTerminalResult Success(CliExecutionCorrelation correlation, CliCommandResult result);
        /** @brief Creates a failed terminal envelope. @param correlation Final correlation. @param error Typed failure. @return Owned
         * envelope. */
        [[nodiscard]] static CliTerminalResult Failure(CliExecutionCorrelation correlation, Error error);
        /** @brief Returns terminal correlation. @return Immutable identity snapshot. */
        [[nodiscard]] const CliExecutionCorrelation &Correlation() const noexcept;
        /** @brief Returns the typed application outcome. @return Success payload or typed failure. */
        [[nodiscard]] const Result<CliCommandResult> &Outcome() const noexcept;

    private:
        CliTerminalResult(CliExecutionCorrelation correlation, Result<CliCommandResult> outcome);
        CliExecutionCorrelation correlation_;
        Result<CliCommandResult> outcome_;
    };

    /** @brief Host-owned immutable adapter table and synchronous application dispatcher. */
    class CliDispatcher final {
    public:
        /**
         * @brief Validates and atomically owns an executable adapter table.
         * @param registry Accepted inert descriptor inventory to own.
         * @param registrations Domain adapters and their exact constructor-bound capabilities.
         * @param policy Host authority and execution bounds.
         * @return Dispatcher or a typed activation failure; no adapter executes during creation.
         * @throws std::bad_alloc When retaining accepted registrations fails.
         */
        [[nodiscard]] static Result<CliDispatcher> Create(CliCommandRegistry registry,
                                                          std::vector<CliCommandAdapterRegistration> registrations,
                                                          CliDispatchPolicy policy);

        CliDispatcher(CliDispatcher &&) noexcept = default;
        CliDispatcher &operator=(CliDispatcher &&) noexcept = default;
        CliDispatcher(const CliDispatcher &) = delete;
        CliDispatcher &operator=(const CliDispatcher &) = delete;

        /**
         * @brief Executes one validated request after capability, authority, context, and timeout admission.
         * @param request Parser-produced command request.
         * @param invocation Explicit invocation-scoped host facts.
         * @return Terminal envelope retaining correlation for every success or failure path.
         * @throws std::bad_alloc When bounded adapter output or error context cannot be retained.
         *
         * `Execute` is a structured one-shot scope: an adapter must join or cancel all child work and
         * release invocation subscriptions before returning. The dispatcher owns adapters and must be
         * destroyed before their constructor-injected application services.
         */
        [[nodiscard]] CliTerminalResult Dispatch(const CliCommandRequest &request, const CliInvocationContext &invocation);
        /** @brief Returns the immutable descriptor inventory paired with this dispatcher. @return Accepted registry. */
        [[nodiscard]] const CliCommandRegistry &Registry() const noexcept;

    private:
        struct Entry final {
            const CliCommandDescriptor *descriptor{};
            std::unique_ptr<ICliCommandAdapter> adapter;
            std::vector<CliCapabilityId> capabilities;
        };

        /** @brief Takes ownership of an already-validated registry and adapter table. */
        CliDispatcher(CliCommandRegistry registry, std::vector<Entry> entries, CliDispatchPolicy policy);
        /** @brief Applies bounded terminal validation after one adapter invocation returns. */
        [[nodiscard]] CliTerminalResult Finalize(const CliInvocationContext &invocation, const CliExecutionContext &context,
                                                 const std::optional<std::chrono::steady_clock::time_point> &deadline,
                                                 Result<CliCommandResult> result) const;
        CliCommandRegistry registry_;
        std::vector<Entry> entries_;
        CliDispatchPolicy policy_;
    };
}  // namespace Horo::Cli
