#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "Horo/Runtime/Ui/UiCpuWorkStatistics.h"
#include "Horo/Runtime/Ui/UiHotReload.h"
#include "Horo/Runtime/Ui/UiLayoutInspector.h"
#include "Horo/Runtime/Ui/UiLayoutWork.h"
#include "Horo/Runtime/Ui/UiPresentationProfiles.h"
#include "Horo/Runtime/Ui/UiRenderGeometry.h"
#include "Horo/Runtime/Ui/UiTheme.h"

#include <type_traits>
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiLayoutInspectionSnapshot>);
static_assert(std::is_move_constructible_v<Horo::Runtime::Ui::UiPresentationProfileTable>);
static_assert(Horo::Runtime::Ui::MaximumUiPresentationProfiles == 32);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiLayoutWork>);

using Horo::Runtime::Ui::UiDocument;
using Horo::Runtime::Ui::UiElementTree;
static_assert(requires(const UiDocument &document, const UiElementTree &tree, Horo::Runtime::Ui::UiCanvasId canvas) {
    Horo::Runtime::Ui::UiPresentationProfileTable::CreateForAuthoring(document, tree, canvas);
});

static_assert(requires(const Horo::Runtime::Ui::UiComputedStyleSnapshot &computed, const Horo::Runtime::Ui::UiThemeSurfaceInput &input,
                       const Horo::Runtime::Ui::UiThemeSurfaceProperties &properties) {
    Horo::Runtime::Ui::ProjectUiThemeSurface(computed, input, properties);
});

static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiCanvasInspectionSnapshot>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::Ui::UiCpuWorkStatistics>);
static_assert(requires(const Horo::Runtime::Ui::UiRenderGeometryPlan &plan) { plan.Inspect(16); });
static_assert(requires(const Horo::Runtime::Ui::UiHotReload &publisher, const Horo::Runtime::Ui::UiReloadLease &lease,
                       Horo::Runtime::Ui::UiCanvasId canvas) { publisher.Inspect(lease, canvas); });
static_assert(requires(const Horo::Runtime::Ui::RuntimeUiService &service, Horo::Runtime::Ui::RuntimeUiInstanceId instance,
                       const Horo::Runtime::Ui::UiReloadLease &lease,
                       Horo::Runtime::Ui::UiCanvasId canvas) { service.Inspect(instance, lease, canvas); });
