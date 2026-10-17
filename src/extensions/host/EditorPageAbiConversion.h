#pragma once
#include "Horo/Extensions/EditorPageAbi.h"
#include "Horo/Extensions/EditorPageHost.h"

namespace Horo::Extensions::Detail {
    /** @brief Copies bounded inert ABI page data into canonical settings and shared FormKit types without registration. */
    [[nodiscard]] Result<EditorPageDefinition> CopyPageDefinition(const HoroEditorPageDescriptor &descriptor, std::string_view surfaceId,
                                                                  std::string_view titleKey);
    /** @brief Copies exact requested access lists; host admission independently checks every requested permission. */
    [[nodiscard]] Result<void> CopyPagePermissions(const HoroEditorPageDescriptor &descriptor, EditorSurfaceContextDescriptor &context);
    /** @brief Adapts one borrowed callback table; executable lifetime is provided only by the caller's retirement lease. */
    [[nodiscard]] std::shared_ptr<IEditorPageOperations> MakeNativePageOperations(const HoroEditorPageDescriptor &descriptor);
}  // namespace Horo::Extensions::Detail
