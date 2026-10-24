#pragma once
/** @file OpenXRLoaderLease.h
 * @brief Non-installed retained first-party loader/files ownership used by the application XR composition root.
 */
#include "Horo/Platform/DynamicLibrary.h"
#include "OpenXRProductDeployment.h"

namespace Horo::Application::Internal {
    /** @brief Retains exact signed component, private files and loader through every native session borrow.
     * @details The returned DynamicLibrary itself retains all file/install evidence, so abandoning an outer
     * partial composition cannot remove files while an existing native owner still retains its library.
     * Native session/frame/renderer drain remains the application's explicit prerequisite for releasing its final lease.
     */
    class OpenXRLoaderLease final {
        struct Files;
        class RetainedLibrary;

    public:
        /** @brief Stages and opens exactly one already admitted first-party component, without runtime discovery.
         * @param component Sealed immutable install/manifest and full identity.
         * @param deployment Signed installer-qualified dependency-minimized deployment bound to that identity.
         * @param temporaryParent Canonical owner-selected existing private temporary parent.
         * @param gate Existing host-composed native signature authority, mandatory immediately before loading.
         * @param cancellation Cooperative preparation cancellation.
         * @return Owning loader lease or typed original verification/storage/loading failure. No fallback search is performed.
         * @pre This load-time operation runs on the host's declared preparation lane, never in a frame callback.
         * @post No vendor runtime library is opened, no session is activated, no support claim or service is published.
         */
        [[nodiscard]] static Result<std::shared_ptr<const OpenXRLoaderLease>> Prepare(
            VerifiedOpenXRComponent component, VerifiedOpenXRDeployment deployment, const std::filesystem::path &temporaryParent,
            std::shared_ptr<const Security::NativeArtifactGate> gate, const CancellationToken &cancellation);
        /** @brief Returns the loader's owning private native primitive; it also pins files, signatures and installation.
         * @return Shared library owner consumed by the existing OpenXRNativeSession. */
        [[nodiscard]] std::shared_ptr<const Platform::DynamicLibrary> Library() const noexcept;
        /** @brief Returns immutable exact verified component evidence. @return Borrow valid through this lease. */
        [[nodiscard]] const VerifiedOpenXRComponent &Component() const noexcept;

    private:
        /** @brief Publishes only completely staged, signed and opened ownership. */
        OpenXRLoaderLease(std::shared_ptr<const Files> files, std::shared_ptr<const Platform::DynamicLibrary> library);
        std::shared_ptr<const Files> files_;
        std::shared_ptr<const Platform::DynamicLibrary> library_;
    };
}  // namespace Horo::Application::Internal
