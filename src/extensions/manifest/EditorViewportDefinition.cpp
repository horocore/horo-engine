#include "Horo/Extensions/EditorViewportHost.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <cmath>

namespace Horo::Extensions {
    /** @copydoc ValidateEditorViewportDefinition */
    Result<void> ValidateEditorViewportDefinition(const EditorSurfaceDescriptor &surface, const EditorViewportDefinition &definition) {
        const bool gizmo = surface.kind == EditorSurfaceKind::Gizmo;
        const auto reject = [] {
            return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
        };
        if ((!gizmo && surface.kind != EditorSurfaceKind::ViewportOverlay) || definition.lines.empty() || definition.lines.size() > 32 ||
            definition.tool > EditorViewportTool::Scale || surface.placement.kind != EditorSurfacePlacementKind::Viewport ||
            surface.persistence != EditorSurfacePersistence::None || surface.openByDefault)
            return reject();
        if (gizmo ? (definition.editableType != "horo.scene.transform" || definition.tool == EditorViewportTool::None)
                  : (!definition.editableType.empty() || definition.tool != EditorViewportTool::None))
            return reject();
        for (const auto &line : definition.lines) {
            const auto coordinate = [](float value) {
                return std::isfinite(value) && value >= 0 && value <= 1;
            };
            if (!coordinate(line.x0) || !coordinate(line.y0) || !coordinate(line.x1) || !coordinate(line.y1) ||
                !std::isfinite(line.thickness) || line.thickness < 1 || line.thickness > 4 || line.tone > EditorViewportTone::Z ||
                (gizmo ? (line.axis < 0 || line.axis > 2) : line.axis != -1) || (line.x0 == line.x1 && line.y0 == line.y1))
                return reject();
        }
        return ValidateEditorSurfaceDescriptor(surface);
    }
}  // namespace Horo::Extensions
