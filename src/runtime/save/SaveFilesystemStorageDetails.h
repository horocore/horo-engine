#pragma once

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveNamespace.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Horo::Runtime::SaveFilesystemDetails {
    [[nodiscard]] inline Error Failure(const ErrorCodeDescriptor &descriptor, const char *operation, const int nativeError = 0) {
        std::string message{"Save file "};
        message.append(operation);
        if (nativeError != 0)
            message.append(" failed with filesystem code ").append(std::to_string(nativeError));
        message.push_back('.');
        return MakeError(descriptor, std::move(message));
    }

    [[nodiscard]] inline std::string SlotName(const SaveGameSlotId slot) {
        return slot.ToString() + ".horosave";
    }

    template <typename Step> [[nodiscard]] Result<void> OpenNamespaceComponents(const SaveNamespaceId &name, Step &&step) {
        if (auto opened = step(name.environment.ToString()); opened.HasError())
            return opened;
        if (const auto *profile = std::get_if<UserProfileOwner>(&name.owner)) {
            if (auto opened = step("profile"); opened.HasError())
                return opened;
            if (auto opened = step(profile->user.ToString() + "_" + profile->profile.ToString()); opened.HasError())
                return opened;
        } else {
            if (auto opened = step("server"); opened.HasError())
                return opened;
            if (auto opened = step(std::get<ServerWorldOwner>(name.owner).owner.ToString()); opened.HasError())
                return opened;
        }
        return step("slots");
    }

#ifdef _WIN32
    class WindowsTemporary final {
    public:
        explicit WindowsTemporary(HANDLE file) noexcept : file_(file) {}

        ~WindowsTemporary() {
            if (file_ != INVALID_HANDLE_VALUE) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                (void)::SetFileInformationByHandle(file_, FileDispositionInfo, &disposition, sizeof(disposition));
            }
        }

        void Published() noexcept {
            file_ = INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE file_;
    };

    [[nodiscard]] inline Result<void> WriteWindowsBytes(HANDLE file, std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD written{};
            if (!::WriteFile(file, bytes.data() + offset, amount, &written, nullptr) || written == 0)
                return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows temporary write", ::GetLastError()));
            offset += written;
        }
        if (!::FlushFileBuffers(file))
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows temporary synchronization", ::GetLastError()));
        return Result<void>::Success();
    }
#endif
}  // namespace Horo::Runtime::SaveFilesystemDetails
