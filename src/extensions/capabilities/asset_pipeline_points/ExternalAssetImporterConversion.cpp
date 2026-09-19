#include "ExternalAssetImporterConversion.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Extensions::Detail {
    namespace {
        constexpr std::uint32_t kMaxTextBytes = 4096;
        constexpr std::uint32_t kMaxListEntries = 256;

        [[nodiscard]] bool ConvertValue(const HoroAssetImportSettingValue &source, Assets::ImportSettingValue &destination) {
            using enum Assets::ImportSettingKind;
            switch (source.kind) {
                case HORO_ASSET_IMPORT_SETTING_BOOLEAN:
                    destination = source.booleanValue != 0;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_INTEGER:
                    destination = source.integerValue;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_FLOAT:
                    destination = source.floatValue;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_TEXT: {
                    std::string value;
                    if (!CopyExternalText(source.textValue, value, true))
                        return false;
                    destination = std::move(value);
                    return true;
                }
                case HORO_ASSET_IMPORT_SETTING_CHOICE:
                    if (source.choiceIndex > std::numeric_limits<std::size_t>::max())
                        return false;
                    destination = static_cast<std::size_t>(source.choiceIndex);
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool ConvertKind(const HoroAssetImportSettingKind source, Assets::ImportSettingKind &destination) {
            using enum Assets::ImportSettingKind;
            switch (source) {
                case HORO_ASSET_IMPORT_SETTING_BOOLEAN:
                    destination = Boolean;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_INTEGER:
                    destination = Integer;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_FLOAT:
                    destination = Float;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_TEXT:
                    destination = Text;
                    return true;
                case HORO_ASSET_IMPORT_SETTING_CHOICE:
                    destination = Choice;
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool ConvertHeader(const HoroAssetImportSettingDescriptor &source, Assets::ImportSettingDescriptor &output) {
            const bool validIdentity = CopyExternalText(source.id, output.id) && CopyExternalText(source.labelKey, output.labelKey) &&
                                       CopyExternalText(source.descriptionKey, output.descriptionKey, true);
            const bool validValue = ConvertKind(source.kind, output.kind) && ConvertValue(source.defaultValue, output.defaultValue);
            const bool validChoices = source.choiceCount <= kMaxListEntries && (source.choices != nullptr || source.choiceCount == 0);
            if (!validIdentity || !validValue || !validChoices)
                return false;
            if (source.hasMinimum != 0)
                output.minimum = source.minimum;
            if (source.hasMaximum != 0)
                output.maximum = source.maximum;
            output.includeInPresets = source.includeInPresets != 0;
            return true;
        }

        [[nodiscard]] bool ConvertChoices(const HoroAssetImportSettingDescriptor &source, Assets::ImportSettingDescriptor &output) {
            output.choices.reserve(source.choiceCount);
            for (std::uint32_t index = 0; index < source.choiceCount; ++index) {
                Assets::ImportSettingChoice choice;
                if (!CopyExternalText(source.choices[index].id, choice.id) ||
                    !CopyExternalText(source.choices[index].labelKey, choice.labelKey) ||
                    !ConvertValue(source.choices[index].value, choice.value))
                    return false;
                output.choices.push_back(std::move(choice));
            }
            return true;
        }
    }  // namespace

    bool CopyExternalText(const HoroExtensionStringView view, std::string &output, const bool allowEmpty) {
        if (view.length > kMaxTextBytes || (view.data == nullptr && view.length != 0) || (!allowEmpty && view.length == 0))
            return false;
        output.assign(view.data != nullptr ? view.data : "", view.length);
        return true;
    }

    bool ConvertExternalImportSetting(const HoroAssetImportSettingDescriptor &source, Assets::ImportSettingDescriptor &output) {
        return ConvertHeader(source, output) && ConvertChoices(source, output);
    }
}  // namespace Horo::Extensions::Detail
