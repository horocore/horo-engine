#pragma once
/** @file EditorViewportAbi.h @brief ABI1.7 bounded copied viewport lines and canonical transform handle declarations. */
#include "Horo/Extensions/ExtensionAbi.h"
#define HORO_EDITOR_VIEWPORT_HOST_CAPABILITY "editor.viewport"

/** @brief Native scalar line geometry; normalized logical coordinates, semantic tone 0..4 and axis -1/passive or 0..2. */
struct HoroEditorViewportLine {
    uint32_t structSize;
    float x0, y0, x1, y1, thickness;
    uint32_t tone;
    int32_t axis;
};
#ifndef __cplusplus
typedef struct HoroEditorViewportLine HoroEditorViewportLine;
#endif
/** @brief Borrowed registration-only descriptor, deeply copied before return.
 * @details kind 0 overlay/1 gizmo; tool 0 none/1 move/2 rotate/3 scale; visible 0/1.
 * Gizmos declare editableType horo.scene.transform. No callbacks, document/native handles or renderer resources are accepted.
 */
struct HoroEditorViewportDescriptor {
    uint32_t structSize, schemaVersion, kind;
    HoroExtensionStringView surfaceId, labelKey, editableType;
    uint32_t tool, visible;
    const HoroEditorViewportLine *lines;
    uint32_t lineCount;
    const HoroExtensionStringView *permissions;
    uint32_t permissionCount;
};
#ifndef __cplusplus
typedef struct HoroEditorViewportDescriptor HoroEditorViewportDescriptor;
#endif
/** @brief Checks all prefixes and paired bounds before deep-copy traversal.
 * @param value Borrowed native descriptor; declared nonnull storage must cover its extents. @return Nonzero for structural admission.
 */
static inline int HoroEditorViewportDescriptorShape(const HoroEditorViewportDescriptor *value) {
    if (!value || value->structSize < sizeof(*value) || value->schemaVersion != 1 || value->kind > 1 || value->tool > 3 ||
        value->visible > 1 || value->lineCount == 0 || value->lineCount > 32 || !value->lines || value->permissionCount > 32 ||
        (value->permissionCount && !value->permissions))
        return 0;
    for (uint32_t i = 0; i < value->lineCount; ++i)
        if (value->lines[i].structSize < sizeof(HoroEditorViewportLine) || value->lines[i].tone > 4 || value->lines[i].axis < -1 ||
            value->lines[i].axis > 2)
            return 0;
    return 1;
}
