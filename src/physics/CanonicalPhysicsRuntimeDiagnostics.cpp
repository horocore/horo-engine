#include "CanonicalPhysicsRuntimeDiagnostics.h"

#include <Jolt/Core/Memory.h>
#include <algorithm>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Native allocation callbacks retained while the explicit canonical owner is alive. */
        struct AllocatorFunctions final {
            JPH::AllocateFunction allocate{};
            JPH::ReallocateFunction reallocate{};
            JPH::AlignedAllocateFunction alignedAllocate{};
        };

        using NativeAllocationBlock = std::invoke_result_t<JPH::AllocateFunction, std::size_t>;

        /** @brief Returns the process-owned native allocator callbacks saved around checked installation. */
        AllocatorFunctions &InstalledAllocatorFunctions() noexcept {
            static AllocatorFunctions functions;
            return functions;
        }

        /** @brief Native allocation cannot unwind through no-exception Jolt frames; fail closed on exhaustion. */
        NativeAllocationBlock RequireNativeAllocation(NativeAllocationBlock memory) noexcept {
            if (memory == nullptr)
                std::abort();
            return memory;
        }

        /** @brief Delegates one native allocation while preserving the process-owned checked callback contract. */
        NativeAllocationBlock CheckedAllocate(const std::size_t size) {
            return RequireNativeAllocation(InstalledAllocatorFunctions().allocate(std::max(size, std::size_t{1})));
        }

        /** @brief Delegates one native reallocation while preserving the process-owned checked callback contract. */
        NativeAllocationBlock CheckedReallocate(NativeAllocationBlock memory, const std::size_t oldSize, const std::size_t newSize) {
            return RequireNativeAllocation(InstalledAllocatorFunctions().reallocate(memory, oldSize, std::max(newSize, std::size_t{1})));
        }

        /** @brief Delegates one aligned native allocation while preserving the process-owned checked callback contract. */
        NativeAllocationBlock CheckedAlignedAllocate(const std::size_t size, const std::size_t alignment) {
            return RequireNativeAllocation(InstalledAllocatorFunctions().alignedAllocate(std::max(size, std::size_t{1}), alignment));
        }
    }  // namespace

    void DiagnosticInbox::AppendDroppedSummary(std::string &message, const std::uint32_t droppedCount) {
        if (droppedCount == 0)
            return;
        std::array<char, 16> countText{};
        const auto [countEnd, error] = std::to_chars(countText.data(), countText.data() + countText.size(), droppedCount);
        if (error != std::errc{})
            return;
        std::string suffix{" ["};
        suffix.append(countText.data(), static_cast<std::size_t>(countEnd - countText.data()));
        suffix.append(" additional messages dropped]");
        if (message.size() + suffix.size() > MaximumPhysicsDiagnosticMessageBytes)
            message.resize(MaximumPhysicsDiagnosticMessageBytes - suffix.size());
        message.append(suffix);
    }

    Error DiagnosticInbox::MakeDiagnosticError(const CanonicalDiagnosticKind kind, std::string message) {
        using enum CanonicalDiagnosticKind;
        switch (kind) {
            case Validation:
                return MakeError(PhysicsErrors::SolverValidationMessage, std::move(message));
            case Assertion:
                return MakeError(PhysicsErrors::SolverAssertionFailed, std::move(message));
            case Fatal:
                return MakeError(PhysicsErrors::SolverFatalCondition, std::move(message));
        }
        return MakeError(PhysicsErrors::SolverFatalCondition, "Unknown canonical solver diagnostic classification.");
    }

    void DiagnosticInbox::RetainEmergency(const CanonicalDiagnosticKind kind) noexcept {
        const auto desired = static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) + std::uint8_t{1});
        std::uint8_t current = emergencyKind.load();
        while (current < desired && !emergencyKind.compare_exchange_weak(current, desired)) {
            // A producer changed the priority; retry only while this condition is more severe.
        }
    }

    void DiagnosticInbox::Submit(const CanonicalDiagnosticKind kind, const std::string_view message) noexcept {
        if (lock.test_and_set()) {
            dropped.fetch_add(1);
            if (kind != CanonicalDiagnosticKind::Validation)
                RetainEmergency(kind);
            return;
        }
        if (!occupied || kind > retainedKind) {
            retainedKind = kind;
            const std::string_view evidence =
                message.empty() ? std::string_view{"Native solver emitted an empty diagnostic message."} : message;
            const std::size_t count = std::min(evidence.size(), text.size() - 1);
            std::copy_n(evidence.data(), count, text.data());
            text[count] = '\0';
            size = count;
            occupied = true;
        }
        lock.clear();
    }

    std::optional<Error> DiagnosticInbox::Drain() {
        if (lock.test_and_set())
            return MakeError(PhysicsErrors::SolverFatalCondition,
                             "A native solver callback did not quiesce before the owner-thread drain boundary.");
        const std::uint8_t emergency = emergencyKind.exchange(0);
        if (!occupied && emergency == 0) {
            lock.clear();
            return std::nullopt;
        }
        CanonicalDiagnosticKind kind = occupied ? retainedKind : static_cast<CanonicalDiagnosticKind>(emergency - 1);
        std::string message =
            occupied ? std::string{text.data(), size} : std::string{"Native solver fatal evidence was bounded by callback contention."};
        if (occupied && emergency > static_cast<std::uint8_t>(kind) + 1) {
            kind = static_cast<CanonicalDiagnosticKind>(emergency - 1);
            message = "Native solver fatal evidence was bounded by callback contention.";
        }
        AppendDroppedSummary(message, dropped.exchange(0));
        occupied = false;
        size = 0;
        lock.clear();
        return MakeDiagnosticError(kind, std::move(message));
    }

    std::atomic<DiagnosticInbox *> &ActiveDiagnosticInbox() noexcept {
        static std::atomic<DiagnosticInbox *> inbox;
        return inbox;
    }

    void CaptureNativeTrace(const char *format, ...) noexcept {  // NOSONAR -- JPH::TraceFunction requires this C varargs ABI.
        DiagnosticInbox *inbox = ActiveDiagnosticInbox().load();
        if (inbox == nullptr || format == nullptr)
            return;
        thread_local std::array<char, MaximumPhysicsDiagnosticMessageBytes + 1> message{};
        message.fill('\0');
        std::va_list arguments;
        va_start(arguments, format);
        const int formatted = std::vsnprintf(message.data(), message.size(), format, arguments);  // NOSONAR -- native format ABI.
        va_end(arguments);
        if (formatted < 0)
            inbox->Submit(CanonicalDiagnosticKind::Validation, "Native solver validation message could not be formatted.");
        else
            inbox->Submit(CanonicalDiagnosticKind::Validation, message.data());
    }

#ifdef JPH_ENABLE_ASSERTS
    bool CaptureNativeAssertion(const char *expression, const char *message, const char *, const JPH::uint) noexcept {
        DiagnosticInbox *inbox = ActiveDiagnosticInbox().load();
        if (inbox == nullptr)
            return false;
        std::string_view evidence{"Native solver assertion"};
        if (message != nullptr && message[0] != '\0')
            evidence = message;
        else if (expression != nullptr)
            evidence = expression;
        inbox->Submit(CanonicalDiagnosticKind::Assertion, evidence);
        return false;
    }
#endif

    void InstallAllocators() {
        JPH::RegisterDefaultAllocator();
        InstalledAllocatorFunctions() = {JPH::Allocate, JPH::Reallocate, JPH::AlignedAllocate};
        JPH::Allocate = CheckedAllocate;
        JPH::Reallocate = CheckedReallocate;
        JPH::AlignedAllocate = CheckedAlignedAllocate;
    }

    void ResetAllocators() noexcept {
        InstalledAllocatorFunctions() = {};
    }
}  // namespace Horo::Physics::Detail
