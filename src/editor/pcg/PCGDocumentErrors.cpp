#include "Horo/Editor/PCGGraphDocument.h"

#include <array>

namespace Horo::Editor {
    namespace {
        using Reason = PCGDocumentCommandFailureReason;
        const ErrorDomainId Domain{"horo.editor.pcg_document"};
        const std::array Errors{
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.command.invalid"}, ErrorSeverity::Error, "Invalid PCG document command.",
                                "Refresh the document projection."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.command.stale"}, ErrorSeverity::Error, "Stale PCG document fence.",
                                "Capture the current session and revision."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.command.unauthorized"}, ErrorSeverity::Error,
                                "PCG mutation capability is unavailable.", "Request explicit authoring authority."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.command.limit"}, ErrorSeverity::Error,
                                "PCG transaction exceeds its finite limits.", "Submit a smaller transaction."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.history.limit"}, ErrorSeverity::Error,
                                "Exact reversible history cannot be reserved.", "Submit a smaller transaction."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.document.closed"}, ErrorSeverity::Error, "PCG document admission is closed.",
                                "Open a new document session."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.thread"}, ErrorSeverity::Error, "PCG document used from a non-owner thread.",
                                "Queue the operation on the owner thread."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.cancelled"}, ErrorSeverity::Error, "PCG document operation was cancelled.",
                                "Submit a new operation."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.dirty"}, ErrorSeverity::Error, "PCG document contains unsaved edits.",
                                "Save, discard explicitly, or cancel."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.conflict"}, ErrorSeverity::Error,
                                "PCG durable source differs from the accepted base.", "Compare or reload the changed source."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.busy"}, ErrorSeverity::Error, "PCG source publication is active.",
                                "Drain publication before replacement or close."},
            ErrorCodeDescriptor{Domain, ErrorCode{"editor.pcg.exhausted"}, ErrorSeverity::Error, "PCG document revision is exhausted.",
                                "Open a new document session."},
        };

    }  // namespace

    /** @copydoc PCGDocumentError */
    const ErrorCodeDescriptor &PCGDocumentError(Reason reason) noexcept {
        const auto index = static_cast<std::size_t>(reason);
        return Errors[index < Errors.size() ? index : 0];
    }

}  // namespace Horo::Editor
