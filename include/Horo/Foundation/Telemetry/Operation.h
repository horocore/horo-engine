#pragma once

/**
 * @file Operation.h
 * @brief Backend-neutral operation/span lifecycle and cross-thread context binding.
 */

#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Telemetry {
    using OperationId = std::uint64_t;

    /** @brief Immutable logical-work context safe to retain across jobs and threads. */
    struct OperationContext {
        OperationId operationId{};
        OperationId parentOperationId{};
        Log::LogContextSnapshot diagnosticContext;
    };

    /** @brief Captures only numeric operation lineage without clock reads, allocation or ambient field copying.
     * @return Owned numeric identity with empty diagnostic context; callers explicitly supply any approved fields.
     */
    [[nodiscard]] OperationContext CaptureOperationIdentity() noexcept;

    /**
     * @brief Captures active operation identity and all inherited diagnostic context.
     * @return Independent snapshot whose lifetime is not tied to the current scope.
     */
    [[nodiscard]] OperationContext CaptureOperationContext();

    /** @brief Binds a captured logical-work context to the calling thread for one scope. */
    class ScopedOperationContext final {
    public:
        /**
         * @brief Restores a captured operation and diagnostic context.
         * @param context Immutable context captured by a scheduling thread.
         */
        explicit ScopedOperationContext(const OperationContext &context);
        ScopedOperationContext(const ScopedOperationContext &) = delete;
        ScopedOperationContext &operator=(const ScopedOperationContext &) = delete;
        ScopedOperationContext(ScopedOperationContext &&) = delete;
        ScopedOperationContext &operator=(ScopedOperationContext &&) = delete;
        ~ScopedOperationContext();

    private:
        bool pushedOperation_{};
        std::optional<Log::ScopedLogContext> diagnosticBinding_;
    };

    /**
     * @brief Scope-owned meaningful operation that emits one terminal span record.
     *
     * Operations are intentionally non-movable because they bind thread-local
     * context. Destroying an incomplete operation records it as cancelled.
     */
    class OperationSpan final {
    public:
        /**
         * @brief Begins a nested operation and installs its context on this thread.
         * @param subsystem Stable hierarchical owner.
         * @param name Stable low-cardinality operation name.
         */
        OperationSpan(std::string_view subsystem, std::string_view name);
        OperationSpan(const OperationSpan &) = delete;
        OperationSpan &operator=(const OperationSpan &) = delete;
        OperationSpan(OperationSpan &&) = delete;
        OperationSpan &operator=(OperationSpan &&) = delete;
        ~OperationSpan();

        /** @brief Returns the process-local operation identifier. */
        [[nodiscard]] OperationId Id() const noexcept;
        /** @brief Returns the inherited parent operation identifier, or zero. */
        [[nodiscard]] OperationId ParentId() const noexcept;
        /** @brief Returns an immutable snapshot suitable for explicit async forwarding. */
        [[nodiscard]] const OperationContext &Context() const noexcept;

        /**
         * @brief Commits the single terminal state and emits its span record.
         * @param status Succeeded, Failed, Cancelled, or TimedOut.
         * @param fields Bounded terminal metadata copied into the record.
         * @return True only for the first valid terminal transition.
         */
        [[nodiscard]] bool Complete(SpanStatus status, std::span<const Field> fields = {}) noexcept;

    private:
        std::string subsystem_;
        std::string name_;
        OperationContext context_;
        std::optional<ScopedOperationContext> binding_;
        std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};
        bool completed_{};
    };
}  // namespace Horo::Telemetry
