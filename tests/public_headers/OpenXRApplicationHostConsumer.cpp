#include "OpenXRPlayHost.h"

#include <type_traits>

static_assert(static_cast<int>(Horo::Editor::EditorPlayViewSource::AuthoredCamera) == 0);
static_assert(static_cast<int>(Horo::Editor::EditorPlayViewSource::SelectedXr) == 1);

static_assert(std::is_base_of_v<Horo::Editor::IEditorPlayXrHost, Horo::Application::Internal::OpenXRPlayHost>);

namespace Horo::Tests {
    /** @brief Compiles the real non-installed host's complete staged dependency closure. */
    auto OpenXRApplicationPresentationProjection() {
        return &Application::Internal::OpenXRPlayHost::PresentationState;
    }

    auto OpenXRApplicationMirrorBorrow() {
        return &Application::Internal::OpenXRPlayHost::MirrorView;
    }

    auto OpenXRApplicationHostFactory() {
        return &Application::Internal::OpenXRPlayHost::Create;
    }
}  // namespace Horo::Tests
