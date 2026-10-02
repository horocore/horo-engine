#pragma once

/**
 * @file ProcessObserverRegistry.h
 * @brief Permission-gated, read-only observation of host process and operation lifecycle.
 */

#include "Horo/Extensions/ExtensionCapabilityAdmission.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Extensions {
    /** @brief The only process notifications an extension may request. */
    enum class ProcessObserverEventKind : std::uint8_t {
        HostStarted,
        HostStopping,
        OperationStarted,
        OperationFinished,
        DiagnosticRaised,
    };

    /** @brief Bounded outcome category; no provider error text is exposed. */
    enum class ProcessObserverOutcome : std::uint8_t {
        None,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Non-content diagnostic category approved for extension observation. */
    enum class ProcessObserverDiagnostic : std::uint8_t {
        None,
        OperationRejected,
        OperationTimedOut,
        ProviderFailed,
    };

    /**
     * @brief Host-created, content-free process notification.
     * @details No native process handle, PID, executable, arguments, path, environment,
     * credential, output, or unrestricted diagnostic string crosses this boundary.
     */
    struct ProcessObserverEvent final {
        ProcessObserverEventKind kind{ProcessObserverEventKind::HostStarted};
        std::uint64_t revision{};    /**< Non-zero host publication sequence. */
        std::uint64_t operationId{}; /**< Opaque host operation ID, or zero for host lifecycle. */
        ProcessObserverOutcome outcome{ProcessObserverOutcome::None};
        ProcessObserverDiagnostic diagnostic{ProcessObserverDiagnostic::None};
    };

    /** @brief Exact provider activation and its approved event allowlist. */
    struct ProcessObserverDescriptor final {
        std::string observerId;               /**< Canonical contribution identity. */
        std::string extensionId;              /**< Owning extension identity. */
        std::string moduleId;                 /**< Owning module identity. */
        std::uint64_t activationGeneration{}; /**< Non-zero admission generation. */
        std::vector<ProcessObserverEventKind> allowedEvents;
    };

    /** @brief Synchronous, borrowed, read-only callback; never receives process authority. */
    using ProcessObserverCallback = std::function<Result<void>(const ProcessObserverEvent &)>;

    struct ProcessObserverRegistryState;
    struct ProcessObserverEntry;

    /** @brief Move-only publication that removes future callback admission on reset. */
    class ProcessObserverRegistration final {
    public:
        ~ProcessObserverRegistration() noexcept;
        ProcessObserverRegistration(const ProcessObserverRegistration &) = delete;
        ProcessObserverRegistration &operator=(const ProcessObserverRegistration &) = delete;
        ProcessObserverRegistration(ProcessObserverRegistration &&other) noexcept;
        ProcessObserverRegistration &operator=(ProcessObserverRegistration &&other) noexcept;

        /** @brief Idempotently removes the observer from future dispatch. */
        void Reset() noexcept;
        /** @brief Reports whether this exact observer remains published. */
        [[nodiscard]] bool IsRegistered() const noexcept;

    private:
        friend class ProcessObserverRegistry;
        ProcessObserverRegistration(std::weak_ptr<ProcessObserverRegistryState> registry,
                                    std::shared_ptr<ProcessObserverEntry> entry) noexcept;

        std::weak_ptr<ProcessObserverRegistryState> registry_;
        std::shared_ptr<ProcessObserverEntry> entry_;
    };

    /**
     * @brief Composition-owned process observer extension point.
     * @details Registration and dispatch are synchronous and independent of GUI/backend state.
     * Dispatch runs only on the constructing thread; other producers marshal there. The
     * registry never creates, owns, cancels, or kills an external process. Callback
     * admission is closed before shutdown, and an in-flight callback retains its
     * provider code lease until it returns.
     */
    class ProcessObserverRegistry final {
    public:
        static constexpr std::size_t MaximumObservers = 256;
        static constexpr std::size_t MaximumEventKinds = 5;

        /** @brief Creates a registry with an immutable host event allowlist. */
        explicit ProcessObserverRegistry(std::vector<ProcessObserverEventKind> hostAllowedEvents);
        ~ProcessObserverRegistry() noexcept;
        ProcessObserverRegistry(const ProcessObserverRegistry &) = delete;
        ProcessObserverRegistry &operator=(const ProcessObserverRegistry &) = delete;
        ProcessObserverRegistry(ProcessObserverRegistry &&) noexcept = delete;
        ProcessObserverRegistry &operator=(ProcessObserverRegistry &&) noexcept = delete;

        /**
         * @brief Publishes one observer after exact activation and capability validation.
         * @param descriptor Copied identity and event allowlist, necessarily a subset of host policy.
         * @param authority Admission for `horo.process.observe`, requested with `process.observe` permission.
         * @param callback Provider callback invoked synchronously on the registry's owner thread.
         * @param codeLease Host-owned lease keeping callback code loaded through in-flight calls.
         * @return Lifetime registration or a typed validation, permission, duplicate, capacity, or shutdown error.
         */
        [[nodiscard]] Result<ProcessObserverRegistration> Register(ProcessObserverDescriptor descriptor,
                                                                   ExtensionCapabilityHandle authority, ProcessObserverCallback callback,
                                                                   std::shared_ptr<void> codeLease);

        /**
         * @brief Dispatches one validated host event to interested live observers in ID order.
         * @param event Content-free notification constructed by the host.
         * @return Success or a typed invalid, thread, reentrancy, shutdown, or attributed callback failure.
         * @note A failed callback is revoked; other observers still receive this notification.
         */
        [[nodiscard]] Result<void> Dispatch(const ProcessObserverEvent &event);

        /** @brief Closes future registration and callback admission; safe repeatedly. */
        void BeginShutdown() noexcept;
        /** @brief Reports whether the registry has entered terminal shutdown. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        std::shared_ptr<ProcessObserverRegistryState> state_;
    };
}  // namespace Horo::Extensions
