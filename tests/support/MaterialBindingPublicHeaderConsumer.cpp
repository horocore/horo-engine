#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingBackend.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderBackend.h"

#include <type_traits>
static_assert(std::is_base_of_v<Horo::Render::IMaterialBindingBackend, Horo::Render::IRenderBackend>);

int main() {
    const Horo::Render::MaterialBindingLimits limits;
    Horo::Render::IMaterialBindingBackend unsupported;
    return limits.IsValid() && unsupported.Realize({}).HasError() ? 0 : 1;
}
