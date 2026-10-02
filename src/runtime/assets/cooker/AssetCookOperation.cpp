/**
 * @file
 * @brief Projects committed cook outcomes while containing optional history notification failures.
 */

#include "AssetCookOperation.h"

#include "../AssetErrors.h"

#include <chrono>
#include <exception>
#include <string_view>
#include <utility>

namespace Horo::Assets {

    /** @copydoc AssetCookOperation::AssetCookOperation */
    AssetCookOperation::AssetCookOperation(OperationStore *store, BuildOutputStore *output, const CancellationToken &cancellation,
                                           const BuildOutputSessionId sessionId, const std::optional<OperationId> id)
        : store_(store), output_(output), cancellation_(&cancellation), sessionId_(sessionId), id_(id) {}

    /** @copydoc AssetCookOperation::~AssetCookOperation */
    AssetCookOperation::~AssetCookOperation() noexcept {
        try {
            FinalizeProjection();
        } catch (const std::exception &) {
            // Optional notification failure cannot change disk commit truth or escape operation cleanup.
            return;
        } catch (...) {
            // Foreign history sinks can throw nonstandard exceptions; retain the same cleanup guarantee.
            return;
        }
    }

    /** @copydoc AssetCookOperation::RecordOutcome */
    void AssetCookOperation::RecordOutcome(const bool cancelled) {
        cancelledResult_ = cancelled;
    }

    /** @copydoc AssetCookOperation::RecordCommitted */
    void AssetCookOperation::RecordCommitted() noexcept {
        committed_ = true;
    }

    /** @copydoc AssetCookOperation::RecordError */
    void AssetCookOperation::RecordError(const Error &error) {
        const std::string_view domain = error.domain.Value();
        cancelledResult_ =
            error.code.Value() == CookErrors::Cancelled.code.Value() && (domain.empty() || domain == CookErrors::Cancelled.domain.Value());
    }

    /** @copydoc AssetCookOperation::Update */
    void AssetCookOperation::Update(std::string phase, std::string message, const float progress) {
        if (store_ != nullptr && id_.has_value())
            static_cast<void>(store_->Update(*id_, OperationUpdate{.state = OperationState::Running,
                                                                   .phase = std::move(phase),
                                                                   .message = std::move(message),
                                                                   .progress = progress}));
    }

    /** @copydoc AssetCookOperation::Succeed */
    void AssetCookOperation::Succeed(std::string message) noexcept {
        try {
            Publish(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                      .result = BuildOutputResult::Succeeded,
                                      .stage = "complete",
                                      .code = DiagnosticCode{"asset.cook.succeeded"},
                                      .message = message});
        } catch (const std::exception &) {
            // Optional output allocation failure must not suppress the authoritative success projection.
            CompleteProjection(std::move(message));
            return;
        } catch (...) {
            // Foreign notification failures cannot reverse a committed generation.
            CompleteProjection(std::move(message));
            return;
        }
        CompleteProjection(std::move(message));
    }

    /** @copydoc AssetCookOperation::CompleteProjection */
    void AssetCookOperation::CompleteProjection(std::string message) noexcept {
        completed_ = true;
        try {
            if (store_ != nullptr && id_.has_value())
                static_cast<void>(store_->Update(*id_, OperationUpdate{.state = OperationState::Succeeded,
                                                                       .phase = "complete",
                                                                       .message = std::move(message),
                                                                       .progress = 1.0F}));
        } catch (const std::exception &) {
            // Terminal history sinks are optional; disk commit remains the cook result's authority.
            return;
        } catch (...) {
            // Nonstandard sink exceptions receive the same terminal truth guarantee.
            return;
        }
    }

    /** @copydoc AssetCookOperation::Publish */
    void AssetCookOperation::Publish(BuildOutputRecord record) const {
        if (output_ == nullptr)
            return;
        record.sessionId = sessionId_;
        record.operationId = id_;
        output_->Append(std::move(record));
    }

    /** @copydoc AssetCookOperation::FinalizeProjection */
    void AssetCookOperation::FinalizeProjection() {
        if (completed_)
            return;
        if (committed_) {
            Succeed("Cooked generation committed");
            return;
        }
        PublishUncommittedOutcome();
    }

    /** @copydoc AssetCookOperation::PublishUncommittedOutcome */
    void AssetCookOperation::PublishUncommittedOutcome() {
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

}  // namespace Horo::Assets
