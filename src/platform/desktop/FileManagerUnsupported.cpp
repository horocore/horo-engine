#include "FileManagerNative.h"

namespace Horo::FileManagerInternal {
    /** @copydoc Dispatch */
    Result<void> Dispatch(const Target &) {
        return Result<void>::Failure(MakeError(FileManagerErrors::Unavailable));
    }
}  // namespace Horo::FileManagerInternal
