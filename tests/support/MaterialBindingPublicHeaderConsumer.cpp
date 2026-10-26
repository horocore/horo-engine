#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingBackend.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"

#include <concepts>
#include <type_traits>
static_assert(std::is_same_v<decltype(Horo::Render::RenderGraphicsPipelineDescriptor{}.specializationValues),
                             std::vector<Horo::Render::ShaderSpecializationValue>>);
static_assert(std::is_base_of_v<Horo::Render::IMaterialBindingBackend, Horo::Render::IRenderBackend>);

static_assert(!std::is_same_v<Horo::Render::MaterialBindingId, Horo::Render::MaterialBindingGenerationId>);
static_assert(Horo::Render::CoreDefaultMaterial.IsValid());
static_assert(!Horo::Render::MaterialBindingGenerationId{}.IsValid());

static_assert(!std::is_default_constructible_v<Horo::Render::MaterialBindingTable>);
static_assert(!std::is_default_constructible_v<Horo::Render::ResidentMaterialBinding>);
static_assert(!std::is_constructible_v<Horo::Render::MaterialBindingTable, std::nullptr_t>);
static_assert(!std::is_constructible_v<Horo::Render::ResidentMaterialBinding, std::nullptr_t>);

template <class T>
concept ForgedConstructionKey = requires { T(nullptr, {}); };
static_assert(!ForgedConstructionKey<Horo::Render::MaterialBindingTable>);
static_assert(!ForgedConstructionKey<Horo::Render::ResidentMaterialBinding>);

static_assert(std::is_same_v<decltype(Horo::Render::MaterialParameterValue{}.parameter), Horo::Render::ShaderParameterId>);

static_assert(std::is_same_v<decltype(Horo::Render::RenderMaterialPass{}.submission), std::optional<Horo::Render::RenderQueueSubmission>>);
static_assert(Horo::Render::RenderGraphSynchronizationOperation::GraphicsInputBuffer !=
              Horo::Render::RenderGraphSynchronizationOperation::Storage);

int main() {
    const Horo::Render::MaterialBindingLimits limits;
    Horo::Render::IMaterialBindingBackend unsupported;
    return limits.IsValid() && unsupported.Realize({}).HasError() ? 0 : 1;
}

static_assert(requires(const Horo::Render::MaterialBindingLayout &layout, std::span<const Horo::Render::MaterialParameterValue> values,
                       std::span<std::byte> bytes) {
    {
        Horo::Render::PackMaterialParameterBinding(layout, Horo::Render::ShaderBindingId{1}, values, bytes)
    } -> std::same_as<Horo::Result<std::size_t>>;
});
