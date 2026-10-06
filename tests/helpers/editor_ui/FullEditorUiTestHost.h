#pragma once

#include "EditorUiTestHarness.h"
#include "Horo/Editor/GuiRoute.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct ImGuiTestContext;

namespace Horo::Editor {
    struct EditorMenuInvocation;
    class GuiScreenHost;
}  // namespace Horo::Editor

namespace Horo::Tests {
    /** @brief Owns the real HoroEditor screen, modal, project, input, and workspace composition for one scenario. */
    class FullEditorUiTestHost {
    public:
        FullEditorUiTestHost(IEditorUiTestSurface &surface, std::string locale = "en-US",
                             std::optional<std::string> recentProjectName = std::nullopt);
        ~FullEditorUiTestHost();
        FullEditorUiTestHost(const FullEditorUiTestHost &) = delete;
        FullEditorUiTestHost &operator=(const FullEditorUiTestHost &) = delete;

        /** @brief Advances and draws one complete editor frame. */
        void DrawFrame(ImGuiTestContext *context);

        /** @brief Advances exactly the requested number of editor fixed ticks. */
        void AdvanceFixedTicks(std::size_t count);

        /** @brief Returns the first rendered object's world position when one is published. */
        [[nodiscard]] std::optional<Math::Vec3> FirstViewportObjectPosition() const noexcept;

        /** @brief Returns the first active authoring-runtime entity position. */
        [[nodiscard]] std::optional<Math::Vec3> FirstAuthoringObjectPosition() const noexcept;

        /** @brief Returns the document revision currently published to the viewport handoff. */
        [[nodiscard]] std::uint64_t ViewportDocumentRevision() const noexcept;

        /** @brief Returns retained build output for E2E failure diagnostics. */
        [[nodiscard]] std::string BuildDiagnosticText() const;
        /** @brief Checks the published native artifact against the current source and host toolchain inputs. */
        [[nodiscard]] bool IsGameplayBuildUpToDate(const std::filesystem::path &projectRoot) const;

        /** @brief Returns the active top-level route. */
        [[nodiscard]] Editor::GuiRouteKind ActiveRoute() const noexcept;

        /** @brief Returns the isolated root where setup-created projects are stored. */
        [[nodiscard]] const std::filesystem::path &ProjectsRoot() const noexcept;

        /** @brief Reports whether the given route has been submitted to its real screen Draw callback. */
        [[nodiscard]] bool WasRouteDrawn(Editor::GuiRouteKind route) const noexcept;

        /** @brief Returns how many frames submitted the given route to its real screen Draw callback. */
        [[nodiscard]] std::size_t RouteDrawCount(Editor::GuiRouteKind route) const noexcept;

        /** @brief Returns the real screen host for cross-surface assertions. */
        [[nodiscard]] Editor::GuiScreenHost &Screens() noexcept;

        /** @brief Dispatches one menu invocation from the next editor-owned UI frame. */
        void DispatchMenuInvocationOnNextFrame(Editor::EditorMenuInvocation invocation);

        /** @brief Seeds the active real asset-import modal with one source file. */
        [[nodiscard]] bool BeginAssetImport(const std::filesystem::path &source);

        /** @brief Imports the first pending item through the active real asset-import modal. */
        [[nodiscard]] bool ImportFirstPendingAsset();

        /** @brief Resolves the active asset-import conflict by keeping a renamed copy. */
        [[nodiscard]] bool ResolvePendingAssetConflict();

        /** @brief Returns the central input router driven by the Test Engine IO bridge. */
        [[nodiscard]] Input::InputRouter &Input() noexcept;

        /** @brief Returns the projection published by the active workspace to the viewport scene handoff. */
        [[nodiscard]] Runtime::CameraProjection ViewportProjection() const noexcept;

        /** @brief Reports whether the selected renderer has produced a viewport image. */
        [[nodiscard]] bool RendererReady() const noexcept;

        /** @brief Returns the canonical renderer selected for this E2E composition. */
        [[nodiscard]] std::string_view RendererName() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Tests
