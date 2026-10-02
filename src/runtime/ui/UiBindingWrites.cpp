#include "UiBindingStoreInternal.h"

namespace Horo::Runtime::Ui {
    using BindingStoreInternal::Failure;

    namespace {
        /** @brief Copies the supported control vocabulary into already reserved target storage. */
        [[nodiscard]] Result<void> CopyDraft(UiBindingValue &draft, const UiActionValue &value) {
            return std::visit([&value]<typename Value>(Value &stored) {
                if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, double>) {
                    const auto *input = std::get_if<Value>(&value);
                    if (!input)
                        return Failure(UiErrors::BindingTypeMismatch);
                    stored = *input;
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    const auto *input = std::get_if<UiActionText>(&value);
                    if (!input || !input->IsValid())
                        return Failure(UiErrors::BindingTypeMismatch);
                    if (input->size > stored.capacity())
                        return Failure(UiErrors::BindingValueInvalid);
                    stored.assign(input->View());
                } else {
                    return Failure(UiErrors::BindingTypeMismatch);
                }
                return Result<void>::Success();
            }, draft);
        }

        /** @brief Reads typed action identity/payload without reflection or provider lookup. */
        [[nodiscard]] const UiActionPayload *WritePayload(const UiActionRequest &request, const UiActionId action) noexcept {
            if (const auto *command = std::get_if<UiGameplayActionCommand>(&request.command))
                return command->action == action ? &command->payload : nullptr;
            if (const auto *command = std::get_if<UiFormActionCommand>(&request.command))
                return command->action == action ? &command->payload : nullptr;
            return nullptr;
        }

        /** @brief Copies terminal evidence before releasing a reserved command slot. */
        [[nodiscard]] UiBindingWriteResult Outcome(const UiBindingWriteCommand &command, const UiBindingWriteDisposition disposition,
                                                   std::optional<Error> error = {}) {
            return {command.request, command.operation, disposition, std::move(error)};
        }

