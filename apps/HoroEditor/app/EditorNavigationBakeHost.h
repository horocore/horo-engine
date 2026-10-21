#pragma once
/** @file EditorNavigationBakeHost.h
 * @brief Executable composition and lifetime for project navigation operations.
 */
#include "Horo/Editor/NavigationBakeWorkspace.h"

namespace Horo::Editor {
    /** @brief Host-owned project sessions, independent of workspace and modal surfaces.
     * @details All calls run on the editor lane. Dedicated bounded workers own preparation/capture;
     * shutdown revokes source authority, cancels and drains before releasing applications.
     */
    class EditorNavigationBakeHost final {
    public:
        /** @brief Borrows the process operation store through shutdown. @param operations Shared Operations authority. */
        explicit EditorNavigationBakeHost(OperationStore &operations);
        /** @brief Drains owned workers before releasing project source and publication capabilities. */
        ~EditorNavigationBakeHost();
        /** @brief Returns narrow callbacks borrowing this executable owner. @return Presentation capabilities. */
        [[nodiscard]] NavigationBakeWorkspaceAccess Access();
        /** @brief Advances preparation and accepted operations without waiting, independent of surface visibility. */
        void Pump();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
