#include "Horo/Application/UiTemplateHost.h"

#include <type_traits>
#include <utility>

using Horo::Application::UiTemplateCanvasProjection;
using Horo::Application::UiTemplateRuntimeLease;
static_assert(
    std::is_same_v<decltype(std::declval<const UiTemplateRuntimeLease &>().Projections()), std::span<const UiTemplateCanvasProjection>>);
static_assert(noexcept(std::declval<const UiTemplateRuntimeLease &>().Projections()));
// Runtime holds the actual layout slot. A second host pin would prevent bounded retirement from draining.
static_assert(std::is_same_v<decltype(UiTemplateCanvasProjection::layout), Horo::Runtime::Ui::UiLayoutSnapshotDescriptor>);
