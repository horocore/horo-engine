#include "Horo/AI/AIErrors.h"
#include "Horo/AI/BlackboardInstance.h"

#include <new>
#include <utility>

namespace Horo::AI {
    /** @copydoc BlackboardInstance::CreateFromCanonical */
    Result<std::unique_ptr<BlackboardInstance>> BlackboardInstance::CreateFromCanonical(const BlackboardInstanceBinding &binding,
                                                                                        std::shared_ptr<const BlackboardSchema> schema,
                                                                                        const BlackboardCanonicalState &state) {
        if (!schema || !binding.IsValid() || binding.schema != schema->Identity() || binding.schemaVersion != schema->Version())
            return Result<std::unique_ptr<BlackboardInstance>>::Failure(MakeError(AIErrors::BlackboardInstanceInvalid));
        if (const auto valid = ValidateCanonicalBlackboard(state, *schema); valid.HasError())
            return Result<std::unique_ptr<BlackboardInstance>>::Failure(valid.ErrorValue());
        try {
            std::vector<std::optional<BlackboardValue>> values;
            values.reserve(state.entries.size());
            for (const auto &entry : state.entries)
                values.push_back(entry.value);
            auto scratch = values;
            auto generation = std::make_shared<std::atomic_bool>(true);
            const ConstructionKey key;
            return Result<std::unique_ptr<BlackboardInstance>>::Success(
                std::make_unique<BlackboardInstance>(key, binding, std::move(schema), std::move(values), std::move(scratch),
                                                     std::move(generation)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<BlackboardInstance>>::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardInstance::CaptureCanonical */
    Result<BlackboardCanonicalState> BlackboardInstance::CaptureCanonical() const {
        if (!active_)
            return Result<BlackboardCanonicalState>::Failure(MakeError(AIErrors::BlackboardInstanceStale));
        try {
            BlackboardCanonicalState state{.schema = schema_->Identity(), .schemaVersion = schema_->Version()};
            state.entries.reserve(values_.size());
            for (std::size_t index = 0; index < values_.size(); ++index)
                state.entries.push_back({schema_->Keys()[index].key, values_[index]});
            return Result<BlackboardCanonicalState>::Success(std::move(state));
        } catch (const std::bad_alloc &) {
            return Result<BlackboardCanonicalState>::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }
}  // namespace Horo::AI
