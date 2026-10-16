#include "Horo/AI/BlackboardInstance.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        using BlackboardValueStorage = std::vector<std::optional<BlackboardValue>>;
        static_assert(std::is_nothrow_copy_assignable_v<std::optional<BlackboardValue>>);
        static_assert(std::is_nothrow_move_assignable_v<BlackboardValue>);

        [[nodiscard]] Error Failure(const ErrorCodeDescriptor &descriptor) {
            return MakeError(descriptor);
        }

        [[nodiscard]] std::size_t FindKey(const BlackboardSchema &schema, const BlackboardKeyId key) noexcept {
            const auto keys = schema.Keys();
            const auto found = std::ranges::lower_bound(keys, key, {}, &BlackboardKeyDescriptor::key);
            return found != keys.end() && found->key == key ? static_cast<std::size_t>(found - keys.begin()) : keys.size();
        }

        [[nodiscard]] Result<BlackboardValueStorage> BuildDefaultValues(const BlackboardSchema &schema) {
            try {
                BlackboardValueStorage values;
                values.reserve(schema.Keys().size());
                for (const auto &key : schema.Keys()) {
                    if (key.presence == BlackboardKeyPresence::Required && !key.defaultValue.has_value())
                        return Result<BlackboardValueStorage>::Failure(Failure(AIErrors::BlackboardInstanceInvalid));
                    values.push_back(key.defaultValue);
                }
                return Result<BlackboardValueStorage>::Success(std::move(values));
            } catch (const std::bad_alloc &) {
                return Result<BlackboardValueStorage>::Failure(Failure(AIErrors::BlackboardStorageUnavailable));
            }
        }

        [[nodiscard]] Result<BlackboardValueStorage> BuildReplacementValues(const BlackboardSchema &currentSchema,
                                                                            const BlackboardValueStorage &currentValues,
                                                                            const BlackboardSchema &replacementSchema) {
            auto candidate = BuildDefaultValues(replacementSchema);
            if (candidate.HasError())
                return candidate;
            auto values = std::move(candidate).Value();
            for (std::size_t index = 0; index < replacementSchema.Keys().size(); ++index) {
                const auto oldIndex = FindKey(currentSchema, replacementSchema.Keys()[index].key);
                if (oldIndex == currentSchema.Keys().size() || !currentValues[oldIndex].has_value())
                    continue;
                if (ValidateBlackboardValue(*currentValues[oldIndex], &replacementSchema.Keys()[index],
                                            replacementSchema.UnknownValuePolicy())
                        .HasError())
                    return Result<BlackboardValueStorage>::Failure(Failure(AIErrors::BlackboardValueTypeMismatch));
                values[index] = currentValues[oldIndex];
            }
            return Result<BlackboardValueStorage>::Success(std::move(values));
        }

        [[nodiscard]] Result<bool> ApplyWrite(const BlackboardSchema &schema, BlackboardValueStorage &values,
                                              const BlackboardWrite &write) {
            const std::size_t index = FindKey(schema, write.key);
            if (index == schema.Keys().size())
                return Result<bool>::Failure(Failure(AIErrors::BlackboardUnknownValueRejected));
            if (schema.Keys()[index].access == BlackboardKeyAccess::ReadOnly)
                return Result<bool>::Failure(Failure(AIErrors::BlackboardBatchInvalid));
            if (const auto valid = ValidateBlackboardValue(write.value, &schema.Keys()[index], schema.UnknownValuePolicy());
                valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
            if (values[index].has_value() && *values[index] == write.value)
                return Result<bool>::Success(false);
            values[index] = write.value;
            return Result<bool>::Success(true);
        }

        /** @brief Publishes one changed, completely validated candidate and advances its revision exactly once. */
        [[nodiscard]] Result<BlackboardCommitResult> PublishChangedValues(BlackboardValueStorage &activeValues,
                                                                          BlackboardValueStorage &candidateValues,
                                                                          BlackboardCommitResult result, std::uint64_t &revision) {
            if (result.changedKeyCount == 0)
                return Result<BlackboardCommitResult>::Success(std::move(result));
            if (revision == std::numeric_limits<std::uint64_t>::max())
                return Result<BlackboardCommitResult>::Failure(Failure(AIErrors::BlackboardRevisionExhausted));
            activeValues.swap(candidateValues);
            result.revision = ++revision;
            return Result<BlackboardCommitResult>::Success(std::move(result));
        }

        [[nodiscard]] bool IsCompatibleReplacement(const BlackboardInstanceBinding &current, const BlackboardInstanceBinding &replacement,
                                                   const std::shared_ptr<const BlackboardSchema> &schema) noexcept {
            return schema != nullptr && replacement.IsValid() && replacement.agent == current.agent &&
                   replacement.schema == schema->Identity() && replacement.schemaVersion == schema->Version() &&
                   replacement.schemaGeneration > current.schemaGeneration &&
                   current.instanceGeneration != std::numeric_limits<std::uint32_t>::max() &&
                   replacement.instanceGeneration == current.instanceGeneration + 1;
        }
    }  // namespace

    /** @copydoc BlackboardInstanceBinding::IsValid */
    bool BlackboardInstanceBinding::IsValid() const noexcept {
        return agent.IsValid() && schema.IsValid() && schemaVersion != 0 && schemaGeneration != 0 && instanceGeneration != 0;
    }

    /** @copydoc BlackboardWriteBatch::Stage */
    Result<void> BlackboardWriteBatch::Stage(BlackboardWrite write) {
        if (!binding_.IsValid() || !write.key.IsValid())
            return Result<void>::Failure(Failure(AIErrors::BlackboardBatchInvalid));
        if (const auto duplicate = std::ranges::find(writes_, write.key, &BlackboardWrite::key); duplicate != writes_.end()) {
            duplicate->value = std::move(write.value);
            duplicate->authorization = std::move(write.authorization);
            return Result<void>::Success();
        }
        if (writes_.size() >= MaximumBlackboardWritesPerBatch)
            return Result<void>::Failure(Failure(AIErrors::BlackboardLimitExceeded));
        try {
            writes_.push_back(std::move(write));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(Failure(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardSnapshot::Revision */
    Result<std::uint64_t> BlackboardSnapshot::Revision() const {
        if (!generationActive_->load())
            return Result<std::uint64_t>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        return Result<std::uint64_t>::Success(revision_);
    }

    /** @copydoc BlackboardSnapshot::Read */
    Result<std::optional<BlackboardValue>> BlackboardSnapshot::Read(const BlackboardKeyId key) const {
        if (!generationActive_->load())
            return Result<std::optional<BlackboardValue>>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        const std::size_t index = FindKey(*schema_, key);
        if (index == schema_->Keys().size())
            return Result<std::optional<BlackboardValue>>::Failure(Failure(AIErrors::BlackboardUnknownValueRejected));
        if (authorizations_[index] && !authorizations_[index]->IsCurrent())
            return Result<std::optional<BlackboardValue>>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        return Result<std::optional<BlackboardValue>>::Success(values_[index]);
    }

    /** @copydoc BlackboardInstance::Create */
    Result<std::unique_ptr<BlackboardInstance>> BlackboardInstance::Create(const BlackboardInstanceBinding &binding,
                                                                           std::shared_ptr<const BlackboardSchema> schema) {
        if (!binding.IsValid() || schema == nullptr || binding.schema != schema->Identity() || binding.schemaVersion != schema->Version())
            return Result<std::unique_ptr<BlackboardInstance>>::Failure(Failure(AIErrors::BlackboardInstanceInvalid));
        auto values = BuildDefaultValues(*schema);
        if (values.HasError())
            return Result<std::unique_ptr<BlackboardInstance>>::Failure(values.ErrorValue());
        try {
            auto generationActive = std::make_shared<std::atomic_bool>(true);
            auto scratch = values.Value();
            const ConstructionKey key;
            return Result<std::unique_ptr<BlackboardInstance>>::Success(
                std::make_unique<BlackboardInstance>(key, binding, std::move(schema), std::move(values).Value(), std::move(scratch),
                                                     std::move(generationActive)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<BlackboardInstance>>::Failure(Failure(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardInstance::Snapshot */
    Result<BlackboardSnapshot> BlackboardInstance::Snapshot() const {
        if (!active_)
            return Result<BlackboardSnapshot>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        try {
            return Result<BlackboardSnapshot>::Success(
                BlackboardSnapshot{binding_, schema_, values_, revision_, generationActive_, authorizations_});
        } catch (const std::bad_alloc &) {
            return Result<BlackboardSnapshot>::Failure(Failure(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardInstance::ReadAtSafePoint */
    Result<std::optional<BlackboardValue>> BlackboardInstance::ReadAtSafePoint(const BlackboardKeyId key) const {
        if (!active_)
            return Result<std::optional<BlackboardValue>>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        const std::size_t index = FindKey(*schema_, key);
        if (index == schema_->Keys().size())
            return Result<std::optional<BlackboardValue>>::Failure(Failure(AIErrors::BlackboardUnknownValueRejected));
        if (authorizations_[index] && !authorizations_[index]->IsCurrent())
            return Result<std::optional<BlackboardValue>>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        return Result<std::optional<BlackboardValue>>::Success(values_[index]);
    }

    /** @copydoc BlackboardInstance::BeginWriteBatch */
    Result<BlackboardWriteBatch> BlackboardInstance::BeginWriteBatch() const {
        if (publishing_)
            return Result<BlackboardWriteBatch>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_)
            return Result<BlackboardWriteBatch>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        return Result<BlackboardWriteBatch>::Success(BlackboardWriteBatch{binding_, revision_});
    }

    /** @copydoc BlackboardInstance::RegisterObserverAtBlackboardSync */
    Result<BlackboardObserverToken> BlackboardInstance::RegisterObserverAtBlackboardSync(
        const BlackboardObserverRegistration &registration) {
        if (publishing_)
            return Result<BlackboardObserverToken>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_)
            return Result<BlackboardObserverToken>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        if (registration.agent != binding_.agent || registration.callback == nullptr ||
            ValidateAiRuntimeHandle(registration.task, binding_.agent.incarnation).HasError() ||
            FindKey(*schema_, registration.key) == schema_->Keys().size())
            return Result<BlackboardObserverToken>::Failure(Failure(AIErrors::BlackboardObserverInvalid));
        std::size_t available = observers_.size();
        for (std::size_t index = 0; index < observers_.size(); ++index) {
            const auto &slot = observers_[index];
            if (slot.active) {
                if (slot.registration.agent == registration.agent && slot.registration.task == registration.task &&
                    slot.registration.key == registration.key && slot.registration.callback == registration.callback &&
                    slot.registration.context == registration.context)
                    return Result<BlackboardObserverToken>::Failure(Failure(AIErrors::BlackboardObserverInvalid));
                continue;
            }
            if (!slot.retired && available == observers_.size())
                available = index;
        }
        if (available == observers_.size())
            return Result<BlackboardObserverToken>::Failure(Failure(AIErrors::BlackboardObserverLimitExceeded));
        auto &slot = observers_[available];
        slot.registration = registration;
        slot.active = true;
        return Result<BlackboardObserverToken>::Success({binding_, available, slot.generation});
    }

    /** @copydoc BlackboardInstance::RemoveObserverAtBlackboardSync */
    Result<bool> BlackboardInstance::RemoveObserverAtBlackboardSync(const BlackboardObserverToken &token) {
        if (publishing_)
            return Result<bool>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_)
            return Result<bool>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        if (!token.IsValid() || token.binding != binding_)
            return Result<bool>::Failure(Failure(AIErrors::BlackboardObserverInvalid));
        auto &slot = observers_[token.slot];
        if (!slot.active || slot.generation != token.generation)
            return Result<bool>::Success(false);
        ReleaseObserverSlot(slot);
        return Result<bool>::Success(true);
    }

    /** @copydoc BlackboardInstance::CancelTaskObserversAtBlackboardSync */
    Result<std::size_t> BlackboardInstance::CancelTaskObserversAtBlackboardSync(const TaskHandle task) {
        if (publishing_)
            return Result<std::size_t>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_)
            return Result<std::size_t>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        if (ValidateAiRuntimeHandle(task, binding_.agent.incarnation).HasError())
            return Result<std::size_t>::Failure(Failure(AIErrors::HandleInvalid));
        std::size_t removed{};
        for (auto &slot : observers_) {
            if (!slot.active || slot.registration.task != task)
                continue;
            ReleaseObserverSlot(slot);
            ++removed;
        }
        return Result<std::size_t>::Success(removed);
    }

    /** @copydoc BlackboardInstance::CommitAtBlackboardSync */
    Result<BlackboardCommitResult> BlackboardInstance::CommitAtBlackboardSync(BlackboardWriteBatch &&batch) {
        // Transfer the consumption obligation, not vector storage: MSVC Debug allocates
        // an iterator proxy when move-constructing a vector inside this measured boundary.
        struct ConsumeBatch final {
            explicit ConsumeBatch(BlackboardWriteBatch &&input) noexcept : batch(std::move(input)) {}

            ConsumeBatch(const ConsumeBatch &) = delete;
            ConsumeBatch &operator=(const ConsumeBatch &) = delete;
            ConsumeBatch(ConsumeBatch &&) = delete;
            ConsumeBatch &operator=(ConsumeBatch &&) = delete;

            BlackboardWriteBatch &&batch;

            ~ConsumeBatch() {
                batch.binding_ = {};
                batch.baseRevision_ = 0;
                batch.writes_.clear();
            }
        };

        const ConsumeBatch consumed{std::move(batch)};

        if (publishing_)
            return Result<BlackboardCommitResult>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_ || consumed.batch.Binding() != binding_ || consumed.batch.BaseRevision() != revision_)
            return Result<BlackboardCommitResult>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        std::ranges::sort(consumed.batch.writes_, {}, &BlackboardWrite::key);
        scratch_ = values_;
        authorizationScratch_ = authorizations_;
        BlackboardCommitResult result{.revision = revision_};
        for (const auto &write : consumed.batch.Writes()) {
            if (write.authorization && !write.authorization->IsCurrent())
                return Result<BlackboardCommitResult>::Failure(Failure(AIErrors::BlackboardInstanceStale));
            auto applied = ApplyWrite(*schema_, scratch_, write);
            if (applied.HasError())
                return Result<BlackboardCommitResult>::Failure(applied.ErrorValue());
            const auto index = FindKey(*schema_, write.key);
            if (applied.Value() || authorizationScratch_[index] != write.authorization)
                result.changedKeys[result.changedKeyCount++] = write.key;
            authorizationScratch_[index] = write.authorization;
        }
        auto published = PublishChangedValues(values_, scratch_, std::move(result), revision_);
        if (published.HasValue()) {
            authorizations_.swap(authorizationScratch_);
            PublishNotification(published.Value());
        }
        return published;
    }

    /** @copydoc BlackboardInstance::ReplaceAtBlackboardSync */
    Result<void> BlackboardInstance::ReplaceAtBlackboardSync(const BlackboardInstanceBinding &replacementBinding,
                                                             std::shared_ptr<const BlackboardSchema> replacementSchema) {
        if (publishing_)
            return Result<void>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_ || !IsCompatibleReplacement(binding_, replacementBinding, replacementSchema))
            return Result<void>::Failure(Failure(AIErrors::BlackboardInstanceInvalid));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(Failure(AIErrors::BlackboardRevisionExhausted));
        auto replacement = BuildReplacementValues(*schema_, values_, *replacementSchema);
        if (replacement.HasError())
            return Result<void>::Failure(replacement.ErrorValue());
        try {
            auto replacementGeneration = std::make_shared<std::atomic_bool>(true);
            auto replacementScratch = replacement.Value();
            std::vector<std::shared_ptr<const BlackboardValueAuthorization>> replacementAuthorizations(replacementSchema->Keys().size());
            for (std::size_t index = 0; index < replacementSchema->Keys().size(); ++index) {
                const auto oldIndex = FindKey(*schema_, replacementSchema->Keys()[index].key);
                if (oldIndex < authorizations_.size())
                    replacementAuthorizations[index] = authorizations_[oldIndex];
            }
            auto replacementAuthorizationScratch = replacementAuthorizations;
            values_ = std::move(replacement).Value();
            scratch_ = std::move(replacementScratch);
            authorizations_ = std::move(replacementAuthorizations);
            authorizationScratch_ = std::move(replacementAuthorizationScratch);
            schema_ = std::move(replacementSchema);
            InvalidateObservers();
            binding_ = replacementBinding;
            ++revision_;
            generationActive_->store(false);
            generationActive_ = std::move(replacementGeneration);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(Failure(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardInstance::ResetAtBlackboardSync */
    Result<BlackboardCommitResult> BlackboardInstance::ResetAtBlackboardSync() {
        if (publishing_)
            return Result<BlackboardCommitResult>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_)
            return Result<BlackboardCommitResult>::Failure(Failure(AIErrors::BlackboardInstanceStale));
        auto defaults = BuildDefaultValues(*schema_);
        if (defaults.HasError())
            return Result<BlackboardCommitResult>::Failure(defaults.ErrorValue());
        BlackboardCommitResult result{.revision = revision_};
        for (std::size_t index = 0; index < schema_->Keys().size(); ++index) {
            if (values_[index] != defaults.Value()[index] || authorizations_[index])
                result.changedKeys[result.changedKeyCount++] = schema_->Keys()[index].key;
        }
        auto candidateValues = std::move(defaults).Value();
        auto published = PublishChangedValues(values_, candidateValues, std::move(result), revision_);
        if (published.HasValue()) {
            std::ranges::fill(authorizations_, nullptr);
            std::ranges::fill(authorizationScratch_, nullptr);
            PublishNotification(published.Value());
        }
        return published;
    }

    /** @copydoc BlackboardInstance::TeardownAtBlackboardSync */
    Result<void> BlackboardInstance::TeardownAtBlackboardSync() {
        if (publishing_)
            return Result<void>::Failure(Failure(AIErrors::BlackboardReentrantMutation));
        if (!active_)
            return Result<void>::Success();
        active_ = false;
        generationActive_->store(false);
        InvalidateObservers();
        schema_.reset();
        values_.clear();
        scratch_.clear();
        authorizations_.clear();
        authorizationScratch_.clear();
        return Result<void>::Success();
    }

    /** @copydoc BlackboardInstance::ReleaseObserverSlot */
    void BlackboardInstance::ReleaseObserverSlot(ObserverSlot &slot) noexcept {
        slot.active = false;
        slot.registration = {};
        if (slot.generation == std::numeric_limits<std::uint32_t>::max())
            slot.retired = true;
        else
            ++slot.generation;
    }

    /** @copydoc BlackboardInstance::InvalidateObservers */
    void BlackboardInstance::InvalidateObservers() noexcept {
        for (auto &slot : observers_) {
            if (slot.active)
                ReleaseObserverSlot(slot);
        }
    }

    /** @copydoc BlackboardInstance::PublishNotification */
    void BlackboardInstance::PublishNotification(const BlackboardCommitResult &result) noexcept {
        if (result.Changes().empty())
            return;

        struct FrozenObserver final {
            BlackboardObserverCallback callback{};
            void *context{};
        };

        std::array<FrozenObserver, MaximumBlackboardObservers> matching{};
        std::size_t matchingCount{};
        for (const auto &slot : observers_) {
            if (slot.active && std::ranges::binary_search(result.Changes(), slot.registration.key))
                matching[matchingCount++] = {slot.registration.callback, slot.registration.context};
        }
        if (matchingCount == 0)
            return;
        const BlackboardNotificationBatch notification{binding_, result};
        publishing_ = true;
        for (std::size_t index = 0; index < matchingCount; ++index)
            matching[index].callback(matching[index].context, notification);
        publishing_ = false;
    }
}  // namespace Horo::AI
