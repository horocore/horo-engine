#pragma once

/**
 * @file ScriptInvocation.h
 * @brief Host-owned script invocation lifecycle, cancellation, affinity, and completion ABI.
 */

#include "Horo/Extensions/ScriptValue.h"
#include "Horo/Foundation/CancellationToken.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Extensions {
    struct ScriptInvocationRegistryState;
    struct ScriptInvocationProviderState;
    struct ScriptInvocationContextState;
    struct ScriptInvocationState;
    class ScriptInvocationRegistry;
    class ScriptInvocationProviderRegistration;
    class ScriptInvocationContextRegistration;
    class ScriptInvocationController;

    /** @brief Host-issued identity of one accepted script invocation. */
    struct ScriptInvocationId final {
        std::uint64_t value{}; /**< Non-zero identity; zero is invalid. */

        /** @brief Reports whether this identity is usable. @return False only for zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        constexpr auto operator<=>(const ScriptInvocationId &) const noexcept = default;
    };

    /** @brief Closed affinity where runtime completion may be delivered. */
    enum class ScriptInvocationCompletionAffinity : std::uint8_t {
        OwnerThreadSafePoint, /**< Context owner thread explicitly drains at a declared safe point. */
        Count,
    };

    /** @brief Immutable lifecycle state of one accepted invocation. */
    enum class ScriptInvocationStateKind : std::uint8_t {
        Queued,
        Running,
        Completed,
        Failed,
        Cancelled,
    };

    /** @brief Source that terminalized a cancelled invocation. */
    enum class ScriptInvocationCancellationReason : std::uint8_t {
        None,
        Caller,
        Context,
        Provider,
        Timeout,
        Shutdown,
    };

    /** @brief Result of a caller cancellation request. */
    enum class ScriptInvocationCancellationRequestResult : std::uint8_t {
        Requested,
        AlreadyRequested,
        AlreadyTerminal,
        InvalidHandle,
    };

    /** @brief Result of a provider/controller lifecycle transition. */
    enum class ScriptInvocationTransitionResult : std::uint8_t {
        Applied,
        CancellationWon,
        AlreadyTerminal,
        InvalidTransition,
    };

    /** @brief Result of a bounded progress publication. */
    enum class ScriptInvocationProgressDisposition : std::uint8_t {
        Published,
        Coalesced,
        CancellationWon,
        AlreadyTerminal,
    };

    /** @brief Result of observing cooperative cancellation from provider work. */
    enum class ScriptInvocationCancellationObservation : std::uint8_t {
        NotRequested,
        Cancelled,
        AlreadyTerminal,
    };

    /** @brief Exact monotonic progress in one named phase. */
    struct ScriptInvocationProgress final {
        std::string phase{"queued"};    /**< Stable bounded phase identity. */
        std::uint64_t completedUnits{}; /**< Completed units, never greater than totalUnits. */
        std::uint64_t totalUnits{1};    /**< Positive exact phase-local unit bound. */

        /** @brief Returns normalized progress without changing exact counters. @return Value in [0, 1]. */
        [[nodiscard]] double Fraction() const noexcept;
    };

    /** @brief Host bounds for invocation records, handles, events, and timeout admission. */
    struct ScriptInvocationRegistryLimits final {
        std::size_t maximumContexts{64};                    /**< Maximum live script contexts. */
        std::size_t maximumProviders{256};                  /**< Maximum live provider generations. */
        std::size_t maximumInvocations{1024};               /**< Maximum active invocations across contexts. */
        std::size_t maximumQueuedEvents{4096};              /**< Maximum progress plus terminal events retained for delivery. */
        std::size_t maximumHandlesPerContext{1024};         /**< Maximum active opaque handles in one context. */
        std::size_t maximumProgressEventsPerInvocation{64}; /**< Maximum separately queued progress events per invocation. */
        std::chrono::milliseconds maximumTimeout{std::chrono::hours{24}}; /**< Maximum admitted invocation timeout. */
        ScriptValueLimits value;                                          /**< Value and result codec bounds. */
    };

    /** @brief Provider generation admission and per-generation in-flight bound. */
    struct ScriptInvocationProviderDescriptor final {
        std::uint64_t generation{};         /**< Non-zero immutable provider generation identity. */
        std::size_t maximumInvocations{64}; /**< Maximum concurrent invocations for this generation. */
    };

    /** @brief Context admission and per-context in-flight/handle bounds. */
    struct ScriptInvocationContextDescriptor final {
        CancellationToken parentCancellation; /**< Optional parent cancellation for the context. */
        std::size_t maximumInvocations{64};   /**< Maximum concurrent invocations for this context. */
        std::size_t maximumHandles{1024};     /**< Maximum active opaque handles for this context. */
    };

    /** @brief Immutable request copied before provider work is admitted. */
    struct ScriptInvocationRequest final {
        ScriptExportDescriptorSnapshotPtr descriptors;    /**< Validated immutable API generation. */
        std::string apiId;                                /**< Exact script API identity. */
        std::string functionId;                           /**< Exact function identity within the API. */
        std::vector<ScriptValue> arguments;               /**< Owned ordered arguments; no VM values or borrowed views. */
        std::optional<std::chrono::milliseconds> timeout; /**< Optional finite timeout; zero expires at admission. */
        ScriptInvocationCompletionAffinity completionAffinity{ScriptInvocationCompletionAffinity::OwnerThreadSafePoint};
    };

    /** @brief Immutable invocation snapshot copied under the host lifecycle lock. */
    struct ScriptInvocationSnapshot final {
        ScriptInvocationId invocation;      /**< Host-issued invocation identity. */
        ScriptContextId context;            /**< Owning script context identity. */
        std::uint64_t providerGeneration{}; /**< Exact provider generation admitted for the call. */
        std::string apiId;                  /**< Copied API identity. */
        std::string functionId;             /**< Copied function identity. */
        ScriptExportInvocationMode invocationMode{ScriptExportInvocationMode::Count}; /**< Descriptor scheduling mode. */
        ScriptInvocationCompletionAffinity completionAffinity{ScriptInvocationCompletionAffinity::Count};
        ScriptInvocationStateKind state{ScriptInvocationStateKind::Queued}; /**< Current or immutable terminal state. */
        ScriptInvocationProgress progress;                                  /**< Latest bounded progress. */
        ScriptInvocationCancellationReason cancellationReason{ScriptInvocationCancellationReason::None};
        bool cancellationRequested{};                   /**< A caller, context, provider, timeout, or shutdown request is visible. */
        std::optional<ScriptCallResult> terminalResult; /**< Exactly one copied terminal result after completion. */
        std::uint64_t revision{1};                      /**< Monotonically increasing state revision. */

        /** @brief Reports whether this snapshot is terminal. @return True for completed, failed, or cancelled. */
        [[nodiscard]] bool IsTerminal() const noexcept;
    };

    /** @brief Event kind returned by the context owner safe-point drain. */
    enum class ScriptInvocationEventKind : std::uint8_t {
        Progress,
        Completed,
    };

    /**
     * @brief Copied event delivered once to a live context owner at its declared safe point.
     *
     * Events never contain callbacks or VM objects. A terminal event is reserved at admission,
     * so bounded backpressure may coalesce progress without orphaning a pending completion.
     */
    struct ScriptInvocationEvent final {
        ScriptInvocationEventKind kind{ScriptInvocationEventKind::Progress};
        ScriptInvocationId invocation;
        ScriptContextId context;
        std::uint64_t providerGeneration{};
        ScriptInvocationProgress progress;
        std::optional<ScriptCallResult> terminalResult;
        std::uint64_t revision{1};
    };

    /** @brief Move-only provider-generation registration whose reset revokes its invocations and handles. */
    class ScriptInvocationProviderRegistration final {
    public:
        ~ScriptInvocationProviderRegistration() noexcept;
        ScriptInvocationProviderRegistration(const ScriptInvocationProviderRegistration &) = delete;
        ScriptInvocationProviderRegistration &operator=(const ScriptInvocationProviderRegistration &) = delete;
        ScriptInvocationProviderRegistration(ScriptInvocationProviderRegistration &&other) noexcept;
        ScriptInvocationProviderRegistration &operator=(ScriptInvocationProviderRegistration &&other) noexcept;

        /** @brief Revokes new admission and terminalizes active invocations without invoking provider code. */
        void Reset() noexcept;
        /** @brief Reports whether this generation is still admitted. @return True while registered. */
        [[nodiscard]] bool IsRegistered() const noexcept;
        /** @brief Returns the immutable generation identity. @return Zero for moved-from registration. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;

    private:
        friend class ScriptInvocationRegistry;
        ScriptInvocationProviderRegistration(std::weak_ptr<ScriptInvocationRegistryState> registry,
                                             std::shared_ptr<ScriptInvocationProviderState> provider) noexcept;

        std::weak_ptr<ScriptInvocationRegistryState> registry_;
        std::shared_ptr<ScriptInvocationProviderState> provider_;
    };

    /** @brief Move-only script context registration owning safe-point delivery and context revocation. */
    class ScriptInvocationContextRegistration final {
    public:
        ~ScriptInvocationContextRegistration() noexcept;
        ScriptInvocationContextRegistration(const ScriptInvocationContextRegistration &) = delete;
        ScriptInvocationContextRegistration &operator=(const ScriptInvocationContextRegistration &) = delete;
        ScriptInvocationContextRegistration(ScriptInvocationContextRegistration &&other) noexcept;
        ScriptInvocationContextRegistration &operator=(ScriptInvocationContextRegistration &&other) noexcept;

        /** @brief Revokes delivery first, cancels active work, and invalidates all context handles. */
        void Reset() noexcept;
        /** @brief Reports whether the context still admits calls and completion delivery. @return True while registered. */
        [[nodiscard]] bool IsRegistered() const noexcept;
        /** @brief Returns the immutable context identity. @return Invalid identity for moved-from registration. */
        [[nodiscard]] ScriptContextId Id() const noexcept;

    private:
        friend class ScriptInvocationRegistry;
        ScriptInvocationContextRegistration(std::weak_ptr<ScriptInvocationRegistryState> registry,
                                            std::shared_ptr<ScriptInvocationContextState> context) noexcept;

        std::weak_ptr<ScriptInvocationRegistryState> registry_;
        std::shared_ptr<ScriptInvocationContextState> context_;
    };

    /** @brief Copyable host capability for polling, snapshots, and cooperative cancellation. */
    class ScriptInvocationHandle final {
    public:
        ScriptInvocationHandle() = default;

        /** @brief Reports whether this handle names an admitted invocation. @return True for a usable handle. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the host-issued invocation identity. @return Invalid identity for an empty handle. */
        [[nodiscard]] ScriptInvocationId Id() const noexcept;
        /** @brief Copies the latest immutable snapshot without invoking provider or runtime code. */
        [[nodiscard]] std::optional<ScriptInvocationSnapshot> Snapshot() const;
        /** @brief Returns the latest state revision. @return Empty for an invalid handle. */
        [[nodiscard]] std::optional<std::uint64_t> Revision() const noexcept;
        /** @brief Requests caller cancellation and terminalizes exactly once. @return Stable request disposition. */
        [[nodiscard]] ScriptInvocationCancellationRequestResult RequestCancellation() const noexcept;

    private:
        friend class ScriptInvocationController;
        explicit ScriptInvocationHandle(std::shared_ptr<ScriptInvocationState> state) noexcept;

        std::shared_ptr<ScriptInvocationState> state_;
    };

    /** @brief Move-only provider-side controller that must publish exactly one terminal result. */
    class ScriptInvocationController final {
    public:
        ~ScriptInvocationController() noexcept;
        ScriptInvocationController(const ScriptInvocationController &) = delete;
        ScriptInvocationController &operator=(const ScriptInvocationController &) = delete;
        ScriptInvocationController(ScriptInvocationController &&other) noexcept;
        ScriptInvocationController &operator=(ScriptInvocationController &&other) noexcept;

        /** @brief Returns the copyable polling/cancellation handle. @return Empty only after move. */
        [[nodiscard]] ScriptInvocationHandle Handle() const noexcept;
        /** @brief Returns the immutable copied request consumed by the provider adapter. @return Borrowed request or nullptr after move. */
        [[nodiscard]] const ScriptInvocationRequest *Request() const noexcept;
        /** @brief Returns the cooperative token observed by provider work. @return Empty token after move. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;

        /** @brief Publishes bounded monotonic progress or coalesces it under event backpressure. */
        [[nodiscard]] Result<ScriptInvocationProgressDisposition> PublishProgress(ScriptInvocationProgress progress) const;
        /** @brief Observes cancellation and terminalizes when a cancellation source wins. */
        [[nodiscard]] Result<ScriptInvocationCancellationObservation> ObserveCancellation() const;
        /** @brief Publishes one successful terminal result and one safe-point completion event. */
        [[nodiscard]] Result<ScriptInvocationTransitionResult> Complete(ScriptCallResult result = ScriptCallResult::Success()) const;
        /** @brief Publishes one structured failed terminal result and one safe-point completion event. */
        [[nodiscard]] Result<ScriptInvocationTransitionResult> Fail(ScriptError error) const;
        /** @brief Converts and publishes one Foundation error while preserving its stable identity. */
        [[nodiscard]] Result<ScriptInvocationTransitionResult> Fail(const Error &error, bool retryable = false) const;

    private:
        explicit ScriptInvocationController(std::shared_ptr<ScriptInvocationState> state) noexcept;
        friend class ScriptInvocationRegistry;

        void Abandon() noexcept;
        std::shared_ptr<ScriptInvocationState> state_;
    };

    /** @brief Explicit host-owned registry for safe script values and asynchronous invocation records. */
    class ScriptInvocationRegistry final {
    public:
        static constexpr std::size_t MaximumContexts = 256;
        static constexpr std::size_t MaximumProviders = 1024;
        static constexpr std::size_t MaximumInvocations = 8192;

        explicit ScriptInvocationRegistry(ScriptInvocationRegistryLimits limits = {});
        ~ScriptInvocationRegistry() noexcept;
        ScriptInvocationRegistry(const ScriptInvocationRegistry &) = delete;
        ScriptInvocationRegistry &operator=(const ScriptInvocationRegistry &) = delete;
        ScriptInvocationRegistry(ScriptInvocationRegistry &&) noexcept = delete;
        ScriptInvocationRegistry &operator=(ScriptInvocationRegistry &&) noexcept = delete;

        /**
         * @brief Registers one provider generation without invoking provider code.
         * @param descriptor Non-zero generation and finite concurrent-call bound.
         * @return Move-only registration or a typed invalid, duplicate, capacity, or shutdown error.
         */
        [[nodiscard]] Result<ScriptInvocationProviderRegistration> RegisterProvider(ScriptInvocationProviderDescriptor descriptor);

        /**
         * @brief Registers a context on the calling thread, which becomes its completion owner.
         * @param descriptor Parent cancellation and finite context bounds.
         * @return Move-only context registration or a typed invalid, capacity, or shutdown error.
         */
        [[nodiscard]] Result<ScriptInvocationContextRegistration> RegisterContext(ScriptInvocationContextDescriptor descriptor = {});

        /**
         * @brief Validates and admits one copied call against an exact descriptor generation.
         * @param context Live context registration whose owner thread submits the call.
         * @param provider Live provider-generation registration owning the call authority.
         * @param request Copied descriptor target, values, timeout, and completion affinity.
         * @return Move-only controller or a typed malformed, wrong-thread, unavailable, or capacity failure.
         */
        [[nodiscard]] Result<ScriptInvocationController> Begin(const ScriptInvocationContextRegistration &context,
                                                               const ScriptInvocationProviderRegistration &provider,
                                                               ScriptInvocationRequest request);

        /**
         * @brief Drains bounded progress and terminal events on the context owner safe point.
         * @param context Live context registration being drained.
         * @param maximumEvents Maximum events to move into the returned owned vector.
         * @return Owned events or a typed invalid/wrong-thread/revoked failure.
         */
        [[nodiscard]] Result<std::vector<ScriptInvocationEvent>> Drain(const ScriptInvocationContextRegistration &context,
                                                                       std::size_t maximumEvents = 64);

        /**
         * @brief Issues one generation-safe opaque handle for a live context/provider pair.
         * @param context Live context registration.
         * @param provider Live provider generation that owns the resource.
         * @param type Stable bounded resource type identity.
         * @return Opaque identifier or a typed invalid, revoked, capacity, or shutdown failure.
         */
        [[nodiscard]] Result<ScriptHandle> IssueHandle(const ScriptInvocationContextRegistration &context,
                                                       const ScriptInvocationProviderRegistration &provider, std::string type);

        /**
         * @brief Releases one exact live opaque handle from its owning context.
         * @param handle Handle identity copied from the runtime adapter.
         * @return Success or a typed malformed, stale, or revoked failure.
         */
        [[nodiscard]] Result<void> ReleaseHandle(const ScriptHandle &handle);

        /**
         * @brief Validates that an opaque handle still belongs to a live context/provider generation.
         * @param handle Candidate handle copied from a runtime adapter.
         * @return Success or a typed malformed, wrong-owner, stale, or revoked failure.
         */
        [[nodiscard]] Result<void> ValidateHandle(const ScriptHandle &handle) const;

        /** @brief Closes admission and revokes every provider/context without invoking external code. */
        void BeginShutdown() noexcept;
        /** @brief Reports whether registration and invocation admission are closed. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        friend class ScriptInvocationProviderRegistration;
        friend class ScriptInvocationContextRegistration;

        static void ResetProviderState(const std::shared_ptr<ScriptInvocationRegistryState> &registry,
                                       const std::shared_ptr<ScriptInvocationProviderState> &provider,
                                       ScriptInvocationCancellationReason reason) noexcept;
        static void ResetContextState(const std::shared_ptr<ScriptInvocationRegistryState> &registry,
                                      const std::shared_ptr<ScriptInvocationContextState> &context,
                                      ScriptInvocationCancellationReason reason) noexcept;

        std::shared_ptr<ScriptInvocationRegistryState> state_;
    };
}  // namespace Horo::Extensions
