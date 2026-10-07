#pragma once

#include "Horo/Editor/MixerAssetDocument.h"

namespace Horo::Editor::MixerDocumentInternal {
    /** @brief Stages one semantic operation without publishing intermediate graph state. */
    [[nodiscard]] Result<void> Apply(Audio::MixerAssetSchema &candidate, const MixerDocumentCommand &command,
                                     const Audio::MixerAssetSchemaLimits &limits);
}  // namespace Horo::Editor::MixerDocumentInternal
