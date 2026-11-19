#pragma once

/** @file EditorMountedWorldViewport.h
 * @brief App-owned handoff from the selected Play mount to the editor's render graph.
 */
#include "MountedWorldHost.h"

#include <functional>
#include <memory>

namespace Horo::Application::Internal {
    /** @brief Exact canonical scene selected by the mounted Play owner. */
    struct EditorMountedWorldViewportView final {
        Runtime::SceneRuntimeId owner;
        Render::RenderViewId view;
        std::shared_ptr<const MountedWorldRenderPacket> packet;
    };

    /** @brief Single owner-lane publication retained by the real viewport graph across its phases. */
    class EditorMountedWorldViewport final {
    public:
        /** @brief Publishes a complete packet for one new Play world. @return Typed rejection if occupied or invalid. */
        [[nodiscard]] Result<void> Publish(EditorMountedWorldViewportView value);
        /** @brief Replaces the exact current packet with a fresh capture from the same Scene incarnation and renderer view.
         * @param expected Packet currently owned by the viewport publication.
         * @param candidate Complete successor capture, still valid for new submissions.
         * @return Success or typed rejection that leaves the current publication intact. */
        [[nodiscard]] Result<void> Replace(const std::shared_ptr<const MountedWorldRenderPacket> &expected,
                                           std::shared_ptr<const MountedWorldRenderPacket> candidate);
        /** @brief Withdraws only the issuing Play world's packet. */
        void Withdraw(Runtime::SceneRuntimeId owner) noexcept;

        /** @brief Borrows the exact active publication for this app viewport. */
        [[nodiscard]] const EditorMountedWorldViewportView &Current() const noexcept {
            return current_;
        }

    private:
        EditorMountedWorldViewportView current_;
    };

    /** @brief Explicit device-neutral camera/draw selection and app-owned frontend admission. */
    struct EditorMountedWorldViewportSelection final {
        Runtime::SceneObjectId camera;
        std::vector<Runtime::SceneObjectId> meshes;
        std::shared_ptr<EditorMountedWorldViewport> publication;
        std::function<Result<Render::RenderViewId>()> issueView;
    };
}  // namespace Horo::Application::Internal
