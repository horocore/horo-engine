#pragma once

#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace Horo::Platform::Detail {
    /** @brief Verifies a retained read-only file view against the actual Windows main executable through the kernel.
     * @param view A live read-only data mapping owned by the caller for this operation.
     * @return Typed unavailability for foreign files, invalid mappings or a missing native comparison API.
     * @details This private check never issues ProcessImageIdentity or accepts a pathname/digest as authority.
     */
    [[nodiscard]] Result<void> VerifyWindowsMainImageFileView(const void *view);
}  // namespace Horo::Platform::Detail

namespace Horo::Platform {
    /** @brief Owns one observed already loaded Windows System32 image and its kernel-matched file bytes.
     * @details Move-only. This evidence concerns one image. It grants no permission to load code and says
     * nothing about its dependencies, import redirection or future changes to the process module inventory.
     */
    class WindowsSystemModuleImage final {
    public:
        /** @brief Transfers the module reference and read-only file mapping. */
        WindowsSystemModuleImage(WindowsSystemModuleImage &&) noexcept;
        /** @brief Releases existing evidence and takes ownership of the given lease. */
        WindowsSystemModuleImage &operator=(WindowsSystemModuleImage &&) noexcept;
        /** @brief Releases the mapping, file and loader reference. */
        ~WindowsSystemModuleImage();

        /** @brief Revalidates the retained image and borrows its complete read-only file bytes.
         * @return Verified file bytes or typed unavailable if the observed inventory or retained identity changed.
         * @post A returned view is borrowed until destruction or replacement of the lease.
         */
        [[nodiscard]] Result<std::span<const std::byte>> ReadVerifiedFile() const;

    private:
        struct State;
        /** @brief Stores the verified native owner. */
        explicit WindowsSystemModuleImage(std::unique_ptr<State> state) noexcept;
        friend Result<WindowsSystemModuleImage> RetainLoadedWindowsSystemModule(std::string_view);
        std::unique_ptr<State> state_;
    };

    /** @brief Retains an unambiguous already loaded System32 DLL by its bounded ordinary ASCII basename.
     * @param basename Import name without a path, stream or redirection.
     * @return Move-only verified lease, or typed unavailability for absent, ambiguous, foreign or changed evidence.
     * @details Does not map executable code, change process lookup policy or attempt to satisfy missing DLLs.
     */
    [[nodiscard]] Result<WindowsSystemModuleImage> RetainLoadedWindowsSystemModule(std::string_view basename);
}  // namespace Horo::Platform
