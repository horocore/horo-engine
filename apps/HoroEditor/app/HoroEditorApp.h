#pragma once

#include <memory>
#include <optional>

namespace Horo::Render {
    class IShaderCompilerAdapter;
}

namespace Horo::Editor {

    class IEditorUpdateBackend;

    /** @brief Installed-host verified activation result from a completed helper transaction. */
    enum class EditorVerifiedUpdateOutcome {
        Active,
        RolledBack,
    };

    /** @brief Borrowed installer-owned update operations and optional verified relaunch outcome. */
    struct EditorUpdateHostServices final {
        IEditorUpdateBackend &backend;
        std::optional<EditorVerifiedUpdateOutcome> verifiedOutcome;
    };

    /**
     * @file HoroEditorApp.h
     * @brief Graphical HoroEditor application bootstrap.
     */

    /**
     * @brief Runs the graphical HoroEditor application.
     * @param argc Process argument count.
     * @param argv Process argument values.
     * @param updateHost Optional installed-host update operations and authenticated helper outcome, kept alive through shutdown.
     * @param shaderCompiler Optional explicitly qualified host compiler; absent produces typed tool-unavailable output, without SDK
     * discovery.
     * @return Process exit code.
     */
    int RunEditorGuiApp(int argc, char **argv, const EditorUpdateHostServices *updateHost = nullptr,
                        std::shared_ptr<const Render::IShaderCompilerAdapter> shaderCompiler = {});

}  // namespace Horo::Editor
