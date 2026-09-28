#pragma once

namespace Horo::Editor {

    class IEditorUpdateBackend;

    /**
     * @file HoroEditorApp.h
     * @brief Graphical HoroEditor application bootstrap.
     */

    /**
     * @brief Runs the graphical HoroEditor application.
     * @param argc Process argument count.
     * @param argv Process argument values.
     * @param updateBackend Optional installed-host update operations, kept alive through shutdown.
     * @return Process exit code.
     */
    int RunEditorGuiApp(int argc, char **argv, IEditorUpdateBackend *updateBackend = nullptr);

}  // namespace Horo::Editor
