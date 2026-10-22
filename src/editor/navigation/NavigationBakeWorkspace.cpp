#include "Horo/Editor/NavigationBakeWorkspace.h"

namespace Horo::Editor {
    /** @copydoc NavigationBakeCancelDecision::Confirm */
    Result<void> NavigationBakeCancelDecision::Confirm() {
        if (confirmed_)
            return Result<void>::Success();
        if (!cancel_ || (id_.opening == 0 || id_.command == 0))
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::InvalidHandle));
        auto result = cancel_(id_);
        confirmed_ = result.HasValue();
        return result;
    }
}  // namespace Horo::Editor
