#pragma once

/** @file CanonicalPhysicsRuntimeDiagnostics.h
 * @brief Private native diagnostic callback state for the canonical Physics runtime.
 */

#include "CanonicalPhysicsRuntime.h"
#include "Horo/Physics/PhysicsDiagnostics.h"

#include <Jolt/Jolt.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Physics::Detail {
    /** @brief Bounded callback mailbox drained by the owner thread after a native step. */
    struct DiagnosticInbox final {
        /** @brief Appends a bounded owner-thread summary for messages rejected by callback contention. */
        static void AppendDroppedSummary(std::string &message, std::uint32_t droppedCount);
        /** @brief Maps one private normalized classification to a stable Horo error. */
        [[nodiscard]] static Error MakeDiagnosticError(CanonicalDiagnosticKind kind, std::string message);
        /** @brief Retains the most severe callback evidence observed during contention. */
        void RetainEmergency(CanonicalDiagnosticKind kind) noexcept;
        /** @brief Submits one bounded callback message without retaining native memory. */
        void Submit(CanonicalDiagnosticKind kind, std::string_view message) noexcept;
        /** @brief Drains one callback generation into an owner-thread error. */
        [[nodiscard]] std::optional<Error> Drain();

        std::atomic_flag lock = ATOMIC_FLAG_INIT;
        std::atomic<std::uint32_t> dropped{};
        std::atomic<std::uint8_t> emergencyKind{};
        std::array<char, MaximumPhysicsDiagnosticMessageBytes + 1> text{};
        std::size_t size{};
        CanonicalDiagnosticKind retainedKind{CanonicalDiagnosticKind::Validation};
        bool occupied{};
    };

    /** @brief Returns the process-owned active callback mailbox, if one is installed. */
    [[nodiscard]] std::atomic<DiagnosticInbox *> &ActiveDiagnosticInbox() noexcept;
    /** @brief Captures one native trace callback into bounded owned evidence. */
    void CaptureNativeTrace(const char *format, ...) noexcept;
#ifdef JPH_ENABLE_ASSERTS
    /** @brief Captures one native assertion callback into bounded owned evidence. */
    bool CaptureNativeAssertion(const char *expression, const char *message, const char *file, JPH::uint line) noexcept;
#endif
    /** @brief Installs checked native allocation callbacks for the canonical process owner. */
    void InstallAllocators();
    /** @brief Clears the saved native allocation callbacks after process-global hooks are restored. */
    void ResetAllocators() noexcept;
}  // namespace Horo::Physics::Detail
