#include "WorldSourceValidation.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace Horo::Editor::Detail {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Preserves the declared editor boundary error without publishing a candidate. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }
    }  // namespace

    /** @copydoc ValidateWorldSourceSuccessors */
    Result<void> ValidateWorldSourceSuccessors(const PartitionedWorldDocument &document, const PartitionedWorldDocument &previous) {
        for (const auto &source : document.sources) {
            const auto old = std::ranges::find(previous.sources, source.id, &PartitionedWorldSource::id);
            if (old != previous.sources.end() && *old != source && source.revision <= old->revision)
                return Failure<void>(WorldDocumentErrors::Stale);
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateWorldDocumentSources */
    Result<void> ValidateWorldDocumentSources(const PartitionedWorldDocument &document, const WorldDocumentLimits &limits) {
        if (document.sources.size() > limits.maximumSources)
            return Failure<void>(WorldDocumentErrors::Capacity);
        std::set<WS::StreamingSourceId> sourceIds;
        for (const auto &source : document.sources) {
            if (!source.id.IsValid() || !source.revision.IsValid() || !source.priority.IsValid() ||
                std::ranges::any_of(source.debugColor, [](float value) {
                return !std::isfinite(value) || value < 0 || value > 1;
            }))
                return Failure<void>(WorldDocumentErrors::Invalid);
            if (source.intent >= WS::StreamingSourceIntent::Count || source.target > WS::StreamingDesiredResidency::Activated)
                return Failure<void>(WorldDocumentErrors::Unsupported);
            if (!sourceIds.insert(source.id).second)
                return Failure<void>(WorldDocumentErrors::Conflict);
            if (const auto valid = WS::ValidateStreamingSourceShape(source.shape); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Editor::Detail
