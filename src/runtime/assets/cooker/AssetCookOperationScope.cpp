#include "AssetCookOperationScope.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <chrono>
#include <string_view>
#include <utility>

namespace Horo::Assets::Detail {
    /** @copydoc CookOperationScope::CookOperationScope */
    CookOperationScope::CookOperationScope(OperationStore *store, BuildOutputStore *output, const CancellationToken &cancellation,
                                           const BuildOutputSessionId sessionId, const std::optional<OperationId> id)
        : store_(store), output_(output), cancellation_(&cancellation), sessionId_(sessionId), id_(id) {}

    /** @copydoc CookOperationScope::~CookOperationScope */
    CookOperationScope::~CookOperationScope() {
        if (completed_)
            return;
        const bool cancelled = cancelledResult_.value_or(cancellation_->IsCancellationRequested());
        Publish(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                  .severity = cancelled ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
                                  .result = cancelled ? BuildOutputResult::Cancelled : BuildOutputResult::Failed,
                                  .stage = "cook",
                                  .code = DiagnosticCode{cancelled ? "asset.cook.cancelled" : "asset.cook.failed"},
                                  .message = cancelled ? "Cook cancelled" : "Cook failed"});
        if (store_ != nullptr && id_.has_value())
            static_cast<void>(store_->Update(*id_, OperationUpdate{.state = cancelled ? OperationState::Cancelled : OperationState::Failed,
                                                                   .phase = "cook",
                                                                   .message = cancelled ? "Cook cancelled" : "Cook failed"}));
    }

    /** @copydoc CookOperationScope::RecordOutcome */
    void CookOperationScope::RecordOutcome(const bool cancelled) {
        cancelledResult_ = cancelled;
    }

    /** @copydoc CookOperationScope::RecordError */
    void CookOperationScope::RecordError(const Error &error) {
        const std::string_view domain = error.domain.Value();
        cancelledResult_ = IsJobCancelled(error) || (error.code.Value() == CookErrors::Cancelled.code.Value() &&
                                                     (domain.empty() || domain == CookErrors::Cancelled.domain.Value()));
    }

    /** @copydoc CookOperationScope::Update */
    void CookOperationScope::Update(std::string phase, std::string message, const float progress) {
        if (store_ != nullptr && id_.has_value())
            static_cast<void>(store_->Update(*id_, OperationUpdate{.state = OperationState::Running,
                                                                   .phase = std::move(phase),
                                                                   .message = std::move(message),
                                                                   .progress = progress}));
    }

    /** @copydoc CookOperationScope::Succeed */
    void CookOperationScope::Succeed(std::string message) {
        const std::string outputMessage = message;
        Publish(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                  .result = BuildOutputResult::Succeeded,
                                  .stage = "complete",
                                  .code = DiagnosticCode{"asset.cook.succeeded"},
                                  .message = outputMessage});
        if (store_ != nullptr && id_.has_value())
            static_cast<void>(store_->Update(*id_, OperationUpdate{.state = OperationState::Succeeded,
                                                                   .phase = "complete",
                                                                   .message = std::move(message),
                                                                   .progress = 1.0F}));
        completed_ = true;
    }

    /** @copydoc CookOperationScope::Publish */
    void CookOperationScope::Publish(BuildOutputRecord record) const {
        if (output_ == nullptr)
            return;
        record.sessionId = sessionId_;
        record.operationId = id_;
        output_->Append(std::move(record));
    }
}  // namespace Horo::Assets::Detail
