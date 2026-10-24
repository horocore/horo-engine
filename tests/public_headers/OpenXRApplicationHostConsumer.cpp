#include "OpenXRPlayHost.h"

#include <type_traits>

static_assert(std::is_base_of_v<Horo::Editor::IEditorPlayXrHost, Horo::Application::Internal::OpenXRPlayHost>);

namespace Horo::Tests {
    /** @brief Compiles the real non-installed host's complete staged dependency closure. */
    auto OpenXRApplicationMirrorBorrow() {
        return &Application::Internal::OpenXRPlayHost::MirrorView;
    }

    auto OpenXRApplicationHostFactory() {
        return &Application::Internal::OpenXRPlayHost::Create;
    }
}  // namespace Horo::Tests
