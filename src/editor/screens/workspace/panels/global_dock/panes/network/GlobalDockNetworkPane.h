#pragma once
/** @file GlobalDockNetworkPane.h
 * @brief Presentation of immutable bounded runtime network diagnostics.
 */
#include "Horo/Application/NetworkDebugger.h"

#include <array>
struct ImVec2;

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Narrow query/command client; owns only display selection and copied diagnostic evidence. */
    class GlobalDockNetworkPane final {
    public:
        /** @brief Borrows host-owned application capabilities through panel detachment.
         * @param query Optional source query. @param control Optional typed capture command capability. */
        void Attach(const Application::INetworkDebuggerQuery *query, Application::INetworkDebuggerControl *control) noexcept;
        /** @brief Releases borrowed capabilities and removes live provenance. */
        void Detach() noexcept;
        /** @brief Draws a responsive view of one coherent publication.
         * @param origin Content origin. @param width Available width. @param context Shared theme and localization. */
        void Draw(const ImVec2 &origin, float width, const EditorGuiContext &context);

        /** @brief Returns the last drawn immutable projection. @return Copied evidence and assessed provenance. */
        [[nodiscard]] const Application::NetworkDebuggerProjection &Projection() const noexcept {
            return projection_;
        }

        /** @brief Returns the localization key for explicit source provenance. @param state Assessed state. @return Stable key. */
        [[nodiscard]] static const char *StateKey(Application::NetworkDebuggerState state) noexcept;

    private:
        enum class View : std::uint8_t {
            Connections,
            Replication,
            Rpc,
            Prediction,
            Interest,
            Capture,
            Count
        };
        void DrawToolbar(float width, const EditorGuiContext &context);
        void DrawEvidence(float width, const EditorGuiContext &context) const;
        void Request(Network::NetworkCaptureAction action);
        const Application::INetworkDebuggerQuery *query_{};
        Application::INetworkDebuggerControl *control_{};
        Application::NetworkDebuggerProjection projection_;
        std::array<char, 256> search_{};
        View view_{View::Connections};
    };
}  // namespace Horo::Editor
