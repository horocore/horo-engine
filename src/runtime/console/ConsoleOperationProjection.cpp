#include "ConsoleText.h"
#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Debug {
    namespace {
        /** @brief Rejects malformed source identity/lifecycle/progress before any presentation state changes. */
        bool ValidOperation(const OperationRecord &record) {
            return record.id != 0 && static_cast<unsigned>(record.kind) <= static_cast<unsigned>(OperationKind::Other) &&
                   static_cast<unsigned>(record.state) <= static_cast<unsigned>(OperationState::Cancelled) &&
                   (!record.progress || (std::isfinite(*record.progress) && *record.progress >= 0 && *record.progress <= 1));
        }

        /** @brief Produces bounded complete-scalar views while retaining the original immutable operation snapshot. */
        Result<ConsoleOperationRow> ProjectOperation(const std::shared_ptr<const OperationStoreSnapshot> &snapshot,
                                                     const OperationRecord &record) {
            if (!ValidOperation(record))
                return Result<ConsoleOperationRow>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            const std::string_view error = record.error ? std::string_view(record.error->code.Value()) : std::string_view{};
            ConsoleOperationRow row{snapshot,
                                    &record,
                                    Detail::ConsoleTextPrefix(record.title, 512),
                                    Detail::ConsoleTextPrefix(record.phase, 128),
                                    Detail::ConsoleTextPrefix(record.message, 4096),
                                    Detail::ConsoleTextPrefix(error, 256),
                                    false};
            for (const auto text : {row.title, row.phase, row.message, row.errorCode})
                if (!IsValidUtf8ScalarSequence(text))
                    return Result<ConsoleOperationRow>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            row.truncated = row.title.size() != record.title.size() || row.phase.size() != record.phase.size() ||
                            row.message.size() != record.message.size() || row.errorCode.size() != error.size();
            return Result<ConsoleOperationRow>::Success(std::move(row));
        }
    }  // namespace

    /** @copydoc ConsoleStoreOverlays::AcceptJobs */
    Result<void> ConsoleStoreOverlays::AcceptJobs(OperationStoreSnapshot snapshot) {
        if (snapshot.operations.size() > 4096)
            return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
        auto next = std::make_shared<const OperationStoreSnapshot>(std::move(snapshot));
        std::vector<ConsoleOperationRow> rows;
        rows.reserve(next->operations.size());
        std::array<OperationId, 4096> identities{};
        std::size_t count{};
        for (const auto &record : next->operations) {
            auto projected = ProjectOperation(next, record);
            if (projected.HasError())
                return Result<void>::Failure(projected.ErrorValue());
            identities[count++] = record.id;
            rows.push_back(std::move(projected).Value());
        }
        const auto ids = std::span(identities).first(count);
        std::ranges::sort(ids);
        if (std::ranges::adjacent_find(ids) != ids.end())
            return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
        operationRows_.swap(rows);
        operationSnapshot_ = std::move(next);
        ++jobProjectionRevision_;
        return Result<void>::Success();
    }
}  // namespace Horo::Debug
