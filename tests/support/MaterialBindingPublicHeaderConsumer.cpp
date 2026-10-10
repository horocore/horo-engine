#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingBackend.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderBackend.h"

#include <type_traits>
static_assert(std::is_base_of_v<Horo::Render::IMaterialBindingBackend, Horo::Render::IRenderBackend>);

static_assert(!std::is_same_v<Horo::Render::MaterialBindingId, Horo::Render::MaterialBindingGenerationId>);
static_assert(Horo::Render::CoreDefaultMaterial.IsValid());
static_assert(!Horo::Render::MaterialBindingGenerationId{}.IsValid());

int main() {
    const Horo::Render::MaterialBindingLimits limits;
    Horo::Render::IMaterialBindingBackend unsupported;
    return limits.IsValid() && unsupported.Realize({}).HasError() ? 0 : 1;
}
