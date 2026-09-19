#pragma once

#include "ExternalAssetImporter.h"

#include <string>

namespace Horo::Extensions::Detail {
    [[nodiscard]] bool CopyExternalText(HoroExtensionStringView view, std::string &output, bool allowEmpty = false);
    [[nodiscard]] bool ConvertExternalImportSetting(const HoroAssetImportSettingDescriptor &source,
                                                    Assets::ImportSettingDescriptor &output);
}  // namespace Horo::Extensions::Detail
