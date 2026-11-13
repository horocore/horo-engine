#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

#if !defined(HORO_PROFILE_EDITOR) && !defined(HORO_PROFILE_DEVELOPMENT) && !defined(HORO_PROFILE_DIAGNOSTICS)
#error "DiagnosticsOnly console translation units require Editor, Development or Diagnostics."
#endif

namespace Horo::Debug::Detail {
    /** @copydoc SupportDescriptor */
    DebugCommandDescriptor SupportDescriptor() noexcept {
        return {SupportBundleCommandId,
                "diag.support_bundle",
                "Generate a separately user-approved diagnostic bundle",
                "diag.support_bundle",
                {},
                CommandPermission::Restricted,
                CommandThreadPolicy::WorkerJob,
                CommandAvailability::DiagnosticsOnly,
                CommandFlags::AuditLogged | CommandFlags::RedactArguments};
    }
}  // namespace Horo::Debug::Detail
