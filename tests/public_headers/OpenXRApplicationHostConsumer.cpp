#include "EditorVulkanXrViewportAccess.h"
#include "OpenXRPlayHost.h"

#include <type_traits>

static_assert(static_cast<int>(Horo::Editor::EditorPlayViewSource::AuthoredCamera) == 0);
static_assert(static_cast<int>(Horo::Editor::EditorPlayViewSource::SelectedXr) == 1);

// A member-pointer declaration alone accepts an incomplete mirror result. Keep
// this real host consumer dependent on the complete canonical value contract.
static_assert(std::is_trivially_copyable_v<Horo::Editor::EditorViewportTextureView>);
constexpr Horo::Editor::EditorViewportTextureView emptyMirror;
static_assert(!emptyMirror.IsValid());
static_assert(emptyMirror.u0 == 0.0F && emptyMirror.v0 == 0.0F && emptyMirror.u1 == 1.0F && emptyMirror.v1 == 1.0F);
static_assert(!emptyMirror.sampledView.IsValid() && !emptyMirror.sampledSampler.IsValid());

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
