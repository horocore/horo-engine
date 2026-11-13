#include "FileManagerNative.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>

namespace Horo::FileManagerInternal {
    /** @copydoc Dispatch */
    Result<void> Dispatch(const Target &target) {
        const auto directory = target.directory ? target.path : target.path.parent_path();
        const std::string uri = DirectoryUri(directory);
        if (!SDL_OpenURL(uri.c_str()))
            return Result<void>::Failure(MakeError(FileManagerErrors::DispatchFailed, SDL_GetError()));
        return Result<void>::Success();
    }
}  // namespace Horo::FileManagerInternal
