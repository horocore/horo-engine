#pragma once
/** @file RenderGraphInspectionPane.h
 * @brief Host-private read-only consumer of detached render graph inspection values.
 */
#include "Horo/Editor/IGlobalDockPane.h"
#include "Horo/Runtime/Render/RenderGraphInspection.h"

#include <functional>

namespace Horo::Editor {
    /** @brief Query of an already committed owned value; must not capture, enumerate native state or enable instrumentation. */
    using RenderGraphInspectionQuery = std::function<Result<std::shared_ptr<const Render::RenderGraphInspectionSnapshot>>()>;

    /**
     * @brief Creates an optional inspector pane over a host-composed cached-value query.
     * @param query Owned narrow callback; borrowed host dependencies must outlive detachment or pane destruction.
     * @return Owned pane, initially inactive; construction/hidden panes never invoke the query.
     * @details The pane formats at most 128 records per selected page, keeps one immutable snapshot,
     * and changes no renderer state. Empty, failed and unavailable timing states remain explicit.
     * Detachment releases the callback and captured value; later draws never query the former host.
     */
    [[nodiscard]] std::unique_ptr<IGlobalDockPane> MakeRenderGraphInspectionPane(RenderGraphInspectionQuery query);
}  // namespace Horo::Editor
