/** @file MaterialBindingTestFixture.h
 * @brief Shared typed material descriptor and actionable-error assertions for material ownership and packing tests.
 */
#pragma once

#include "Horo/Runtime/Render/MaterialBinding.h"
#include "support/TypedIdentityTestSupport.h"

namespace Horo::Tests::MaterialBindingFixture {
    using namespace Horo::Render;
    inline constexpr RenderResourceOwnerId Owner{17};

    [[nodiscard]] inline MaterialBindingDescriptor Descriptor() {
        MaterialBindingLayout layout;
        layout.backend = ShaderTargetBackend::Metal;
        layout.resources = {
            {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment, true}};
        layout.targetBindings = {{{1}, 0, 0, 2, "", {}, true}};
        layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 3, 1, 0, 0, 0, true, true}};
        return {{4}, 1, {Owner, 2, 3}, std::move(layout), {{{1}, 0, MaterialParameterBinding{std::vector<std::byte>(12)}}}, true, {}};
    }

    template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &code) {
        Tests::RequireActionableError(result, code);
    }
}  // namespace Horo::Tests::MaterialBindingFixture
