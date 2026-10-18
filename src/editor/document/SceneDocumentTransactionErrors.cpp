#include "Horo/Editor/SceneDocumentTransaction.h"

#include <array>

namespace Horo::Editor {
    /** @copydoc SceneTransactionError */
    const ErrorCodeDescriptor &SceneTransactionError(const SceneTransactionFailure reason) noexcept {
        using enum ErrorSeverity;
        static const ErrorDomainId domain{"horo.editor.scene_transaction"};
        static const std::array<ErrorCodeDescriptor, 7> errors{
            {{domain, ErrorCode{"scene_transaction.closed"}, Error, "Scene transaction admission is closed.",
              "Open a fresh document transaction port.", false, true},
             {domain, ErrorCode{"scene_transaction.wrong_thread"}, Error, "Scene transaction requires its owner thread.",
              "Return to the document owner phase.", false, false},
             {domain, ErrorCode{"scene_transaction.stale"}, Error, "Scene transaction evidence is stale or conflicts.",
              "Capture current document and ownership evidence.", false, true},
             {domain, ErrorCode{"scene_transaction.unauthorized"}, Error, "Scene editing permission was not granted.",
              "Obtain explicit document edit authority.", false, true},
             {domain, ErrorCode{"scene_transaction.cancelled"}, Error, "Scene transaction was cancelled.",
              "Preserve the prior authored state.", false, false},
             {domain, ErrorCode{"scene_transaction.invalid"}, Error, "Scene transaction is malformed.",
              "Supply complete finite typed replacements.", false, true},
             {domain, ErrorCode{"scene_transaction.capacity"}, Error, "Scene transaction exceeds its bounded reservation.",
              "Reduce the transaction or raise the host budget within safety bounds.", false, true}}};
        const auto index = static_cast<std::size_t>(reason);
        return errors[index < errors.size() ? index : static_cast<std::size_t>(SceneTransactionFailure::Invalid)];
    }

}  // namespace Horo::Editor
