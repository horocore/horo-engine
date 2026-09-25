#include "Horo/Foundation/Telemetry/Operation.h"

#include "Horo/Foundation/Assertions.h"

#include <atomic>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Telemetry {
    namespace {
        OperationId NextOperationId() noexcept {
            static std::atomic<OperationId> nextId{1};
            return nextId.fetch_add(1, std::memory_order::seq_cst);
        }

        struct ActiveOperation {
            OperationId id{};
            OperationId parentId{};
        };

        std::vector<ActiveOperation> &ActiveOperations() {
            thread_local std::vector<ActiveOperation> operations;
            return operations;
        }

        void PushOperation(const OperationId id, const OperationId parentId) {
            ActiveOperations().push_back({.id = id, .parentId = parentId});
        }

        void PopOperation() {
            auto &operations = ActiveOperations();
            HORO_INVARIANT_MSG(!operations.empty(), "Operation context stack underflow.");
            operations.pop_back();
        }

        [[nodiscard]] bool IsTerminalStatus(const SpanStatus status) noexcept {
            using enum SpanStatus;
            return status == Succeeded || status == Failed || status == Cancelled || status == TimedOut;
        }
    }  // namespace

    /** @copydoc CaptureOperationContext */
    OperationContext CaptureOperationContext() {
        const auto &operations = ActiveOperations();
        if (operations.empty())
            return {.diagnosticContext = Log::CaptureLogContext()};
        return {.operationId = operations.back().id,
                .parentOperationId = operations.back().parentId,
                .diagnosticContext = Log::CaptureLogContext()};
    }

    /** @copydoc ScopedOperationContext::ScopedOperationContext */
    ScopedOperationContext::ScopedOperationContext(const OperationContext &context) {
        diagnosticBinding_.emplace(context.diagnosticContext);
        if (context.operationId != 0) {
            PushOperation(context.operationId, context.parentOperationId);
            pushedOperation_ = true;
        }
    }

    /** @copydoc ScopedOperationContext::~ScopedOperationContext */
    ScopedOperationContext::~ScopedOperationContext() {
        diagnosticBinding_.reset();
        if (pushedOperation_)
            PopOperation();
    }

    /** @copydoc OperationSpan::OperationSpan */
    OperationSpan::OperationSpan(const std::string_view subsystem, const std::string_view name) : subsystem_(subsystem), name_(name) {
        const OperationContext inherited = CaptureOperationContext();
        context_.operationId = NextOperationId();
        context_.parentOperationId = inherited.operationId;
        context_.diagnosticContext = inherited.diagnosticContext.With("operation.id", std::to_string(context_.operationId));
        if (context_.parentOperationId != 0)
            context_.diagnosticContext = context_.diagnosticContext.With("operation.parent_id", std::to_string(context_.parentOperationId));
        if (const auto correlation =
                std::ranges::find(context_.diagnosticContext.Fields(), std::string_view{"correlation.id"}, &Log::MdcField::first);
            correlation == context_.diagnosticContext.Fields().end())
            context_.diagnosticContext = context_.diagnosticContext.With("correlation.id", std::to_string(context_.operationId));
        binding_.emplace(context_);
    }

    /** @copydoc OperationSpan::~OperationSpan */
    OperationSpan::~OperationSpan() {
        if (!completed_) {
            const Field field{.key = "completion.reason", .value = std::string{"scope_abandoned"}};
            static_cast<void>(Complete(SpanStatus::Cancelled, std::span<const Field>{&field, 1}));
        }
    }

    /** @copydoc OperationSpan::Id */
    OperationId OperationSpan::Id() const noexcept {
        return context_.operationId;
    }

    /** @copydoc OperationSpan::ParentId */
    OperationId OperationSpan::ParentId() const noexcept {
        return context_.parentOperationId;
    }

    /** @copydoc OperationSpan::Context */
    const OperationContext &OperationSpan::Context() const noexcept {
        return context_;
    }

    /** @copydoc OperationSpan::Complete */
    bool OperationSpan::Complete(const SpanStatus status, const std::span<const Field> fields) noexcept {
        if (completed_ || !IsTerminalStatus(status))
            return false;
        completed_ = true;
        binding_.reset();
        try {
            Record record{.subsystem = subsystem_,
                          .context = context_.diagnosticContext,
                          .payload = SpanRecord{.operationId = context_.operationId,
                                                .parentOperationId = context_.parentOperationId,
                                                .name = name_,
                                                .status = status,
                                                .duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - startedAt_),
                                                .fields = {fields.begin(), fields.end()}}};
            static_cast<void>(Runtime::EmitRecord(std::move(record)));
        } catch (const std::exception &exception) {  // NOSONAR(cpp:S2486) Telemetry must not alter the authoritative lifecycle result.
            static_cast<void>(exception);
            // Lifecycle completion remains authoritative even if diagnostic record construction fails.
        } catch (...) {  // NOSONAR(cpp:S2486)
            // Lifecycle completion remains authoritative even if diagnostic record construction fails.
        }
        return true;
    }

}  // namespace Horo::Telemetry
