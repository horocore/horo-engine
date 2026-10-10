#include "ProjectReadNative.h"

#include <utility>

namespace Horo::Platform::ProjectReadNative {
    /** @copydoc Handle::Handle */
    Handle::Handle(const HandleValue value) noexcept : value_(NormalizeHandle(value)) {}

    /** @copydoc Handle::~Handle */
    Handle::~Handle() {
        if (IsValid())
            CloseHandleValue(value_);
    }

    /** @copydoc Handle::Handle */
    Handle::Handle(Handle &&other) noexcept : value_(other.Release()) {}

    /** @copydoc Handle::operator= */
    Handle &Handle::operator=(Handle &&other) noexcept {
        if (this != &other) {
            if (IsValid())
                CloseHandleValue(value_);
            value_ = other.Release();
        }
        return *this;
    }

    /** @copydoc Handle::Get */
    HandleValue Handle::Get() const noexcept {
        return value_;
    }

    /** @copydoc Handle::Release */
    HandleValue Handle::Release() noexcept {
        return std::exchange(value_, InvalidHandle);
    }

    /** @copydoc Handle::IsValid */
    bool Handle::IsValid() const noexcept {
        return value_ != InvalidHandle;
    }

    /** @copydoc AdmitChild */
    Result<Handle> AdmitChild(Handle child, const bool directory) {
        auto info = Info(child);
        if (info.HasError())
            return Result<Handle>::Failure(info.ErrorValue());
        if ((!info.Value().regular && !info.Value().directory) || (info.Value().regular && info.Value().links != 1) ||
            (directory && !info.Value().directory))
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::UnsafePath));
        return Result<Handle>::Success(std::move(child));
    }

    /** @copydoc OpenRoot */
    Result<Handle> OpenRoot(const std::filesystem::path &path) {
        auto anchor = OpenAnchor(path);
        if (anchor.HasError())
            return anchor;
        auto root = AdmitChild(std::move(anchor).Value(), true);
        if (root.HasError())
            return root;
        for (const auto &part : path.relative_path()) {
            if (part.empty())
                continue;
            auto name = ComponentName(part);
            if (name.HasError())
                return Result<Handle>::Failure(name.ErrorValue());
            auto child = OpenChild(root.Value(), name.Value(), true);
            if (child.HasError())
                return child;
            root = std::move(child);
        }
        return root;
    }
}  // namespace Horo::Platform::ProjectReadNative
