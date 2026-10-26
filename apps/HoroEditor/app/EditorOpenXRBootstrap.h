#pragma once
/** @file EditorOpenXRBootstrap.h
 * @brief Actual executable-owned signed installation/bootstrap lifetime, before window/device creation.
 */
#include "EditorOpenXRHostServices.h"

namespace Horo::Editor {
    /** @brief Retains the independently approved trust, real canonical package owner and its borrowed worker services.
     * @details The executable invokes this factory itself. No injection-only launcher or project-selected trust is
     * required. Absence of explicit host inputs returns no XR host; partial/invalid explicit inputs fail closed.
     * Destruction is cold teardown: it cancels/joins its own package jobs while all borrowed authorities remain alive.
     */
    class EditorOpenXRBootstrap final {
    public:
        /** @brief Parses only explicit host arguments and prepares one real verified offline package installation.
         * @param arguments Actual process arguments, including executable name.
         * @param projectRoot Explicit selected absolute project; package metadata is read only by the canonical host.
         * @param cacheRoot Explicit existing application cache policy. @param trackingRoot Exact authored Scene selection.
         * @return No host for ordinary desktop, a complete retained host, or the original trust/install/admission failure.
         * @details All three --xr-host-roots, --xr-host-catalog and --xr-host-catalog-signature paths are required together.
         * Roots are an independent explicit operator/installer approval, never inferred from the signed catalog/project.
         * The startup-only wait for existing package jobs is bounded to ten seconds; no native library is opened here.
         * @throws std::bad_alloc Cold startup allocation failure, after RAII cancels/joins any admitted package work.
         */
        [[nodiscard]] static Result<std::unique_ptr<EditorOpenXRBootstrap>> Create(std::span<char *> arguments,
                                                                                   const std::filesystem::path &projectRoot,
                                                                                   const std::filesystem::path &cacheRoot,
                                                                                   Runtime::SceneObjectId trackingRoot);
        ~EditorOpenXRBootstrap();
        EditorOpenXRBootstrap(const EditorOpenXRBootstrap &) = delete;
        EditorOpenXRBootstrap &operator=(const EditorOpenXRBootstrap &) = delete;
        /** @brief Borrows the same actual canonical package host/policy through native retirement and application shutdown. */
        [[nodiscard]] const EditorOpenXRHostServices &Services() const noexcept;

    private:
        struct Impl;
        explicit EditorOpenXRBootstrap(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
