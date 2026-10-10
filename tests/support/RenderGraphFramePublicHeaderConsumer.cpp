#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphLifetime.h"
#include "Horo/Runtime/Render/RenderGraphTransientResources.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/UiRenderSubmission.h"

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::IRenderResourceBackend &>().QueryBufferMemoryCost(
                                 std::declval<const Horo::Render::RenderBufferDescriptor &>())),
                             Horo::Result<Horo::Render::RenderMemoryCostPlan>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::IRenderResourceBackend &>().QueryTextureMemoryCost(
                                 std::declval<const Horo::Render::RenderTextureDescriptor &>())),
                             Horo::Result<Horo::Render::RenderMemoryCostPlan>>);
static_assert(!noexcept(std::declval<const Horo::Render::IRenderResourceBackend &>().QueryBufferMemoryCost(
    std::declval<const Horo::Render::RenderBufferDescriptor &>())));
static_assert(!noexcept(std::declval<const Horo::Render::IRenderResourceBackend &>().QueryTextureMemoryCost(
    std::declval<const Horo::Render::RenderTextureDescriptor &>())));

static_assert(std::is_same_v<decltype(std::declval<Horo::Render::RenderFrameScope &>().ExecuteGraph(
                                 std::declval<const Horo::Render::CompiledRenderGraphExecution &>(),
                                 std::span<const Horo::Render::RenderGraphPassWorkload>{})),
                             Horo::Result<void>>);

static_assert(std::is_same_v<decltype(std::declval<Horo::Render::RenderFrameScope &>().ExecuteGraph(
                                 std::declval<const Horo::Render::CompiledRenderGraphExecution &>(),
                                 std::span<const Horo::Render::RenderGraphPassWorkload>{},
                                 Horo::Render::RenderGraphTransientResourcesHandle{}, std::declval<Horo::Render::UiRenderSubmission>())),
                             Horo::Result<void>>);

static_assert(std::is_same_v<decltype(std::declval<Horo::Render::RenderFrontend &>().PrepareTransientGraphResources(
                                 std::declval<const Horo::Render::RenderGraphLifetimePlan &>(), Horo::Render::RenderMemoryScopeId{})),
                             Horo::Result<Horo::Render::RenderGraphTransientResourcesHandle>>);
static_assert(
    std::is_same_v<decltype(std::declval<Horo::Render::RenderFrameScope &>().ExecuteGraph(
                       std::declval<const Horo::Render::CompiledRenderGraphExecution &>(),
                       std::span<const Horo::Render::RenderGraphPassWorkload>{}, Horo::Render::RenderGraphTransientResourcesHandle{})),
                   Horo::Result<void>>);
