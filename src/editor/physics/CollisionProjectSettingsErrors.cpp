#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <array>

namespace Horo::Editor {
    namespace {
        /** @brief Builds immutable stable diagnostics; no provider, registry or project discovery. */
        ErrorCodeDescriptor Describe(const char *code, const char *summary, const char *remedy) {
            return {.domain = ErrorDomainId{"horo.editor.collision"},
                    .code = ErrorCode{code},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = remedy,
                    .userActionable = true};
        }

        const std::array Descriptors{Describe("collision.edit.invalid", "The collision edit is invalid.",
                                              "Correct the complete typed schema and review again."),
                                     Describe("collision.session.closed", "The collision settings opening is closed.",
                                              "Reopen settings for the current project."),
                                     Describe("collision.owner.wrong_thread", "The collision editor owner thread is required.",
                                              "Submit the command on the editor owner thread."),
                                     Describe("collision.review.stale", "The reviewed collision sources or owners changed.",
                                              "Refresh inventory and review the complete edit again."),
                                     Describe("collision.definition.in_use", "A collision definition still has authored references.",
                                              "Repair those exact owners before deleting the definition."),
                                     Describe("collision.owner.unsupported",
                                              "A project owner cannot provide typed collision reference evidence.",
                                              "Retain the definition until its owning provider can prove the references."),
                                     Describe("collision.activation.active", "An active world retains its immutable collision generation.",
                                              "Stop Play or preview, edit settings, then restart to activate a new capture."),
                                     Describe("collision.edit.capacity", "Collision edit capacity is unavailable.",
                                              "Reduce the edit or retry after memory is available.")};
    }  // namespace

    /** @copydoc CollisionSettingsError */
    const ErrorCodeDescriptor &CollisionSettingsError(const CollisionSettingsFailure reason) noexcept {
        const auto index = static_cast<std::size_t>(reason);
        return Descriptors[index < Descriptors.size() ? index : 0];
    }
}  // namespace Horo::Editor
