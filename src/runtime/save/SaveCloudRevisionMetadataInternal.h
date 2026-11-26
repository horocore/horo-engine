#pragma once

#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

namespace Horo::Runtime::SaveCloudRevisionMetadataDetail {
    /** @brief Carries cloud evidence to a newer live catalog revision without advancing the independent cloud revision. */
    [[nodiscard]] Result<SaveCloudRevisionMetadata> RebindIndex(const SaveSlotIndex &index, const SaveCloudRevisionMetadata &previous);
}
