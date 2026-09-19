#pragma once

/**
 * @file BackendOperationRegistry.h
 * @brief Host-owned lifecycle, cancellation, progress, diagnostic, and result contract for backend operations.
 */

#include "Horo/Extensions/ApplicationCapabilityRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Extensions {
    struct BackendOperationRegistryState;
    struct BackendOperationProviderState;
    struct BackendOperationStateData;
    class BackendOperationRegistry;
    class BackendOperationController;

    /** @brief Strong identity allocated by the host for one backend operation record. */
    struct BackendOperationId final {
        std::uint64_t value{}; /**< Non-zero host-issued value; zero is reserved for an invalid identity. */

        /** @brief Reports whether the identity was allocated by a registry. @return False only for the reserved zero value. */
        [[nodiscard]] bool IsValid() const noexcept {
            return value != 0;
        }

        /** @brief Returns the numeric operation identity. @return Zero only for an invalid identity. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value;
        }

        [[nodiscard]] constexpr auto operator<=>(const BackendOperationId &other) const noexcept = default;
    };

    /** @brief Strong identity of the typed result payload contract published by one operation. */
    struct BackendOperationResultId final {
        std::string value; /**< Canonical bounded result contract identity. */

        /** @brief Compares two result contract identities. @param other Identity to compare. @return True when equal. */
        [[nodiscard]] bool operator==(const BackendOperationResultId &other) const noexcept = default;
    };

    /** @brief Strong identity of one provider-owned backend operation contract. */
    struct BackendOperationTypeId final {
        std::string value; /**< Canonical bounded operation contract identity. */

        /** @brief Compares two operation contract identities. @param other Identity to compare. @return True when equal. */
        [[nodiscard]] bool operator==(const BackendOperationTypeId &other) const noexcept = default;
    };

    /** @brief Strong identity of one provider-defined stage in an operation pipeline. */
    struct BackendOperationPhaseId final {
        std::string value; /**< Canonical bounded phase identity. */

        /** @brief Compares two phase identities. @param other Identity to compare. @return True when equal. */
        [[nodiscard]] bool operator==(const BackendOperationPhaseId &other) const noexcept = default;
    };

    /** @brief Observable operation lifecycle; terminal values never change. */
    enum class BackendOperationState : std::uint8_t {
        Queued,    /**< Admitted but no progress has been published. */
        Running,   /**< At least one non-terminal producer transition was published. */
        Completed, /**< Successful immutable terminal state. */
        Failed,    /**< Failure or producer abandonment immutable terminal state. */
        Cancelled, /**< Cooperative cancellation immutable terminal state. */
    };

    /** @brief Source of the cancellation request that wins operation terminalization. */
    enum class BackendOperationCancellationReason : std::uint8_t {
        None,     /**< No cancellation has won. */
        Caller,   /**< The copyable caller handle requested cancellation. */
        Parent,   /**< The parent token was already cancelled. */
        Provider, /**< Provider registration reset requested cancellation. */
        Shutdown, /**< Registry shutdown requested cancellation. */
    };

    /** @brief Result of requesting cooperative cancellation through a copyable operation handle. */
    enum class BackendOperationCancellationRequestResult : std::uint8_t {
        Requested,        /**< This call published the cooperative request. */
        AlreadyRequested, /**< A prior cancellation request is already visible. */
        AlreadyTerminal,  /**< The operation already has an immutable terminal state. */
        InvalidHandle,    /**< The handle does not name an operation. */
    };

    /** @brief Result of a producer-side cancellation observation. */
    enum class BackendOperationCancellationObservation : std::uint8_t {
        NotRequested,    /**< No cancellation source is currently requested. */
        Cancelled,       /**< This observation published the cancelled terminal state. */
        AlreadyTerminal, /**< Another transition already published terminal state. */
    };

    /** @brief Result of an atomic producer transition. */
    enum class BackendOperationTransitionResult : std::uint8_t {
        Applied,           /**< The requested non-terminal or terminal transition committed. */
        CancellationWon,   /**< Cancellation won before the requested producer transition. */
        AlreadyTerminal,   /**< A previous terminal transition is immutable. */
        InvalidTransition, /**< Payload or progress violated the operation contract. */
    };

    /** @brief Exact monotonic work-unit progress for one typed phase. */
    struct BackendOperationProgress final {
        std::uint64_t completedUnits{}; /**< Completed units, never greater than totalUnits. */
        std::uint64_t totalUnits{1};    /**< Positive exact stage-local unit bound. */

        /** @brief Returns normalized progress without changing exact counters. @return Value in [0, 1]. */
        [[nodiscard]] double Fraction() const noexcept;
    };

    /** @brief Optional bounded typed result payload published only with successful completion. */
    struct BackendOperationResultPayload final {
        BackendOperationResultId id;  /**< Must equal the registered operation contract result identity. */
        std::vector<std::byte> bytes; /**< Host-copied bounded bytes; empty is allowed. */
    };

    /** @brief One typed operation contract published by a provider generation. */
    struct BackendOperationTypeDescriptor final {
        BackendOperationTypeId type;     /**< Canonical operation contract identity. */
        BackendOperationResultId result; /**< Canonical expected result contract identity. */
    };

    /** @brief Exact provider ownership, operation contracts, and per-provider bounds. */
    struct BackendOperationProviderDescriptor final {
        ApplicationCapabilityProviderIdentity provider;             /**< Existing module/provider/generation authority. */
        std::vector<BackendOperationTypeDescriptor> operationTypes; /**< Non-empty unique typed operation/result contracts. */
        std::size_t maximumOperations{64};                          /**< Maximum simultaneously active operations for this generation. */
        std::size_t maximumDiagnostics{64};                         /**< Maximum retained diagnostics per operation. */
        std::size_t maximumDiagnosticBytes{16U * 1024U};            /**< Aggregate code/message/source/path bound per operation. */
        std::size_t maximumResultBytes{4U * 1024U * 1024U};         /**< Maximum copied result bytes. */
    };

    /** @brief Registry-wide hard bounds used when admitting backend-operation records. */
    struct BackendOperationRegistryConfig final {
        std::size_t maximumOperations{1024}; /**< Maximum active records; zero is normalized to one and larger values are capped. */
    };

    /** @brief Inputs copied when one backend operation is admitted. */
    struct BackendOperationDescriptor final {
        BackendOperationPhaseId initialPhase{"queued"}; /**< Initial canonical phase identity. */
        CancellationToken parentCancellation;           /**< Optional parent/session cancellation token. */
    };

    /** @brief Immutable polling projection copied under one operation mutex. */
    struct BackendOperationSnapshot final {
        BackendOperationId operation;                               /**< Host-issued operation identity. */
        ApplicationCapabilityProviderIdentity provider;             /**< Exact module/provider/generation authority. */
        BackendOperationTypeId type;                                /**< Selected registered operation contract. */
        BackendOperationResultId result;                            /**< Expected typed result contract. */
        BackendOperationState state{BackendOperationState::Queued}; /**< Current or immutable terminal state. */
        BackendOperationPhaseId phase{"queued"};                    /**< Current typed stage. */
        BackendOperationProgress progress;                          /**< Exact stage-local progress. */
        std::vector<Diagnostic> diagnostics;                        /**< Bounded host-owned Foundation diagnostics. */
        std::optional<BackendOperationResultPayload> resultPayload; /**< Optional successful result. */
        std::optional<Error> terminalError;                         /**< Failure, abandonment, or cancellation cause. */
        BackendOperationCancellationReason cancellationReason{
            BackendOperationCancellationReason::None}; /**< Winning cancellation source. */
        bool cancellationRequested{};                  /**< Cooperative cancellation request is visible. */
        std::uint64_t revision{1};                     /**< Monotonically increasing snapshot revision. */

        /** @brief Reports whether this snapshot is immutable terminal state. @return True for completed, failed, or cancelled. */
        [[nodiscard]] bool IsTerminal() const noexcept;
    };

    /** @brief Move-only registration whose lifetime controls provider publication and active-operation teardown. */
    class BackendOperationRegistration final {
    public:
        ~BackendOperationRegistration() noexcept;
        BackendOperationRegistration(const BackendOperationRegistration &) = delete;
        BackendOperationRegistration &operator=(const BackendOperationRegistration &) = delete;
        BackendOperationRegistration(BackendOperationRegistration &&other) noexcept;
        BackendOperationRegistration &operator=(BackendOperationRegistration &&other) noexcept;

        /** @brief Revokes the provider and terminalizes every active operation without invoking provider code. */
        void Reset() noexcept;

        /** @brief Reports whether this registration still owns a published provider. @return True while discoverable. */
        [[nodiscard]] bool IsRegistered() const noexcept;

    private:
        friend class BackendOperationRegistry;
        BackendOperationRegistration(std::weak_ptr<BackendOperationRegistryState> registry,
                                     std::shared_ptr<BackendOperationProviderState> provider) noexcept;

        std::weak_ptr<BackendOperationRegistryState> registry_;
        std::shared_ptr<BackendOperationProviderState> provider_;
    };

    /** @brief Copyable host handle for polling and cooperative cancellation. */
    class BackendOperationHandle final {
    public:
        BackendOperationHandle() = default;

        /** @brief Reports whether this handle names an admitted operation. @return True for a usable handle. */
        [[nodiscard]] bool IsValid() const noexcept;

        /** @brief Returns the host operation identity. @return Invalid zero identity for an empty handle. */
        [[nodiscard]] BackendOperationId Id() const noexcept;

        /** @brief Copies the latest snapshot without waiting or invoking provider code. @return Empty for an invalid handle. */
        [[nodiscard]] std::optional<BackendOperationSnapshot> Snapshot() const;

        /** @brief Requests caller cancellation without waiting for the producer. @return Stable request disposition. */
        [[nodiscard]] BackendOperationCancellationRequestResult RequestCancellation() const noexcept;

    private:
        friend class BackendOperationController;
        explicit BackendOperationHandle(std::shared_ptr<BackendOperationStateData> state) noexcept;

        std::shared_ptr<BackendOperationStateData> state_;
    };

    /** @brief Move-only producer/controller that must publish exactly one terminal operation state. */
    class BackendOperationController final {
    public:
        ~BackendOperationController() noexcept;
        BackendOperationController(const BackendOperationController &) = delete;
        BackendOperationController &operator=(const BackendOperationController &) = delete;
        BackendOperationController(BackendOperationController &&other) noexcept;
        BackendOperationController &operator=(BackendOperationController &&other) noexcept;

        /** @brief Returns the copyable polling and cancellation capability. @return Empty only for a moved-from controller. */
        [[nodiscard]] BackendOperationHandle Handle() const noexcept;

        /** @brief Returns the producer token observed by backend work. @return Empty token for a moved-from controller. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;

        /**
         * @brief Publishes stage-local progress while preserving same-phase progress monotonicity.
         * @param phase Canonical phase identity; changing it resets stage-local progress.
         * @param progress Exact completed/total work units for the phase.
         * @return Applied, cancellation-won, already-terminal, or invalid-transition disposition.
         */
        [[nodiscard]] BackendOperationTransitionResult PublishProgress(BackendOperationPhaseId phase, BackendOperationProgress progress);

        /**
         * @brief Adds one bounded Foundation diagnostic to the current operation snapshot.
         * @param diagnostic Provider finding copied into host-owned bounded storage.
         * @return Success or a stable closed/payload-bound failure.
         */
        [[nodiscard]] Result<void> AddDiagnostic(Diagnostic diagnostic);

        /**
         * @brief Observes parent, caller, provider, or shutdown cancellation and terminalizes when it wins.
         * @return NotRequested, cancellation-won, or already-terminal disposition.
         */
        [[nodiscard]] BackendOperationCancellationObservation ObserveCancellation() noexcept;

        /**
         * @brief Publishes one immutable successful terminal state and optional bounded typed result payload.
         * @param result Optional result bytes whose identity matches the registered result contract.
         * @return Applied, cancellation-won, already-terminal, or invalid-transition disposition.
         * @note Omitting the payload is a valid successful completion for operations with no result bytes to publish.
         */
        [[nodiscard]] BackendOperationTransitionResult Complete(std::optional<BackendOperationResultPayload> result = {});

        /**
         * @brief Publishes one immutable failed terminal state while preserving the typed cause.
         * @param error Provider failure or host-attributed cause to retain in the terminal snapshot.
         * @return Applied, cancellation-won, or already-terminal disposition.
         */
        [[nodiscard]] BackendOperationTransitionResult Fail(Error error);

    private:
        explicit BackendOperationController(std::shared_ptr<BackendOperationStateData> state) noexcept;
        friend class BackendOperationRegistry;

        void Abandon() noexcept;
        std::shared_ptr<BackendOperationStateData> state_;
    };

    /** @brief Explicit host-owned registry for bounded backend-operation lifecycle records. */
    class BackendOperationRegistry final {
    public:
        static constexpr std::size_t MaximumProviders = 256;
        static constexpr std::size_t MaximumOperations = 4096;

        explicit BackendOperationRegistry(BackendOperationRegistryConfig config = {});
        ~BackendOperationRegistry() noexcept;
        BackendOperationRegistry(const BackendOperationRegistry &) = delete;
        BackendOperationRegistry &operator=(const BackendOperationRegistry &) = delete;
        BackendOperationRegistry(BackendOperationRegistry &&) noexcept = delete;
        BackendOperationRegistry &operator=(BackendOperationRegistry &&) noexcept = delete;

        /**
         * @brief Publishes one inert provider operation contract.
         * @param descriptor Exact module/provider/generation identity and hard payload bounds.
         * @return Move-only registration or a stable invalid, duplicate, capacity, or shutdown error.
         */
        [[nodiscard]] Result<BackendOperationRegistration> Register(BackendOperationProviderDescriptor descriptor);

        /**
         * @brief Begins one operation for an exact currently registered provider generation and operation contract.
         * @param provider Existing module/provider/generation authority selected by composition.
         * @param type Exact operation contract identity registered by that provider generation.
         * @param descriptor Initial phase and parent cancellation token.
         * @return Move-only producer/controller or a stable unavailable, capacity, invalid, or shutdown error.
         */
        [[nodiscard]] Result<BackendOperationController> Begin(const ApplicationCapabilityProviderIdentity &provider,
                                                               const BackendOperationTypeId &type,
                                                               BackendOperationDescriptor descriptor = {});

        /** @brief Idempotently closes admission and terminalizes every active operation. */
        void BeginShutdown() noexcept;

        /** @brief Reports whether registration and operation admission are terminally closed. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        std::shared_ptr<BackendOperationRegistryState> state_;
    };
}  // namespace Horo::Extensions
