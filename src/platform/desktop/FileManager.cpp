#include "FileManagerNative.h"

#include <string_view>

namespace Horo::FileManagerErrors {
    namespace {
        const ErrorDomainId Domain{"horo.platform.file_manager"};
    }

    const ErrorCodeDescriptor InvalidTarget{
        .domain = Domain,
        .code = ErrorCode{"file_manager.invalid_target"},
        .summary = "File Manager requires an existing absolute regular file or directory without a symbolic link.",
    };
    const ErrorCodeDescriptor Unavailable{
        .domain = Domain,
        .code = ErrorCode{"file_manager.unavailable"},
        .summary = "This host does not provide a desktop file manager.",
    };
    const ErrorCodeDescriptor DispatchFailed{
        .domain = Domain,
        .code = ErrorCode{"file_manager.dispatch_failed"},
        .summary = "The desktop could not accept the File Manager request.",
    };
}  // namespace Horo::FileManagerErrors

namespace Horo::FileManagerInternal {
    /** @copydoc PrepareTarget */
    Result<Target> PrepareTarget(const std::filesystem::path &path) {
        const auto &native = path.native();
        if (!path.is_absolute() || native.find(std::filesystem::path::value_type{}) != native.npos)
            return Result<Target>::Failure(MakeError(FileManagerErrors::InvalidTarget));

        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)))
            return Result<Target>::Failure(MakeError(FileManagerErrors::InvalidTarget, error ? error.message() : std::string{}));

        auto nativePath = path;
        nativePath.make_preferred();
        return Result<Target>::Success(Target{std::move(nativePath), std::filesystem::is_directory(status)});
    }

    /** @copydoc DirectoryUri */
    std::string DirectoryUri(const std::filesystem::path &path) {
        constexpr std::string_view Hex{"0123456789ABCDEF"};
        constexpr std::string_view Unescaped{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/"};
        std::string uri{"file://"};
        for (const char8_t character : path.generic_u8string()) {
            const auto byte = static_cast<unsigned char>(character);
            if (Unescaped.find(static_cast<char>(byte)) != std::string_view::npos) {
                uri += static_cast<char>(byte);
            } else {
                uri += '%';
                uri += Hex[byte >> 4U];
                uri += Hex[byte & 0x0FU];
            }
        }
        return uri;
    }
}  // namespace Horo::FileManagerInternal

namespace Horo {
    /** @copydoc RevealInFileManager */
    Result<void> RevealInFileManager(const std::filesystem::path &absolutePath) {
        auto target = FileManagerInternal::PrepareTarget(absolutePath);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        return FileManagerInternal::Dispatch(target.Value());
    }
}  // namespace Horo