        /** @brief Compares the complete bounded action payload, including authored prefix arguments. */
        [[nodiscard]] bool EqualValue(const UiActionValue &left, const UiActionValue &right) noexcept {
            if (left.index() != right.index())
                return false;
            return std::visit([&right]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, UiActionText>)
                    return value.View() == std::get<Value>(right).View();
                else
                    return value == std::get<Value>(right);
            }, left);
        }

        /** @brief Keeps callback reentry fenced through validation, reservation publication and terminal abandonment. */
        struct ProcessingScope final {
            bool &processing;
            bool previous;

            explicit ProcessingScope(bool &value) noexcept : processing(value), previous(std::exchange(value, true)) {}

            ~ProcessingScope() {
                processing = previous;
            }

            ProcessingScope(const ProcessingScope &) = delete;
            ProcessingScope &operator=(const ProcessingScope &) = delete;
            ProcessingScope(ProcessingScope &&) = delete;
            ProcessingScope &operator=(ProcessingScope &&) = delete;
        };
    }  // namespace

    /** @copydoc UiBindingStore::Storage::NextWrite */
    UiBindingStore::Storage::Target *UiBindingStore::Storage::NextWrite() noexcept {
        for (std::size_t offset = 0; offset < targets.size(); ++offset) {
            const auto index = (writeCursor + offset) % targets.size();
            auto &target = targets[index];
            if (target.command && !target.outcome) {
                writeCursor = (index + 1) % targets.size();
                return &target;
            }
        }
        return nullptr;
    }

    /** @copydoc UiBindingStore::Storage::CancelWrite */
    void UiBindingStore::Storage::CancelWrite(Target &target, const UiBindingWriteCancellationReason reason) noexcept {
        if (target.command && !target.outcome) {
            target.outcome = Outcome(*target.command, UiBindingWriteDisposition::Cancelled);
            target.outcome->cancellation = reason;
            const bool wasProcessing = processingWrite;
            processingWrite = true;
            target.admission->authority->Abandon(*target.command);
            processingWrite = wasProcessing;
        }
        target.edit = {};
    }

    /** @copydoc UiBindingStore::QueueWrite */
    Result<UiBindingWriteResult> UiBindingStore::QueueWrite(const UiElementTree &tree, const UiBindingEditId &edit,
                                                            const UiActionRequest &request, const UiBindingCommitTrigger trigger) {
        if (!storage_ || !storage_->active || storage_->processingWrite)
            return Failure<UiBindingWriteResult>(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = request.Validate(); valid.HasError())
            return Result<UiBindingWriteResult>::Failure(valid.ErrorValue());
        const auto found = std::ranges::find(storage_->targets, edit, &Storage::Target::edit);
        if (edit.sequence == 0 || found == storage_->targets.end() || found->command)
            return Failure<UiBindingWriteResult>(UiErrors::RevisionStale);
        auto &target = *found;
        if (const auto valid = storage_->ValidateWriteSource(tree, target, request.source); valid.HasError())
            return Result<UiBindingWriteResult>::Failure(valid.ErrorValue());
        if (request.source != target.source || trigger != target.admission->trigger)
            return Failure<UiBindingWriteResult>(UiErrors::BindingAccessInvalid);
        if (request.id.ownership != request.source.owner.instance.ownership || request.id.sequence <= target.lastRequest)
            return Failure<UiBindingWriteResult>(UiErrors::RevisionStale);
        const auto &provider = storage_->providers[target.provider];
        if (!provider.active || provider.writesRevoked || !target.admission->authority->Active())
            return Failure<UiBindingWriteResult>(UiErrors::BindingLifecycleUnavailable);
        if (provider.revision != target.expected || target.admission->authority->Fence() != target.fence)
            return Failure<UiBindingWriteResult>(UiErrors::RevisionStale);
        const auto *payload = WritePayload(request, target.admission->action);
        if (!payload || payload->Size() == 0)
            return Failure<UiBindingWriteResult>(UiErrors::BindingAccessInvalid);
        const auto &value = payload->Values().back();
        if (const auto copied = CopyDraft(target.draft, value); copied.HasError())
            return Result<UiBindingWriteResult>::Failure(copied.ErrorValue());
        const auto &property = provider.schema.Properties()[target.property];
        if (const auto valid = BindingInternal::ValidateValue(target.draft, property.type, property.limits); valid.HasError())
            return Result<UiBindingWriteResult>::Failure(valid.ErrorValue());
        if (const auto valid = BindingInternal::ValidateValue(target.draft, property.type, target.limits); valid.HasError())
            return Result<UiBindingWriteResult>::Failure(valid.ErrorValue());
        const UiActionOperationId operation{edit.ownership, UiActionOperationSequence::Create(edit.sequence).Value()};
        target.command = {target.fence, target.bound.binding, request.source, request.id, operation, target.expected, trigger, value};
        target.lastRequest = request.id.sequence;
        target.edit = {};
        return Result<UiBindingWriteResult>::Success(Outcome(*target.command, UiBindingWriteDisposition::Pending));
    }

    /** @copydoc UiBindingStore::QueueControlDefault */
    Result<UiBindingWriteResult> UiBindingStore::QueueControlDefault(const UiElementTree &tree, const UiBindingEditId &edit,
                                                                     UiControlStateMachine &control, const UiActionRequest &request) {
        const auto preview = control.PeekDefault();
        if (preview.HasError())
            return Result<UiBindingWriteResult>::Failure(preview.ErrorValue());
        if (!preview.Value())
            return Failure<UiBindingWriteResult>(UiErrors::BindingAccessInvalid);
        const auto &action = *preview.Value();
        const auto refuse = [&control](const ErrorCodeDescriptor &error) {
            const auto suppressed = control.SuppressDefault();
            (void)suppressed;
            return Failure<UiBindingWriteResult>(error);
        };
        if (action.kind == UiControlActionKind::Activate || action.kind >= UiControlActionKind::Count)
            return refuse(UiErrors::BindingAccessInvalid);
        const auto *payload = WritePayload(request, action.action);
        if (!payload || action.source != request.source || payload->Size() != action.payload.Size())
            return refuse(UiErrors::BindingAccessInvalid);
        for (std::size_t index = 0; index < payload->Size(); ++index) {
            if (!EqualValue(payload->Values()[index], action.payload.Values()[index]))
                return refuse(UiErrors::BindingValueInvalid);
        }
        const auto trigger = action.kind == UiControlActionKind::Submit ? UiBindingCommitTrigger::Submit : UiBindingCommitTrigger::Change;
        auto queued = QueueWrite(tree, edit, request, trigger);
        if (queued.HasError()) {
            const auto suppressed = control.SuppressDefault();
            (void)suppressed;
            return queued;
        }
        if (const auto applied = control.ApplyDefault(); applied.HasError())
            return Result<UiBindingWriteResult>::Failure(applied.ErrorValue());
        return queued;
    }

    /** @copydoc UiBindingStore::Storage::PublishWrite */
    Result<void> UiBindingStore::Storage::PublishWrite(const UiElementTree &tree, Target &target, UiLayoutEngine &layout) {
        auto &provider = providers[target.provider];
        const auto next = provider.revision.Next();
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        const StagingScope staging{*this};
        // Validation and staging use the already reserved draft, so text never allocates in the commit path.
        for (const auto index : provider.targets[target.property]) {
            if (targets[index].direction == UiBindingDirection::TargetToSource && &targets[index] != &target)
                continue;
            if (const auto result = Stage(index, &target.draft, UiBindingValueOrigin::Provider); result.HasError())
                return result;
        }
        if (const auto published = Publish(tree, layout); published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        target.admission->authority->Commit(*target.command);
        provider.revision = next.Value();
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::ProcessWrite */
    Result<std::optional<UiBindingWriteResult>> UiBindingStore::ProcessWrite(const UiElementTree &tree, UiLayoutEngine &layout) {
        if (!storage_ || storage_->processingWrite)
            return Failure<std::optional<UiBindingWriteResult>>(UiErrors::BindingLifecycleUnavailable);
        auto *found = storage_->NextWrite();
        if (!found)
            return Result<std::optional<UiBindingWriteResult>>::Success({});
        auto &target = *found;
        auto &authority = *target.admission->authority;

        const ProcessingScope processing{storage_->processingWrite};

        storage_->reentryAttempted = false;
        const auto finish = [&](const UiBindingWriteDisposition disposition, std::optional<Error> error = {},
                                const UiBindingWriteCancellationReason reason = UiBindingWriteCancellationReason::ProviderCancelled) {
            target.outcome = Outcome(*target.command, disposition, std::move(error));
            if (disposition == UiBindingWriteDisposition::Cancelled)
                target.outcome->cancellation = reason;
            if (disposition != UiBindingWriteDisposition::Ready)
                authority.Abandon(*target.command);
            return Result<std::optional<UiBindingWriteResult>>::Success(target.outcome);
        };
        if (!storage_->active || !storage_->providers[target.provider].active || storage_->providers[target.provider].writesRevoked ||
            !authority.Active())
            return finish(UiBindingWriteDisposition::Cancelled, {}, UiBindingWriteCancellationReason::ProviderUnavailable);
        if (const auto valid = storage_->ValidateWriteSource(tree, target, target.command->source); valid.HasError())
            return finish(UiBindingWriteDisposition::Rejected, valid.ErrorValue());
        if (authority.Fence() != target.command->fence || storage_->providers[target.provider].revision != target.command->expected)
            return finish(UiBindingWriteDisposition::Rejected, MakeError(UiErrors::RevisionStale));

        const auto prepared = authority.Prepare(*target.command);
        if (storage_->reentryAttempted)
            return finish(UiBindingWriteDisposition::Rejected, MakeError(UiErrors::ActionHandlerFailed));
        if (prepared.HasError())
            return finish(UiBindingWriteDisposition::Rejected, prepared.ErrorValue());
        if (prepared.Value() == UiBindingWriteDisposition::Pending)
            return Result<std::optional<UiBindingWriteResult>>::Success(Outcome(*target.command, UiBindingWriteDisposition::Pending));
        if (prepared.Value() == UiBindingWriteDisposition::Rejected || prepared.Value() == UiBindingWriteDisposition::Cancelled)
            return finish(prepared.Value());
        if (prepared.Value() != UiBindingWriteDisposition::Ready)
            return finish(UiBindingWriteDisposition::Rejected, MakeError(UiErrors::BindingValueInvalid));
        if (!authority.Active())
            return finish(UiBindingWriteDisposition::Cancelled, {}, UiBindingWriteCancellationReason::ProviderUnavailable);
        if (const auto published = storage_->PublishWrite(tree, target, layout); published.HasError())
            return finish(UiBindingWriteDisposition::Rejected, published.ErrorValue());
        return finish(UiBindingWriteDisposition::Ready);
    }

    /** @copydoc UiBindingStore::DrainWriteResults */
    Result<std::size_t> UiBindingStore::DrainWriteResults(const std::span<UiBindingWriteResult> output) {
        if (!storage_ || storage_->processingWrite)
            return Failure<std::size_t>(UiErrors::BindingLifecycleUnavailable);
        const auto count = static_cast<std::size_t>(std::ranges::count_if(storage_->targets, [](const Storage::Target &target) {
            return target.outcome.has_value();
        }));
        if (output.size() < count)
            return Failure<std::size_t>(UiErrors::BindingCapacityExceeded);
        std::size_t index = 0;
        for (auto &target : storage_->targets)
            if (target.outcome) {
                output[index++] = std::move(*target.outcome);
                target.outcome.reset();
                target.command.reset();
            }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc UiBindingStore::ReconcileControl */
    Result<void> UiBindingStore::ReconcileControl(const UiElementTree &tree, const UiBindingId binding,
                                                  UiControlStateMachine &control) const {
        if (!storage_)
            return Failure(UiErrors::BindingLifecycleUnavailable);
        const auto *target = storage_->FindTarget(binding);
        if (!target || !target->admission)
            return Failure(UiErrors::BindingAccessInvalid);
        if (target->bound.origin == UiBindingValueOrigin::Unavailable)
            return Failure(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateWriteSource(tree, *target, {control.Owner(), control.Element()}); valid.HasError())
            return valid;
        if (target->command && !target->outcome)
            return Failure(UiErrors::BindingDescriptorConflict);
        if (const auto *value = std::get_if<bool>(&target->bound.value))
            return control.ReconcileValue(*value);
        if (const auto *value = std::get_if<double>(&target->bound.value))
            return control.ReconcileValue(*value);
        if (const auto *value = std::get_if<std::string>(&target->bound.value)) {
            const auto text = UiActionText::Create(*value);
            return text.HasError() ? Result<void>::Failure(text.ErrorValue()) : control.ReconcileValue(text.Value());
        }
        return Failure(UiErrors::BindingTypeMismatch);
    }

    /** @copydoc UiBindingWriteActionHandler::UiBindingWriteActionHandler */
    UiBindingWriteActionHandler::UiBindingWriteActionHandler(UiBindingStore &store, const UiElementTree &tree, const UiBindingEditId &edit,
                                                             const UiBindingCommitTrigger trigger) noexcept
        : store_(store), tree_(tree), edit_(edit), trigger_(trigger) {}

    /** @copydoc UiBindingWriteActionHandler::UiBindingWriteActionHandler */
    UiBindingWriteActionHandler::UiBindingWriteActionHandler(UiBindingStore &store, const UiElementTree &tree, const UiBindingEditId &edit,
                                                             UiControlStateMachine &control) noexcept
        : store_(store), tree_(tree), edit_(edit), trigger_(UiBindingCommitTrigger::Count), control_(&control) {}

    /** @copydoc UiBindingWriteActionHandler::Handle */
    Result<UiActionResult> UiBindingWriteActionHandler::Handle(const UiActionRequest &request) {
        const auto queued =
            control_ ? store_.QueueControlDefault(tree_, edit_, *control_, request) : store_.QueueWrite(tree_, edit_, request, trigger_);
        if (queued.HasError())
            return Result<UiActionResult>::Failure(queued.ErrorValue());
        return UiActionResult::Pending(queued.Value().request, queued.Value().operation);
    }
}  // namespace Horo::Runtime::Ui
