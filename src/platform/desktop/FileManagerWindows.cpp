#include "FileManagerNative.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Shell headers require Windows declarations before they are parsed.
// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on
#include <string>

namespace Horo::FileManagerInternal {
    namespace {
        /** @brief Balances only COM initialization owned by this request; respects an existing apartment. */
        class ComApartment final {
        public:
            ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

            ~ComApartment() {
                if (SUCCEEDED(result_))
                    CoUninitialize();
            }

            ComApartment(const ComApartment &) = delete;
            ComApartment &operator=(const ComApartment &) = delete;

            [[nodiscard]] bool Ready() const noexcept {
                return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
            }

        private:
            HRESULT result_;
        };

        /** @brief Releases Shell item identity storage with its matching allocator. */
        struct ShellItem final {
            PIDLIST_ABSOLUTE value{};
            ShellItem() = default;

            ~ShellItem() {
                if (value != nullptr)
                    ILFree(value);
            }

            ShellItem(const ShellItem &) = delete;
            ShellItem &operator=(const ShellItem &) = delete;
        };
    }  // namespace

    /** @copydoc Dispatch */
    Result<void> Dispatch(const Target &target) {
        ComApartment apartment;
        if (!apartment.Ready())
            return Result<void>::Failure(MakeError(FileManagerErrors::DispatchFailed, "COM apartment initialization failed."));

        ShellItem item;
        const HRESULT parsedResult = SHParseDisplayName(target.path.c_str(), nullptr, &item.value, 0, nullptr);
        if (FAILED(parsedResult))
            return Result<void>::Failure(
                MakeError(FileManagerErrors::DispatchFailed, "Shell path parsing failed: " + std::to_string(parsedResult)));

        if (target.directory) {
            SHELLEXECUTEINFOW request{};
            request.cbSize = sizeof(request);
            request.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            request.lpVerb = L"open";
            request.lpIDList = item.value;
            request.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&request))
                return Result<void>::Success();
            return Result<void>::Failure(
                MakeError(FileManagerErrors::DispatchFailed, "Shell directory dispatch failed: " + std::to_string(GetLastError())));
        }

        const HRESULT revealed = SHOpenFolderAndSelectItems(item.value, 0, nullptr, 0);
        if (FAILED(revealed))
            return Result<void>::Failure(
                MakeError(FileManagerErrors::DispatchFailed, "Shell item reveal failed: " + std::to_string(revealed)));
        return Result<void>::Success();
    }
}  // namespace Horo::FileManagerInternal
